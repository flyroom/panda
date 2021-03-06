// This plugin logs the first N calls to any given function of a given asid.
// Then extracts synthetic informations about the buffers each function uses

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "panda/plugin.h"
#include <iostream>
#include <sstream>
#include <string>
#include <fstream>
#include <map>
#include <set>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <stdio.h>
#include <json.hpp>
#include <cctype>
#include <regex>
#include <capstone/capstone.h>
#include "callstack_instr/callstack_instr.h"
#include "EntropyCalculator.hpp"

extern "C" {
#include "callstack_instr/callstack_instr_ext.h"
bool init_plugin(void *);
void uninit_plugin(void *);
}

using namespace std;
using InstrCnt = uint64_t;
using Asid = target_ulong;
using Address = target_ulong;
using Funid = std::pair<Asid, Address>;
using Callid = InstrCnt;

static std::ofstream outfstream;


// Stores informations about a called function.
struct CallInfo {
    std::map<Address, uint8_t> writeset;
    std::map<Address, uint8_t> readset;
    std::map<Address, int> block_executions;
    Asid asid;
    target_ulong program_counter;
    uint64_t instruction_count_call;
    uint64_t instruction_count_ret;
    int writes = 0;
    int reads = 0;
    uint64_t called_by = 0;
    uint64_t caller_addr = 0;
    std::vector<Address> callstack;
    uint64_t instructions_tot = 0;
    uint64_t instructions_arith = 0;
    uint64_t instructions_mov = 0;
};

/* Data structures */
static std::set<Asid> tracked_asids;
static std::map<Funid, int> n_calls;
static std::unordered_map<Callid, CallInfo> call_infos;

static int calls_to_monitor_per_fn_entrypoint = 5;
static bool record_memory_accesses = true;
static uint64_t target_end_count = 0;


// Gets the current (topmost) stack entry from callstack_instr
CallstackStackEntry getCurrentEntry(CPUState *cpu){
    CallstackStackEntry entry = {};
    get_call_entries(&entry, 1,cpu);
    return entry;
}

/* Memory access logging */

/** When a call instruciton is detected, create a new CallInfo */
void on_call(CPUState *cpu, target_ulong entrypoint, uint64_t callid){
    const Asid asid = panda_current_asid(cpu);

    if (panda_in_kernel(cpu) || !tracked_asids.count(asid)){
        return;
    }

    // Get the latest entries on the current stack.
    // The topmost will be this call

    std::vector<CallstackStackEntry> entries(16);
    int n_entries = get_call_entries(entries.data(),
                                     static_cast<int>(entries.size()), cpu);
    entries.resize(n_entries);

    const CallstackStackEntry& current_entry = entries[0];


    assert(current_entry.call_id);


    // Count the number of calls with this asid we've seen
    // ignore if we've seen enough calls of this function
    Funid fnid = make_pair(asid, entrypoint);
    n_calls[fnid]++;
    if(n_calls[fnid] > calls_to_monitor_per_fn_entrypoint) {
        return;
    }

    // create a call_info for this call
    // successive memory callbacks will populate its writeset and readset
    call_infos[callid].asid = asid;
    call_infos[callid].program_counter = entrypoint;
    call_infos[callid].instruction_count_call = callid;

    if(n_entries > 1){
        const CallstackStackEntry& previous_entry = entries[1];
        call_infos[callid].called_by = previous_entry.call_id;

        auto callstackit = entries.begin();
        callstackit++;
        for (; callstackit!= entries.end(); callstackit++){
            call_infos[callid].callstack.push_back(callstackit->function);
        }
    }
}


/** On write, add to the writeset (if CallInfo exists) */
int mem_write_callback(CPUState *cpu, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    (void) pc;
    uint8_t *data = static_cast<uint8_t*>(buf);

    if (panda_in_kernel(cpu) || !tracked_asids.count(panda_current_asid(cpu))){
        return 0;
    }

    CallstackStackEntry entry = getCurrentEntry(cpu);
    if (!entry.call_id){
        return 0;
    }

    if(call_infos.count(entry.call_id)){
        for(target_ulong i=0; i < size; i++){
            call_infos[entry.call_id].writes++;
            call_infos[entry.call_id].writeset[addr +i] = data[i];
        }
    }

    return 0;
}


/** On read, add to the readset (if CallInfo exists) */
int mem_read_callback(CPUState *cpu, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    (void) pc;
    uint8_t *data = static_cast<uint8_t*>(buf);

    if (panda_in_kernel(cpu) || !tracked_asids.count(panda_current_asid(cpu))){
        return 0;
    }

    CallstackStackEntry entry = getCurrentEntry(cpu);
    if (!entry.call_id){
        return 0;
    }

    if( call_infos.count(entry.call_id)){
        for(target_ulong i=0; i < size; i++){
            call_infos[entry.call_id].reads++;
            //if this address hasn't been previously written by the current
            //function, then add it to the readset
            if(!call_infos[entry.call_id].writeset.count(addr + i)){
                call_infos[entry.call_id].readset[addr +i] = data[i];
            }
        }
    }
    
    return 0;
}


