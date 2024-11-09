#include <riscv/simif.h>
#include <riscv/processor.h>
#include <riscv/log_file.h>
#include <fesvr/context.h>
#include <fesvr/htif.h>
#include <fesvr/memif.h>
#include <fesvr/elfloader.h>
#include <map>
#include <sstream>
#include <vpi_user.h>
#include <svdpi.h>

/* Includes for rocc support */
#include <riscv/extension.h>
#include <riscv/rocc.h>
#include <random>
#include <limits>
#include <riscv/mmu.h>
#include <riscv/trap.h>
#include <stdexcept>
#include <iostream>
#include <assert.h>
#include <math.h>

#if __has_include("spiketile_tsi.h")
#define SPIKETILE_HTIF_TSI
extern std::map<int, htif_t*> tsis;
#endif
#if __has_include("spiketile_dtm.h")
#define SPIKETILE_HTIF_DTM
extern htif_t* dtm;
#endif

enum transfer_t {
  NToB,
  NToT,
  BToT
};

enum cache_state_t {
  NONE,
  BRANCH,
  TRUNK,
  DIRTY
};

struct cache_line_t {
  cache_state_t state;
  uint64_t addr;
  uint64_t data[8];
};

struct mem_region_t {
  uint64_t base;
  uint64_t size;
};

struct stq_entry_t {
  uint64_t addr;
  uint64_t bytes;
  size_t len;
};

struct cache_miss_t {
  bool valid;
  uint64_t addr;
  size_t way;
  transfer_t type;
};

struct writeback_t {
  cache_line_t line;
  cache_state_t desired;
  uint64_t sourceid;
  bool voluntary;
};

struct rocc_mem_req_t {
  long long int addr;
  int tag;
  int cmd;
  int size;
  bool phys;
  long long int data;
  int mask;
};

struct rocc_mem_resp_t {
  long long int addr;
  int tag;
  int cmd;
  int size;
  long long int data = 0;
  unsigned char replay = 0;
  unsigned char has_data = 0;
  long long int word_bypass = 0;
  long long int store_data = 0;
  int mask;
};

class chipyard_simif_t : public simif_t
{
public:
  char* addr_to_mem(reg_t addr) override { return NULL; };
  bool reservable(reg_t addr) override;
  bool mmio_fetch(reg_t addr, size_t len, uint8_t* bytes) override;
  bool mmio_load(reg_t addr, size_t len, uint8_t* bytes) override;
  bool mmio_store(reg_t addr, size_t len, const uint8_t* bytes) override;
  void proc_reset(unsigned id) override { };
  const char* get_symbol(uint64_t addr) override { return nullptr; };

  void handle_fence();

  bool icache_a(uint64_t *address, uint64_t *source);
  void icache_d(uint64_t sourceid, uint64_t data[8]);

  bool mmio_a(uint64_t *address, uint64_t* data, unsigned char* store, int* size);
  void mmio_d(uint64_t data);

  bool dcache_a(uint64_t *address, uint64_t* source, unsigned char* state_old, unsigned char* state_new);
  void dcache_b(uint64_t address, uint64_t source, int param);
  bool dcache_c(uint64_t *address, uint64_t* source, int* param, unsigned char* voluntary, unsigned char* has_data, uint64_t* data[8]);
  void dcache_d(uint64_t sourceid, uint64_t data[8], unsigned char has_data, unsigned char grantack);

  void tcm_a(uint64_t address, uint64_t data, uint32_t mask, uint32_t opcode, uint32_t size);
  bool tcm_d(uint64_t *data);

  bool rocc_handshake(rocc_insn_t *insn, reg_t* rs1, reg_t* rs2);
  void push_rocc_insn(rocc_insn_t insn, reg_t rs1, reg_t rs2);
  void push_rocc_result(long long int result);
  long long int get_rocc_result();

  void push_rocc_mem_request(long long int address, int tag, int cmd, int size, bool phys, long long int data, int mask);
  void push_rocc_mem_response(rocc_mem_resp_t data);
  bool rocc_mem_response_handshake(long long int* addr, int* tag, int* cmd, int* size, long long int* data, unsigned char* replay, unsigned char* has_data, long long int* word_bypass, long long int* store_data, int* mask);
  void handle_rocc_mem_request(processor_t* proc);

  long long int translate_rocc_mem_addr(processor_t* proc, long long int addr);

  void set_rocc_exists(bool exists);
  bool get_rocc_exists();

  void set_rocc_busy(bool busy);

  void loadmem(size_t base, const char* fname);

  void drain_stq();
  bool stq_empty() { return st_q.size() == 0; };
  void flush_icache();

  const cfg_t &get_cfg() const { return cfg; }
  const std::map<size_t, processor_t*>& get_harts() const { return harts; }

  ~chipyard_simif_t() { };
  chipyard_simif_t(size_t icache_ways,
                   size_t icache_sets,
                   size_t dcache_ways,
                   size_t dcache_sets,
                   char* cacheable,
                   char* uncacheable,
                   char* readonly_uncacheable,
                   char* executable,
                   size_t icache_sourceids,
                   size_t dcache_sourceids,
                   size_t tcm_base,
                   size_t tcm_size,
                   const char* isastr,
                   size_t pmpregions);
  uint64_t cycle;
  bool use_stq;
  htif_t *htif;
  bool fast_clint;
  cfg_t cfg;
  std::map<size_t, processor_t*> harts;
  bool accessed_tofrom_host;
private:
  bool handle_cache_access(reg_t addr, size_t len,
                           uint8_t* load_bytes,
                           const uint8_t* store_bytes,
                           access_type type);
  void handle_mmio_access(reg_t addr, size_t len,
                          uint8_t* load_bytes,
                          const uint8_t* store_bytes,
                          access_type type,
                          bool readonly);

  size_t icache_ways;
  size_t icache_sets;
  size_t dcache_ways;
  size_t dcache_sets;

  std::vector<mem_region_t> cacheables;
  std::vector<mem_region_t> uncacheables;
  std::vector<mem_region_t> readonly_uncacheables;
  std::vector<mem_region_t> executables;

  std::vector<std::vector<cache_line_t>> dcache;
  std::vector<std::vector<cache_line_t>> icache;
  std::vector<size_t> icache_sourceids;
  std::vector<size_t> dcache_a_sourceids;
  std::vector<size_t> dcache_c_sourceids;
  std::vector<size_t> dcache_mmio_sourceids;

  std::vector<cache_miss_t> dcache_miss_q;
  std::vector<cache_miss_t> icache_miss_q;
  std::vector<cache_miss_t> icache_inflight;
  std::vector<cache_miss_t> dcache_inflight;
  std::vector<writeback_t> wb_q;
  std::vector<stq_entry_t> st_q;

  std::vector<rocc_insn_t> rocc_insn_q;
  std::vector<long long int> rocc_result_q;
  std::vector<reg_t> rocc_rs1_q;
  std::vector<reg_t> rocc_rs2_q;
  bool rocc_exists;

  std::vector<rocc_mem_req_t> rocc_mem_request_q;
  std::vector<rocc_mem_resp_t> rocc_mem_response_q;

  bool rocc_busy;
  std::map<std::pair<uint64_t, size_t>, uint64_t> readonly_cache;

  bool mmio_valid;
  bool mmio_inflight;
  uint64_t mmio_addr;
  bool mmio_st;
  uint64_t mmio_stdata;
  size_t mmio_len;
  uint64_t mmio_lddata;

  uint64_t tcm_base;
  uint64_t tcm_size;
  uint8_t* tcm;
  std::vector<uint64_t> tcm_q;
};

class tile_t {
public:
  tile_t(processor_t* p, chipyard_simif_t* s);
  processor_t* proc;
  chipyard_simif_t* simif;
  size_t max_insns;
  context_t spike_context;
  context_t stq_context;
};

/* Begin RoCC header file */
class generic_t : public extension_t
{
  public:
    generic_t(chipyard_simif_t* s) {
      simif = s;
    }

    const char* name() { return "generic" ; } 

    reg_t custom0(rocc_insn_t insn, reg_t xs1, reg_t xs2);
    reg_t custom1(rocc_insn_t insn, reg_t xs1, reg_t xs2);
    reg_t custom2(rocc_insn_t insn, reg_t xs1, reg_t xs2);
    reg_t custom3(rocc_insn_t insn, reg_t xs1, reg_t xs2);

    virtual std::vector<insn_desc_t> get_instructions();
    virtual std::vector<disasm_insn_t*> get_disasms();
    
    void reset() {};

   protected:
   chipyard_simif_t* simif; 
};
/* End RoCC header file */

context_t *host;
std::map<int, tile_t*> tiles;
std::ostream sout(nullptr);
log_file_t* log_file;
#define DEFAULT_PRIV_ "MSU"

extern "C" void spike_tile_reset(int hartid)
{
  if (tiles.find(hartid) != tiles.end()) {
    tiles[hartid]->proc->reset();
  }
}