/* Data access processing */

/* Holds a synthetic description of a given buffer */
struct bufferinfo {
    target_ulong base = 0;
    target_ulong len = 0;
    float entropy = -1;
    int printableChars = 0;
    int nulls = 0;

    std::string toString() const {
        std::stringstream ss;
        ss << reinterpret_cast<void*>(base) << "+" << len << ":" << entropy << ";";
        return ss.str();
    }

    nlohmann::json toJSON() const{
        nlohmann::json ret;
        ret["base"] = base;
        ret["len"] = len;
        ret["entropy"] = entropy;
        ret["printableChars"] = printableChars;
        ret["nulls"] = nulls;
        return ret;
    }
};

/* Computes a vector of BufferInfo from a map<address,data> (read/writeset) */
std::vector<bufferinfo> toBufferInfos(std::map<target_ulong, uint8_t>& addrset){
    std::vector<bufferinfo> res;
    bufferinfo temp;
    EntropyCalculator ec;

    for (const auto& addr_data : addrset) {
        const auto& addr = addr_data.first;
        const auto& data = addr_data.second;

        if(addr != temp.base + temp.len){
            // start new bufferr

            // save old buffer (if there's one)
            if(temp.base){
                temp.entropy = ec.get();
                res.push_back(temp);
            }

            // init new buffer
            temp = {};
            temp.base = addr;
            ec.reset();
        }

        // add byte to current buffer
        temp.len++;
        if(std::isprint(data)){
            temp.printableChars++;
        }
        if(data == 0){
            temp.nulls++;
        }
        ec.add(data);

    }

    // process last buffer (if any - ie addrset wasn't empty)
    if(temp.base){
        //save
        temp.entropy = ec.get();
        res.push_back(temp);
    }

    return res;
}

/* On return, if the CallInfo exists, log it to stdout, then erase it */
void on_ret(CPUState *cpu, target_ulong entrypoint, uint64_t callid, uint32_t skipped_frames){
    (void) entrypoint;

    if (panda_in_kernel(cpu) || !tracked_asids.count(panda_current_asid(cpu))){
        return;
    }

    if(call_infos.count(callid) == 0){
        // we haven't logged this call, probably because it's been called more 
        // than N times
        return;
    }
    
    auto& call = call_infos[callid];

    if(skipped_frames){
        std::cerr << "Warning, skipped frames: " << skipped_frames << std::endl;
    }

    std::vector<bufferinfo> writebuffs = toBufferInfos(call.writeset);
    std::vector<bufferinfo> readbuffs = toBufferInfos(call.readset);

    hwaddr physPC = panda_virt_to_phys(cpu, call.program_counter);

    nlohmann::json out;

    std::stringstream line;
    out["asid"] = call.asid;
    out["pc"] = call.program_counter;
    out["phisical"] = physPC;
    out["called_by"] = call.called_by;
    out["id"] = call.instruction_count_call;
    out["insn_arith"] = call.instructions_arith;
    out["insn_total"] = call.instructions_tot;
    out["insn_movs"] = call.instructions_mov;

    auto writes = nlohmann::json::array();
    for(const bufferinfo& wrb : writebuffs){
        writes.push_back(wrb.toJSON());
    }

    auto reads = nlohmann::json::array();
    for(const bufferinfo& rdb : readbuffs){
        reads.push_back(rdb.toJSON());
    }

    auto callstack = nlohmann::json::array();
    for(const auto& addr : call.callstack){
        callstack.push_back(addr);
    }

    out["writes"] = writes;
    out["reads"] = reads;
    out["callstack"] = callstack;

    int maxexecs = 0;
    int sumexecs = 0;
    int distinct = 0;
    for(const auto& block_exec : call.block_executions){
        //const auto& pc = block_exec.first;
        const auto& exec = block_exec.second;

        if(exec > maxexecs)
            maxexecs = exec;

        sumexecs += exec;
        distinct++;
    }

    out["maxexecs"] = maxexecs;
    out["sumexecs"] = sumexecs;
    out["distinct_blocks"] = distinct;
    out["nreads"] = call.reads;
    out["nwrites"] = call.writes;

    //cout << out.dump() << std::endl;
    outfstream << out.dump() << std::endl;

    call_infos.erase(callid);
}


/* Plugin initialization */

std::set<target_ulong> parse_addr_list(const char* addrs){
    std::set<target_ulong> res;
    if(!addrs) return res;

    char* arrt = strdup(addrs);

    char* pch = strtok(arrt, " ,;_");
    while (pch != NULL){
        res.insert(static_cast<target_ulong>(std::stoul(pch, nullptr, 0)));
        pch = strtok(NULL, " ,;_");
    }

    free(arrt);
    return res;
}