extern "C" void spike_tile(int hartid, char* isa,
                           int pmpregions,
                           int icache_sets, int icache_ways,
                           int dcache_sets, int dcache_ways,
                           char* cacheable, char* uncacheable, char* readonly_uncacheable, char* executable,
                           int icache_sourceids, int dcache_sourceids,
                           long long int tcm_base, long long int tcm_size,
                           long long int reset_vector,
                           long long int ipc,
                           long long int cycle,
                           long long int* insns_retired,
                           unsigned char has_rocc,

                           char debug,
                           char mtip, char msip, char meip,
                           char seip,

                           unsigned char icache_a_ready,
                           unsigned char* icache_a_valid,
                           long long int* icache_a_address,
                           long long int* icache_a_sourceid,

                           unsigned char icache_d_valid,
                           long long int icache_d_sourceid,
                           long long int icache_d_data_0,
                           long long int icache_d_data_1,
                           long long int icache_d_data_2,
                           long long int icache_d_data_3,
                           long long int icache_d_data_4,
                           long long int icache_d_data_5,
                           long long int icache_d_data_6,
                           long long int icache_d_data_7,

                           unsigned char dcache_a_ready,
                           unsigned char* dcache_a_valid,
                           long long int* dcache_a_address,
                           long long int* dcache_a_sourceid,
                           unsigned char* dcache_a_state_old,
                           unsigned char* dcache_a_state_new,

                           unsigned char dcache_b_valid,
                           long long int dcache_b_address,
                           long long int dcache_b_source,
                           int dcache_b_param,

                           unsigned char dcache_c_ready,
                           unsigned char* dcache_c_valid,
                           long long int* dcache_c_address,
                           long long int* dcache_c_source,
                           int* dcache_c_param,
                           unsigned char* dcache_c_voluntary,
                           unsigned char* dcache_c_has_data,
                           long long int* dcache_c_data_0,
                           long long int* dcache_c_data_1,
                           long long int* dcache_c_data_2,
                           long long int* dcache_c_data_3,
                           long long int* dcache_c_data_4,
                           long long int* dcache_c_data_5,
                           long long int* dcache_c_data_6,
                           long long int* dcache_c_data_7,

                           unsigned char dcache_d_valid,
                           unsigned char dcache_d_has_data,
                           unsigned char dcache_d_grantack,
                           long long int dcache_d_sourceid,
                           long long int dcache_d_data_0,
                           long long int dcache_d_data_1,
                           long long int dcache_d_data_2,
                           long long int dcache_d_data_3,
                           long long int dcache_d_data_4,
                           long long int dcache_d_data_5,
                           long long int dcache_d_data_6,
                           long long int dcache_d_data_7,

                           unsigned char mmio_a_ready,
                           unsigned char* mmio_a_valid,
                           long long int* mmio_a_address,
                           long long int* mmio_a_data,
                           unsigned char* mmio_a_store,
                           int* mmio_a_size,

                           unsigned char mmio_d_valid,
                           long long int mmio_d_data,

                           unsigned char tcm_a_valid,
                           long long int tcm_a_address,
                           long long int tcm_a_data,
                           int tcm_a_mask,
                           int tcm_a_opcode,
                           int tcm_a_size,

                           unsigned char* tcm_d_valid,
                           unsigned char tcm_d_ready,
                           long long int* tcm_d_data,

                           unsigned char rocc_request_ready,
                           unsigned char* rocc_request_valid,
                           int* rocc_request_insn,
                           int* rocc_request_rs1,
                           int* rocc_request_rs2,
                           unsigned char rocc_response_valid,
                           long long int rocc_response_rd,
                           long long int rocc_response_data,
                           unsigned char rocc_busy,

                           unsigned char rocc_mem_request_valid,
                           long long int rocc_mem_request_addr,
                           int rocc_mem_request_tag,
                           int rocc_mem_request_cmd,
                           int rocc_mem_request_size,
                           unsigned char rocc_mem_request_phys,
                           long long int rocc_mem_request_data,
                           int rocc_mem_request_mask,
                           
                           unsigned char* rocc_mem_response_valid,
                           long long int* rocc_mem_response_addr,
                           int* rocc_mem_response_tag,
                           int* rocc_mem_response_cmd,
                           int* rocc_mem_response_size,
                           long long int* rocc_mem_response_data,
                           unsigned char* rocc_mem_response_replay,
                           unsigned char* rocc_mem_response_has_data,
                           long long int* rocc_mem_response_word_bypass,
                           long long int* rocc_mem_response_store_data,
                           int* rocc_mem_response_mask

                          //  unsigned char rocc_ptw_request_valid,
                          //  long long int rocc_ptw_request_addr,
                          //  unsigned char rocc_ptw_request_need_gpa,
                          //  unsigned char rocc_ptw_request_vstage1,
                          //  unsigned char rocc_ptw_request_stage2,

                          //  unsigned char* rocc_ptw_response_valid,
                          //  long long int* rocc_ptw_response_addr
                           )
{
  if (!host) {
    host = context_t::current();
    sout.rdbuf(std::cerr.rdbuf());
    log_file = new log_file_t(nullptr);
  }
  if (tiles.find(hartid) == tiles.end()) {
    printf("Constructing spike processor_t\n");
    isa_parser_t *isa_parser = new isa_parser_t(isa, DEFAULT_PRIV_);
    std::string* isastr = new std::string(isa);
    chipyard_simif_t* simif = new chipyard_simif_t(icache_ways, icache_sets,
                                                   dcache_ways, dcache_sets,
                                                   cacheable, uncacheable, readonly_uncacheable, executable,
                                                   icache_sourceids, dcache_sourceids,
                                                   tcm_base, tcm_size,
                                                   isastr->c_str(), pmpregions);
    processor_t* p = new processor_t(isa,
                                     DEFAULT_PRIV_,
                                     &simif->get_cfg(),
                                     simif,
                                     hartid,
                                     false,
                                     log_file->get(),
                                     sout);
    simif->harts[hartid] = p;

    std::function<extension_t*()> extension;
    generic_t* my_generic_extension = new generic_t(simif);
    p->register_extension(my_generic_extension);
    simif->set_rocc_exists(has_rocc);

    s_vpi_vlog_info vinfo;
    if (!vpi_get_vlog_info(&vinfo))
      abort();
    std::string loadmem_file = "";
    for (int i = 1; i < vinfo.argc; i++) {
      std::string arg(vinfo.argv[i]);
      if (arg == "+spike-debug") {
        p->set_debug(true);
      }
      if (arg == "+spike-stq") {
        simif->use_stq = true;
      }
      if (arg.find("+loadmem=") == 0) {
        loadmem_file = arg.substr(strlen("+loadmem="));
      }
      if (arg == "+spike-fast-clint") {
        simif->fast_clint = true;
      }
      if (arg == "+spike-verbose") {
        p->enable_log_commits();
      }
    }
    if (loadmem_file != "" && tcm_size > 0)
      simif->loadmem(tcm_base, loadmem_file.c_str());

    p->reset();
    p->get_state()->pc = reset_vector;
    tiles[hartid] = new tile_t(p, simif);
    printf("Done constructing spike processor\n");
  }
  tile_t* tile = tiles[hartid];
  chipyard_simif_t* simif = tile->simif;
  processor_t* proc = tile->proc;
#if defined(SPIKETILE_HTIF_TSI)
  if (!simif->htif && tsis.size() > 0 && tsis[0])
    simif->htif = tsis[0];
#endif
#if defined(SPIKETILE_HTIF_DTM)
  if (!simif->htif && dtm)
    simif->htif = dtm;
#endif

  simif->cycle = cycle;
  if (debug) {
    proc->halt_request = proc->HR_REGULAR;
  }
  if (!debug && proc->halt_request != proc->HR_NONE) {
    proc->halt_request = proc->HR_NONE;
  }

  proc->get_state()->mip->backdoor_write_with_mask(MIP_MTIP, mtip ? MIP_MTIP : 0);
  proc->get_state()->mip->backdoor_write_with_mask(MIP_MSIP, msip ? MIP_MSIP : 0);
  proc->get_state()->mip->backdoor_write_with_mask(MIP_MEIP, meip ? MIP_MEIP : 0);
  proc->get_state()->mip->backdoor_write_with_mask(MIP_SEIP, seip ? MIP_SEIP : 0);

  tile->max_insns = ipc;
  uint64_t pre_insns = proc->get_state()->minstret->read();
  simif->accessed_tofrom_host = false;
  tile->spike_context.switch_to();
  *insns_retired = proc->get_state()->minstret->read() - pre_insns;
  if (simif->use_stq) {
    tile->stq_context.switch_to();
  }

  *icache_a_valid = 0;
  if (icache_a_ready) {
    *icache_a_valid = simif->icache_a((uint64_t*)icache_a_address,
                                      (uint64_t*)icache_a_sourceid);
  }

  if (icache_d_valid) {
    uint64_t data[8] = {icache_d_data_0, icache_d_data_1, icache_d_data_2, icache_d_data_3,
                        icache_d_data_4, icache_d_data_5, icache_d_data_6, icache_d_data_7};
    simif->icache_d(icache_d_sourceid, data);
  }

  *dcache_a_valid = 0;
  if (dcache_a_ready) {
    *dcache_a_valid = simif->dcache_a((uint64_t*)dcache_a_address,
                                      (uint64_t*)dcache_a_sourceid,
                                      dcache_a_state_old, dcache_a_state_new);
  }
  if (dcache_b_valid) {
    simif->dcache_b(dcache_b_address, dcache_b_source, dcache_b_param);
  }
  *dcache_c_valid = 0;
  if (dcache_c_ready) {
    uint64_t* data[8] = {(uint64_t*)dcache_c_data_0, (uint64_t*)dcache_c_data_1, (uint64_t*)dcache_c_data_2, (uint64_t*)dcache_c_data_3,
                         (uint64_t*)dcache_c_data_4, (uint64_t*)dcache_c_data_5, (uint64_t*)dcache_c_data_6, (uint64_t*)dcache_c_data_7};
    *dcache_c_valid = simif->dcache_c((uint64_t*)dcache_c_address, (uint64_t*)dcache_c_source, (int*)dcache_c_param,
                                      dcache_c_voluntary, dcache_c_has_data, data);
  }
  if (dcache_d_valid) {
    uint64_t data[8] = {dcache_d_data_0, dcache_d_data_1, dcache_d_data_2, dcache_d_data_3,
                        dcache_d_data_4, dcache_d_data_5, dcache_d_data_6, dcache_d_data_7};
    simif->dcache_d(dcache_d_sourceid, data, dcache_d_has_data, dcache_d_grantack);
  }

  *mmio_a_valid = 0;
  if (mmio_a_ready) {
    *mmio_a_valid = simif->mmio_a((uint64_t*)mmio_a_address, (uint64_t*) mmio_a_data,
                                  mmio_a_store, mmio_a_size);
  }
  if (mmio_d_valid) {
    simif->mmio_d(mmio_d_data);
  }

  if (tcm_a_valid) {
    simif->tcm_a(tcm_a_address, tcm_a_data, tcm_a_mask, tcm_a_opcode, tcm_a_size);
  }
  if (tcm_d_ready) {
    *tcm_d_valid = simif->tcm_d((uint64_t*)tcm_d_data);
  }

  *rocc_request_valid = 0;
  if (rocc_request_ready) {
    *rocc_request_valid = simif->rocc_handshake((rocc_insn_t*) rocc_request_insn, (reg_t*) rocc_request_rs1, (reg_t*) rocc_request_rs2);
  }

  if (rocc_response_valid) {
    simif->push_rocc_result(rocc_response_data);
  }

  if (rocc_mem_request_valid) {
    simif->push_rocc_mem_request(rocc_mem_request_addr, rocc_mem_request_tag, rocc_mem_request_cmd, rocc_mem_request_size, rocc_mem_request_phys, rocc_mem_request_data, rocc_mem_request_mask);
  }

  simif->handle_rocc_mem_request(proc);
  *rocc_mem_response_valid = simif->rocc_mem_response_handshake(rocc_mem_response_addr, rocc_mem_response_tag, rocc_mem_response_cmd, rocc_mem_response_size, rocc_mem_response_data, rocc_mem_response_replay, rocc_mem_response_has_data, rocc_mem_response_word_bypass, rocc_mem_response_store_data, rocc_mem_response_mask);

  simif->set_rocc_busy(rocc_busy);
}

/* Begin RoCC Section */
reg_t generic_t::custom0(rocc_insn_t insn, reg_t xs1, reg_t xs2) {
  bool has_rocc = simif->get_rocc_exists();
  if (!has_rocc) {
    printf("Accelerator not instantiated, are you using the right config?\n");
    exit(1);
  } else {
    simif->push_rocc_insn(insn, xs1, xs2);
    if (insn.xd) {
      return simif->get_rocc_result();
    } else {
      return 0;
    }
  }
}

reg_t generic_t::custom1(rocc_insn_t insn, reg_t xs1, reg_t xs2) {
  bool has_rocc = simif->get_rocc_exists();
  if (!has_rocc) {
    printf("Accelerator not instantiated, are you using the right config?\n");
    exit(1);
  } else {
    simif->push_rocc_insn(insn, xs1, xs2);
    if (insn.xd) {
      return simif->get_rocc_result();
    } else {
      return 0;
    }
  }
}

reg_t generic_t::custom2(rocc_insn_t insn, reg_t xs1, reg_t xs2) {
  bool has_rocc = simif->get_rocc_exists();
  if (!has_rocc) {
    printf("Accelerator not instantiated, are you using the right config?\n");
    exit(1);
  } else {
    simif->push_rocc_insn(insn, xs1, xs2);
    if (insn.xd) {
      return simif->get_rocc_result();
    } else {
      return 0;
    }
  }
}

reg_t generic_t::custom3(rocc_insn_t insn, reg_t xs1, reg_t xs2) {
  bool has_rocc = simif->get_rocc_exists();
  if (!has_rocc) {
    printf("Accelerator not instantiated, are you using the right config?\n");
    exit(1);
  } else {
    simif->push_rocc_insn(insn, xs1, xs2);
    if (insn.xd) {
      return simif->get_rocc_result();
    } else {
      return 0;
    }
  }
}

define_custom_func(generic_t, "generic", generic_custom0, custom0);
define_custom_func(generic_t, "generic", generic_custom1, custom1);
define_custom_func(generic_t, "generic", generic_custom2, custom2);
define_custom_func(generic_t, "generic", generic_custom3, custom3);

std::vector<insn_desc_t> generic_t::get_instructions()
{
  std::vector<insn_desc_t> insns;
  push_custom_insn(insns, ROCC_OPCODE0, ROCC_OPCODE_MASK, ILLEGAL_INSN_FUNC, generic_custom0);
  push_custom_insn(insns, ROCC_OPCODE1, ROCC_OPCODE_MASK, ILLEGAL_INSN_FUNC, generic_custom1);
  push_custom_insn(insns, ROCC_OPCODE2, ROCC_OPCODE_MASK, ILLEGAL_INSN_FUNC, generic_custom2);
  push_custom_insn(insns, ROCC_OPCODE3, ROCC_OPCODE_MASK, ILLEGAL_INSN_FUNC, generic_custom3);
  return insns;
}

std::vector<disasm_insn_t*> generic_t::get_disasms()
{
  std::vector<disasm_insn_t*> insns;
  return insns;
}
/*End RoCC Section*/