static const std::regex ArithMnemonicRegex{R"(add|adc|sub|xor|shr|shl|div|mul|rol|ror|dec)"};
static const std::regex MovMnemonicRegex{R"(mov|lea)"};
class ArithOpsCounter {
public:
    uint32_t arith = 0;
    uint32_t total = 0;
    uint32_t movs = 0;

    void fromMnemonic(const char* mnemonic){
        total++;
        bool is_arith = std::regex_search(mnemonic, ArithMnemonicRegex);
        if(is_arith) arith++;
        bool is_mov = std::regex_search(mnemonic, MovMnemonicRegex);
        if(is_mov) movs++;
    }
};
static std::unordered_map<target_ulong, ArithOpsCounter> arithcounters;

static inline uint32_t tb_idx(CPUState* cpu, TranslationBlock *tb){
    (void) cpu;
    return tb->pc;
}

int after_block_exec(CPUState* cpu, TranslationBlock *tb) {
    if (panda_in_kernel(cpu) || !tracked_asids.count(panda_current_asid(cpu))){
        return 0;
    }

    CallstackStackEntry entry = getCurrentEntry(cpu);
    if (!entry.call_id || !call_infos.count(entry.call_id)){
        return 0;
    }

    auto& call = call_infos[entry.call_id];
    call.block_executions[tb->pc]++;

    auto instr_count = rr_get_guest_instr_count();
    if(target_end_count && instr_count > target_end_count){
        rr_end_replay_requested = 1;
    }

    auto& counter = arithcounters[tb_idx(cpu,tb)];
    call.instructions_arith += counter.arith;
    call.instructions_tot += counter.total;
    call.instructions_mov += counter.movs;
    return 0;
}

void fnmemlogger_on_tb_translated_disass(CPUState *cpu, TranslationBlock *tb,
                                         size_t handle, void *first_,
                                         size_t count){
    ArithOpsCounter aoc;
    cs_insn* insn = reinterpret_cast<cs_insn*>(first_);
    for(size_t i =0 ; i < count; i++){
        aoc.fromMnemonic(insn[i].mnemonic);
    }

    arithcounters[tb_idx(cpu, tb)] = aoc;
}

void *plugin_self;
bool init_plugin(void *self) {
    plugin_self = self;
 
    panda_require("callstack_instr");
    if (!init_callstack_instr_api()) return false;
    
    panda_cb pcb;

    //pcb.asid_changed = asid_changed;
    //panda_register_callback(self, PANDA_CB_ASID_CHANGED, pcb);

    pcb.virt_mem_after_write = mem_write_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_WRITE, pcb);
    pcb.virt_mem_after_read = mem_read_callback;

    panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_READ, pcb);
    pcb.after_block_exec = after_block_exec;

    panda_register_callback(self, PANDA_CB_AFTER_BLOCK_EXEC, pcb);

    PPP_REG_CB("callstack_instr", on_call2, on_call);
    PPP_REG_CB("callstack_instr", on_ret2, on_ret);
    PPP_REG_CB("callstack_instr", on_tb_translated_disass, fnmemlogger_on_tb_translated_disass);


    panda_disable_tb_chaining();  //TODO actually needed?
    panda_enable_memcb();

    panda_arg_list *args = panda_get_args("fn_memlogger");
    if (args != NULL) {
        const char* asidss = panda_parse_string(args, "asids", "list of address space identifiers to track");
        tracked_asids = parse_addr_list(asidss);
        target_end_count = panda_parse_uint64_opt(args, "endat", 0, "instruction count when to end the replay");
        calls_to_monitor_per_fn_entrypoint = static_cast<int>(panda_parse_uint32_opt(args, "call_limit", 5, "how many calls to monitor per entrypoint"));
        record_memory_accesses = !panda_parse_bool_opt(args, "no_memory_accesses", "don't record reads and writes");
    }

    for(const target_ulong asid: tracked_asids){
        printf("tracking asid " TARGET_FMT_ld  " \n", asid);
    }

    outfstream.open("fn_memlogger");
    if(outfstream.fail()){
        return false;
    }

    return true;
}

/** Logs to the standard out the calls that haven't returned.
 * Note these calls haven't been logged */
void printstats(){
    std::cerr << "Missed calls: " << std::endl;
    for(auto const& callid_call : call_infos){
        auto& call = callid_call.second;
        std::cerr << "MISSED_RETURN_OF_FN " << call.program_counter << " AT_TIME " << callid_call.first << std::endl;
    }

}

void uninit_plugin(void *self) {
    (void) self;
    outfstream.flush();
    outfstream.close();
    printstats();
}