chipyard_simif_t::chipyard_simif_t(size_t icache_ways,
                                   size_t icache_sets,
                                   size_t dcache_ways,
                                   size_t dcache_sets,
                                   char* cacheable,
                                   char* uncacheable,
                                   char* readonly_uncacheable,
                                   char* executable,
                                   size_t ic_sourceids,
                                   size_t dc_sourceids,
                                   size_t tcm_base,
                                   size_t tcm_size,
                                   const char* isastr,
                                   size_t pmpregions
                                   ) :
  cycle(0),
  use_stq(false),
  htif(nullptr),
  fast_clint(false),
  accessed_tofrom_host(false),
  icache_ways(icache_ways),
  icache_sets(icache_sets),
  dcache_ways(dcache_ways),
  dcache_sets(dcache_sets),
  tcm_base(tcm_base),
  tcm_size(tcm_size),
  mmio_valid(false),
  mmio_inflight(false)
{

  cfg.initrd_bounds = std::make_pair(0, 0);
  cfg.bootargs = nullptr;
  cfg.isa = isastr;
  cfg.priv = DEFAULT_PRIV_;
  cfg.misaligned = false;
  cfg.endianness = endianness_little;
  cfg.pmpregions = pmpregions;
  cfg.mem_layout = std::vector<mem_cfg_t>();
  cfg.hartids = std::vector<size_t>();
  cfg.explicit_hartids = false;
  cfg.trigger_count = 0;

  icache.resize(icache_ways);
  for (auto &w : icache) {
    w.resize(icache_sets);
    for (size_t i = 0; i < icache_sets; i++) w[i].state = NONE;
  }

  dcache.resize(dcache_ways);
  for (auto &w : dcache) {
    w.resize(dcache_sets);
    for (size_t i = 0; i < dcache_sets; i++) w[i].state = NONE;
  }
  for (int i = 0; i < ic_sourceids; i++) {
    icache_sourceids.push_back(i);
    icache_inflight.push_back(cache_miss_t { 0, 0, 0, NToB });
  }
  for (int i = 0; i < dc_sourceids; i++) {
    dcache_a_sourceids.push_back(i);
    dcache_c_sourceids.push_back(i);
    dcache_inflight.push_back(cache_miss_t { 0, 0, 0, NToB });
  }

  std::stringstream css(cacheable);
  std::stringstream uss(uncacheable);
  std::stringstream rss(readonly_uncacheable);
  std::stringstream xss(executable);
  std::string base;
  std::string size;
  while (css >> base) {
    css >> size;
    uint64_t base_int = std::stoul(base);
    uint64_t size_int = std::stoul(size);
    cacheables.push_back(mem_region_t { base_int, size_int });
  }
  while (uss >> base) {
    uss >> size;
    uint64_t base_int = std::stoul(base);
    uint64_t size_int = std::stoul(size);
    uncacheables.push_back(mem_region_t { base_int, size_int });
  }
  while (rss >> base) {
    rss >> size;
    uint64_t base_int = std::stoul(base);
    uint64_t size_int = std::stoul(size);
    readonly_uncacheables.push_back(mem_region_t { base_int, size_int });
  }
  while (xss >> base) {
    xss >> size;
    uint64_t base_int = std::stoul(base);
    uint64_t size_int = std::stoul(size);
    executables.push_back(mem_region_t { base_int, size_int });
  }

  tcm = (uint8_t*)malloc(tcm_size);
}

void chipyard_simif_t::flush_icache() {
 for (auto &w : icache) {
    for (size_t i = 0; i < icache_sets; i++) w[i].state = NONE;
  }
}

bool chipyard_simif_t::reservable(reg_t addr) {
  for (auto& r: cacheables) {
    if (addr >= r.base && addr < r.base + r.size) {
      return true;
    }
  }
  if (addr >= tcm_base && addr < tcm_base + tcm_size) {
    return true;
  }
  return false;
}

bool chipyard_simif_t::mmio_fetch(reg_t addr, size_t len, uint8_t* bytes) {
  bool executable = false;

  if (addr >= tcm_base && addr < tcm_base + tcm_size) {
    memcpy(bytes, tcm + addr - tcm_base, len);
    return true;
  }

  for (auto& r: executables) {
    if (addr >= r.base && addr + len <= r.base + r.size) {
      executable = true;
      break;
    }
  }
  if (!executable) {
    return false;
  }

  while (!handle_cache_access(addr, len, bytes, nullptr, FETCH)) {
    host->switch_to();
  }
  return true;
}

bool chipyard_simif_t::mmio_load(reg_t addr, size_t len, uint8_t* bytes) {
  bool found = false;
  bool cacheable = false;
  bool readonly = false;
  reg_t tohost_addr = htif ? htif->get_tohost_addr() : 0;
  reg_t fromhost_addr = htif ? htif->get_fromhost_addr() : 0;
  if (addr == tohost_addr || addr == fromhost_addr) {
    accessed_tofrom_host = true;
  }

  if (addr >= tcm_base && addr < tcm_base + tcm_size) {
    memcpy(bytes, tcm + addr - tcm_base, len);
    return true;
  }
  for (auto& r: cacheables) {
    if (addr >= r.base && addr + len <= r.base + r.size) {
      cacheable = true;
      found = true;
      break;
    }
  }
  if (!found) {
    for (auto& r: uncacheables) {
      if (addr >= r.base && addr + len <= r.base + r.size) {
        cacheable = false;
        found = true;
        break;
      }
    }
    for (auto& r: readonly_uncacheables) {
      if (addr >= r.base && addr + len <= r.base + r.size) {
        readonly = true;
        break;
      }
    }
  }

  if (!found) {
    return false;
  }

  if (cacheable) {
    while (!handle_cache_access(addr, len, bytes, nullptr, LOAD)) {
      host->switch_to();
    }
    uint64_t lddata = 0;
    memcpy(&lddata, bytes, len);
  } else {
    handle_mmio_access(addr, len, bytes, nullptr, LOAD, readonly);
  }

  return true;
}

void chipyard_simif_t::handle_mmio_access(reg_t addr, size_t len,
                                          uint8_t* load_bytes,
                                          const uint8_t* store_bytes,
                                          access_type type,
                                          bool readonly) {
  if (type == LOAD && readonly) {
    auto it = readonly_cache.find(std::make_pair(addr, len));
    if (it != readonly_cache.end()) {
      memcpy(load_bytes, &(it->second), len);
      return;
    }
  }

  mmio_valid = true;
  mmio_inflight = false;
  mmio_addr = addr;
  mmio_st = type == STORE;
  if (type == STORE) {
    assert(len <= 8);
    mmio_stdata = 0;
    memcpy(&mmio_stdata, store_bytes, len);
  }
  mmio_len = len;

  while (mmio_valid) {
    host->switch_to();
  }
  if (type == LOAD) {
    memcpy(load_bytes , &mmio_lddata, len);
  }
  if (type == LOAD && readonly) {
    readonly_cache[std::make_pair(addr, len)] = mmio_lddata;
  }
}

bool chipyard_simif_t::handle_cache_access(reg_t addr, size_t len,
                                           uint8_t* load_bytes,
                                           const uint8_t* store_bytes,
                                           access_type type) {
  uint64_t stdata = 0;
  if (type == STORE) {
    assert(len <= 8);
    memcpy(&stdata, store_bytes, len);
  }

  // no stores to icache
  std::vector<std::vector<cache_line_t>> *cache = &icache;
  std::vector<cache_miss_t> *missq = &icache_miss_q;
  std::vector<cache_miss_t> *inflight = &icache_inflight;
  size_t n_sets = icache_sets;
  size_t n_ways = icache_ways;
  if (type != FETCH) {
    cache = &dcache;
    missq = &dcache_miss_q;
    inflight = &dcache_inflight;
    n_sets = dcache_sets;
    n_ways = dcache_ways;
  }
  if (type == LOAD) {
    for (auto& s : st_q) {
      if (addr == s.addr && len < s.len) {
        // Forwarding
        memcpy(load_bytes, &(s.bytes), len);
        return true;
      }
      if (addr < s.addr && addr + len > s.addr) {
        return false;
      }
      if (s.addr < addr && s.addr + s.len > addr) {
        return false;
      }
    }
  }

#define SETIDX(ADDR) ((ADDR >> 6) & (n_sets - 1))
  uint64_t setidx = SETIDX(addr);
  uint64_t offset = addr & (64 - 1);
  bool cache_hit = false;
  size_t hit_way = 0;
  for (int i = 0; i < n_ways; i++) {
    bool addr_match = ((*cache)[i][setidx].addr >> 6) == (addr >> 6);
    if (addr_match && (*cache)[i][setidx].state != NONE) {
      assert(!cache_hit);
      cache_hit = true;
      hit_way = i;
    }
  }

  if (type != STORE) {
    if (cache_hit) {
      memcpy(load_bytes, (uint8_t*)((*cache)[hit_way][setidx].data) + offset, len);
      return true;
    }
  } else {
    for (int i = 0; i < icache_ways; i++) {
      if ((icache[i][setidx].addr >> 6) == addr >> 6) {
        icache[i][setidx].state = NONE;
      }
    }
    if (cache_hit && dcache[hit_way][setidx].state != BRANCH) {
      dcache[hit_way][setidx].state = DIRTY;
      memcpy((uint8_t*)(dcache[hit_way][setidx].data) + offset, store_bytes, len);
      return true;
    }
  }

  for (auto& e : wb_q) {
    cache_line_t& cl = e.line;
    if (cl.addr >> 6 == addr >> 6) {
      return false;
    }
  }

  for (cache_miss_t& cl : *missq) {
    if (cl.addr >> 6 == addr >> 6) {
      return false;
    }
  }

  for (cache_miss_t& cl : *inflight) {
    if (cl.addr >> 6 == addr >> 6 && cl.valid) {
      return false;
    }
  }


  size_t repl_way = rand() % n_ways;
  transfer_t upgrade;
  size_t upgrade_way;
  bool do_repl;
  if (type == STORE) {
    if (cache_hit && (*cache)[hit_way][setidx].state != NONE) {
      upgrade = BToT;
      upgrade_way = hit_way;
      do_repl = false;
    } else {
      upgrade = NToT;
      upgrade_way = repl_way;
      do_repl = true;
    }
  } else {
    upgrade = NToB;
    upgrade_way = repl_way;
    do_repl = true;
  }
  if (do_repl) {
    for (auto& e : *missq) {
      if (SETIDX(e.addr) == setidx) {
        return false;
      }
    }
    for (auto& e : *inflight) {
      if (e.valid && SETIDX(e.addr) == setidx) {
        return false;
      }
    }
  }

  missq->push_back(cache_miss_t { true, addr, upgrade_way, upgrade });

  cache_line_t repl_cl = (*cache)[repl_way][setidx];
  if (do_repl) {
    if (repl_cl.state == DIRTY) {
      wb_q.push_back(writeback_t { repl_cl, NONE, 0, true});
    }
    (*cache)[repl_way][setidx].state = NONE;
  }
  (*cache)[upgrade_way][setidx].state = NONE;

  return false;
}

bool chipyard_simif_t::icache_a(uint64_t* address, uint64_t* sourceid) {
  if (icache_miss_q.empty() || icache_sourceids.empty()) {
    return false;
  }
  *sourceid = icache_sourceids[0];
  *address = (icache_miss_q[0].addr >> 6) << 6;

  icache_inflight[icache_sourceids[0]] = icache_miss_q[0];

  icache_sourceids.erase(icache_sourceids.begin());
  icache_miss_q.erase(icache_miss_q.begin());

  return true;
}

void chipyard_simif_t::icache_d(uint64_t sourceid, uint64_t data[8]) {
  cache_miss_t& miss = icache_inflight[sourceid];
  uint64_t setidx = (miss.addr >> 6) & (icache_sets - 1);
  icache_inflight[sourceid].valid = false;
  icache[miss.way][setidx].state = BRANCH;
  icache[miss.way][setidx].addr = miss.addr;
  memcpy(icache[miss.way][setidx].data, (void*)data, 64);
  icache_sourceids.push_back(sourceid);
}

bool chipyard_simif_t::mmio_a(uint64_t* address, uint64_t* data, unsigned char* store, int* size) {
  if (!mmio_valid || mmio_inflight) {
    return false;
  }
  mmio_inflight = true;
  *address = mmio_addr;
  *store = mmio_st;
  *data = mmio_stdata;
  *size = mmio_len;
  return true;
}

void chipyard_simif_t::mmio_d(uint64_t data) {
  mmio_valid = false;
  mmio_inflight = false;
  size_t offset = mmio_addr & 7;
  mmio_lddata = data >> (offset * 8);
}

bool chipyard_simif_t::dcache_a(uint64_t *address, uint64_t* source, unsigned char* state_old, unsigned char* state_new) {
  if (dcache_miss_q.empty() || dcache_a_sourceids.empty()) {
    return false;
  }
  *source = dcache_a_sourceids[0];
  *address = (dcache_miss_q[0].addr >> 6) << 6;
  switch (dcache_miss_q[0].type) {
  case NToB:
    *state_old = 0;
    *state_new = 0;
    break;
  case NToT:
    *state_old = 0;
    *state_new = 1;
    break;
  case BToT:
    *state_old = 1;
    *state_new = 1;
    break;
  }

  dcache_inflight[dcache_a_sourceids[0]] = dcache_miss_q[0];
  dcache_a_sourceids.erase(dcache_a_sourceids.begin());
  dcache_miss_q.erase(dcache_miss_q.begin());
  return true;
}

void chipyard_simif_t::dcache_b(uint64_t address, uint64_t source, int param) {
  uint64_t setidx = (address >> 6) & (dcache_sets - 1);
  uint64_t offset = address & (64 - 1);
  bool cache_hit = false;
  size_t hit_way = 0;
  for (int i = 0; i < dcache_ways; i++) {
    bool addr_match = dcache[i][setidx].addr >> 6 == address >> 6;
    if (addr_match && dcache[i][setidx].state != NONE) {
      cache_hit = true;
      hit_way = i;
    }
  }
  cache_state_t desired;
  switch (param) {
  case 0:
    desired = TRUNK;
    break;
  case 1:
    desired = BRANCH;
    break;
  case 2:
    desired = NONE;
    break;
  }
  if (!cache_hit) {
    cache_line_t miss { NONE, address, {} };
    wb_q.push_back(writeback_t { miss, desired, source, false});
  } else {
    wb_q.push_back(writeback_t { dcache[hit_way][setidx], desired, source, false});
    if (desired == TRUNK && dcache[hit_way][setidx].state == BRANCH) {
      dcache[hit_way][setidx].state = BRANCH;
    } else {
      dcache[hit_way][setidx].state = desired;
    }

  }
}

bool chipyard_simif_t::dcache_c(uint64_t* address, uint64_t* source, int* param, unsigned char* voluntary,
                                unsigned char* has_data,
                                uint64_t* data[8]) {
  if (wb_q.empty())
    return false;

  writeback_t& wb = wb_q[0];
  if (wb.voluntary && dcache_c_sourceids.empty())
    return false;

  *address = (wb.line.addr >> 6) << 6;
  *source = wb.sourceid;
  *voluntary = wb.voluntary;
  if (wb.voluntary) {
    *source = dcache_c_sourceids[0];
    dcache_c_sourceids.erase(dcache_c_sourceids.begin());
  }

#define SHRINK(_desired, _state, _has_data, _param)       \
  if (wb.line.state == _state && wb.desired == _desired) { \
    *has_data = _has_data; \
    *param = _param; \
  }

  SHRINK(TRUNK  , DIRTY  , true , 3);
  SHRINK(TRUNK  , TRUNK  , false, 3);
  SHRINK(TRUNK  , BRANCH , false, 4);
  SHRINK(TRUNK  , NONE   , false, 5);
  SHRINK(BRANCH , DIRTY  , true , 0);
  SHRINK(BRANCH , TRUNK  , false, 0);
  SHRINK(BRANCH , BRANCH , false, 4);
  SHRINK(BRANCH , NONE   , false, 5);
  SHRINK(NONE   , DIRTY  , true , 1);
  SHRINK(NONE   , TRUNK  , false, 1);
  SHRINK(NONE   , BRANCH , false, 2);
  SHRINK(NONE   , NONE   , false, 5);

  for (int i = 0; i < 8; i++) {
    *(data[i]) = wb.line.data[i];
  }
  wb_q.erase(wb_q.begin());
  return true;
}

bool chipyard_simif_t::mmio_store(reg_t addr, size_t len, const uint8_t* bytes) {
  reg_t tohost_addr = htif ? htif->get_tohost_addr() : 0;
  reg_t fromhost_addr = htif ? htif->get_fromhost_addr() : 0;

  if (addr == tohost_addr || addr == fromhost_addr) {
    accessed_tofrom_host = true;
  }

  if (addr >= tcm_base && addr < tcm_base + tcm_size) {
    memcpy(tcm + addr - tcm_base, bytes, len);
    return true;
  }

  bool found = false;
  bool cacheable = false;
  for (auto& r: cacheables) {
    if (addr >= r.base && addr + len <= r.base + r.size) {
      cacheable = true;
      found = true;
      break;
    }
  }
  for (auto& r: uncacheables) {
    if (addr >= r.base && addr + len <= r.base + r.size) {
      cacheable = false;
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  if (cacheable) {
    uint64_t temp = 0;
    memcpy(&temp, bytes, len);
    if (use_stq) {
      assert(len <= 8);
      uint64_t stdata;
      memcpy(&stdata, bytes, len);
      st_q.push_back(stq_entry_t { addr, stdata, len });
    } else {
      while (!handle_cache_access(addr, len, nullptr, bytes, STORE)) {
        host->switch_to();
      }
    }
  } else {
    handle_mmio_access(addr, len, nullptr, bytes, STORE, false);
  }

  return true;
}

void chipyard_simif_t::drain_stq() {
  while (true) {
    while (st_q.size() == 0) {
      host->switch_to();
    }
    stq_entry_t store = st_q[0];
    while (!handle_cache_access(store.addr, store.len, nullptr, (uint8_t*)(&(store.bytes)), STORE)) {
      host->switch_to();
    }
    st_q.erase(st_q.begin());
  }
}

void chipyard_simif_t::dcache_d(uint64_t sourceid, uint64_t data[8], unsigned char has_data, unsigned char grantack) {
  if (grantack) {
    cache_miss_t& miss = dcache_inflight[sourceid];
    uint64_t setidx = (miss.addr >> 6) & (dcache_sets - 1);
    if (has_data) {
      memcpy(dcache[miss.way][setidx].data, (void*)data, 64);
    }
    dcache_inflight[sourceid].valid = false;
    if (miss.type == NToB) {
      dcache[miss.way][setidx].state = BRANCH;
    } else {
      dcache[miss.way][setidx].state = TRUNK;
    }
    dcache[miss.way][setidx].addr = miss.addr;
    dcache_a_sourceids.push_back(sourceid);
  } else {
    dcache_c_sourceids.push_back(sourceid);
  }
}

void chipyard_simif_t::tcm_a(uint64_t address, uint64_t data, uint32_t mask, uint32_t opcode, uint32_t size) {
  bool load = opcode == 4;
  uint64_t rdata = 0;
  memcpy(&rdata, tcm + address - tcm_base, 8);
  tcm_q.push_back(rdata);

  if (!load) {
    for (size_t i = 0; i < 8; i++) {
      if ((mask >> i) & 1) {
        memcpy(tcm + address - tcm_base + i, ((uint8_t*)&data) + i, 1);
      }
    }
  }
}

bool chipyard_simif_t::tcm_d(uint64_t* data) {
  if (tcm_q.size() == 0)
    return false;
  *data = tcm_q[0];
  tcm_q.erase(tcm_q.begin());
  return true;
}

bool chipyard_simif_t::rocc_handshake(rocc_insn_t* insn, reg_t* rs1, reg_t* rs2) {
  if (rocc_insn_q.empty()) {
    return false;
  }
  *insn = rocc_insn_q[0];
  *rs1 = rocc_rs1_q[0];
  *rs2 = rocc_rs2_q[0];
  
  rocc_insn_q.erase(rocc_insn_q.begin());
  rocc_rs1_q.erase(rocc_rs1_q.begin());
  rocc_rs2_q.erase(rocc_rs2_q.begin());
  return true;
}

void chipyard_simif_t::push_rocc_insn(rocc_insn_t insn, reg_t rs1, reg_t rs2) {
  rocc_insn_q.push_back(insn);
  rocc_rs1_q.push_back(rs1);
  rocc_rs2_q.push_back(rs2);
  
  host->switch_to();
}

void chipyard_simif_t::push_rocc_result(long long int result) {
  rocc_result_q.push_back(result);
}

long long int chipyard_simif_t::get_rocc_result() {
  while (rocc_result_q.size() == 0) {
    host->switch_to();
  }

  if (rocc_result_q.size() == 0) {
    return 0;
  }
  long long int result = rocc_result_q.front();
  rocc_result_q.erase(rocc_result_q.begin());
  return result;
}

void chipyard_simif_t::push_rocc_mem_request(long long int address, int tag, int cmd, int size, bool phys, long long int data, int mask) {
  rocc_mem_request_q.push_back({address, tag, cmd, size, phys, data, mask});
  // printf("Pushed rocc mem request: %llx %d %d %d %d %llx %d\n", address, tag, cmd, size, phys, data, mask);
}

void chipyard_simif_t::push_rocc_mem_response(struct rocc_mem_resp_t result) {
  rocc_mem_response_q.push_back(result);
  // printf("Pushed rocc mem response: %llx %d %d %d %llx %d %d %llx %llx %d\n", result.addr, result.tag, result.cmd, result.size, result.data, result.replay, result.has_data, result.word_bypass, result.store_data, result.mask);
}

bool chipyard_simif_t::rocc_mem_response_handshake(long long int* addr, int* tag, int* cmd, int* size, long long int* data, unsigned char* replay, unsigned char* has_data, long long int* word_bypass, long long int* store_data, int* mask) {
  if (!rocc_mem_response_q.empty()) {
    struct rocc_mem_resp_t resp_data = rocc_mem_response_q[0];
    rocc_mem_response_q.erase(rocc_mem_response_q.begin());
    *addr = resp_data.addr;
    *tag = resp_data.tag;
    *cmd = resp_data.cmd;
    *size = resp_data.size;
    *data = resp_data.data;
    *replay = resp_data.replay;
    *has_data = resp_data.has_data;
    *word_bypass = resp_data.word_bypass;
    *store_data = resp_data.store_data;
    *mask = resp_data.mask;
    return true;
  } else {
    return false;
  }
}

void chipyard_simif_t::handle_rocc_mem_request(processor_t* proc) {
  if (!rocc_mem_request_q.empty()) {
    rocc_mem_req_t request = rocc_mem_request_q[0];
    rocc_mem_resp_t response;

    rocc_mem_request_q.erase(rocc_mem_request_q.begin());

    response.addr = request.addr;
    response.tag = request.tag;
    response.cmd = request.cmd;
    response.size = request.size;
    response.mask = request.mask;

    if (!request.phys) {
      request.addr = proc->translate(request.addr, request.cmd); 
    }
    
    if (request.cmd == 0) {
      // Read req
      response.has_data = 1;
      mmio_load((reg_t) request.addr, (size_t) (1 << request.size), (uint8_t*) &response.data);
    } else if (request.cmd == 1) {
      // Write req
      response.store_data = request.data;
      mmio_store((reg_t) request.addr, (size_t) request.size, (uint8_t*) &request.data);
    } else {
      puts("MEM OPERATION UNDEFINED");
      exit(-1);
    }
    
    push_rocc_mem_response(response);
  }
}

long long int chipyard_simif_t::translate_rocc_mem_addr(processor_t* proc, long long int addr) {
  //TODO
  //return proc->translate(addr, );
}

void chipyard_simif_t::set_rocc_exists(bool exists) {
  rocc_exists = exists;
}

bool chipyard_simif_t::get_rocc_exists() {
  return rocc_exists;
}

void chipyard_simif_t::set_rocc_busy(bool busy) {
  rocc_busy = busy;
}

void chipyard_simif_t::loadmem(size_t base, const char* fname) {
  class loadmem_memif_t : public memif_t {
  public:
    loadmem_memif_t(chipyard_simif_t* _simif, size_t _start) : memif_t(nullptr), simif(_simif), start(_start) {}
    void write(addr_t taddr, size_t len, const void* src) override
    {
      addr_t addr = taddr - start;
      memcpy(simif->tcm + addr, src, len);
    }
    void read(addr_t taddr, size_t len, void* bytes) override {
      assert(false);
    }
    endianness_t get_target_endianness() const override {
      return endianness_little;
    }
  private:
    chipyard_simif_t* simif;
    size_t start;
  } loadmem_memif(this, tcm_base);

  reg_t entry;
  load_elf(fname, &loadmem_memif, &entry, 0);
}

void chipyard_simif_t::handle_fence() {
  while(rocc_busy || !stq_empty()) {
    host->switch_to();
  }
}

bool insn_should_fence(uint64_t bits) {
  uint8_t opcode = bits & 0x7f;
  return opcode == 0b0101111 || opcode == 0b0001111;
}

bool insn_is_wfi(uint64_t bits) {
  return bits == 0x10500073;
}

void spike_thread_main(void* arg)
{
  tile_t* tile = (tile_t*) arg;
  processor_t* proc = tile->proc;
  chipyard_simif_t* simif = tile->simif;
  state_t* state = proc->get_state();
  while (true) {
    while (tile->max_insns == 0) {
      host->switch_to();
    }
    while (tile->max_insns != 0) {
      // TODO: Fences don't work
      //uint64_t last_bits = proc->get_last_bits();
      // if (insn_should_fence(last_bits) && !simif->stq_empty()) {
      //   host->switch_to();
      // }
      uint64_t old_minstret = state->minstret->read();
      proc->step(1);
      tile->max_insns--;

      if (insn_should_fence(proc->get_last_inst())) { //If Instruction is fence, tick the RTL
        simif->handle_fence();
      }

      if (proc->is_waiting_for_interrupt()) {
        if (simif->fast_clint) {
          state->mip->backdoor_write_with_mask(MIP_MTIP, MIP_MTIP);
          tile->max_insns = tile->max_insns <= 1 ? 0 : 1;
        } else {
          tile->max_insns = 0;
        }
      }
      if (state->debug_mode) {
        // TODO: Fix. This needs to apply the same hack as rocket-chip...
        // JALRs in debug mode should flush the ICache.
        // There is no API to determine if a JALR was executed, so hack the
        // pc of the JALR in the debug rom here instead.
        if (state->pc == 0x838) {
          simif->flush_icache();
        }
      }

      // If we get stuck in WFI, or we start polling tohost/fromhost, switch to host thread
      if ((old_minstret == state->minstret->read()) || simif->accessed_tofrom_host) {
        tile->max_insns = 0;
      }
      state->mcycle->write(simif->cycle);
    }
  }
}

void stq_thread_main(void* arg)
{
  tile_t* tile = (tile_t*) arg;
  tile->simif->drain_stq();
}

tile_t::tile_t(processor_t* p, chipyard_simif_t* s) : proc(p), simif(s), max_insns(0) {
  spike_context.init(spike_thread_main, this);
  stq_context.init(stq_thread_main, this);
}
