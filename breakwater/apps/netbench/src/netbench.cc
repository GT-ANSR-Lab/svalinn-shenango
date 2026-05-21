extern "C" {
#include <base/log.h>
#include <base/time.h>
#include <net/ip.h>
#include <unistd.h>
#include <fcntl.h>
#include <breakwater/breakwater.h>
#include <breakwater/seda.h>
#include <breakwater/dagor.h>
#include <breakwater/nocontrol.h>
#include <breakwater/sync.h>
}

#include "cc/net.h"
#include "cc/runtime.h"
#include "cc/sync.h"
#include "cc/thread.h"
#include "cc/timer.h"
#include "breakwater/rpc++.h"

#include "synthetic_worker.h"
#include "loadbalancer.h"
#include "fanouter.h"
#include "m_semaphore.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <ctime>
std::time_t timex;

barrier_t barrier;

constexpr uint16_t kBarrierPort = 41;

const struct crpc_ops *crpc_ops;
const struct srpc_ops *srpc_ops;

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;

typedef enum {
	CPU_BOUND = 0,
	MEM_BOUND,
	LOCK_BOUND,
	NUM_REQUEST_TYPES
} RequestType;

// <- ARGUMENTS FOR EXPERIMENT ->
// the number of worker threads to spawn.
int threads;
// the remote UDP address of the server.
int num_servers;
int nconn[16];
netaddr raddr[16];
netaddr master;
// number of work iterations for cpu-bound request
uint64_t cpu_bound_work_itr;
// number of work iterations for memory-bound request
uint64_t mem_bound_work_itr;
// number of work iterations for lock-bound request
uint64_t lock_bound_work_itr;
// Request type mix
double req_mix[NUM_REQUEST_TYPES] = {};
// RPC service level objective (in us)
int slo;
// Whether to use memory semaphore
bool use_msem;

std::ofstream json_out;
std::ofstream csv_out;

int total_agents = 1;
// number of iterations required for 1us on target server
constexpr uint64_t kIterationsPerUS = 69;  // 83
// Total duration of the experiment in us
constexpr uint64_t kWarmUpTime = 3000000;
constexpr uint64_t kExperimentTime = 8000000;
// RTT
constexpr uint64_t kRTT = 10;
constexpr uint64_t kNumDupClient = 32;

std::vector<double> offered_loads;
double offered_load;

// Load shift load arrays
int do_load_shift = 0;
struct load_shift_test {
  double rate;
  uint64_t duration;
  uint64_t mem_bound_work_itr;
};

std::vector<load_shift_test> load_shift_tests = {
  // This is for warmup
  {.rate = 1000000,
   .duration = kWarmUpTime,
   .mem_bound_work_itr = 25},

  // Actual rates, we want to test
  {.rate = 1000000,
   .duration = 2000000,
   .mem_bound_work_itr = 25},
  {.rate = 1000000,
   .duration = 2000000,
   .mem_bound_work_itr = 500},
  {.rate = 1000000,
   .duration = 2000000,
   .mem_bound_work_itr = 25},
 };


static SyntheticWorker *cpu_bound_workers[NCPU];
static SyntheticWorker *mem_bound_workers[NCPU];
static SyntheticWorker *lock_bound_workers[NCPU];
static MemSemaphore *msem = nullptr;
static cong_aware_mutex_t mut;

struct payload {
  uint64_t req_type;
  uint64_t work_itr;
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t server_queue;
  uint64_t hash;
  uint64_t work_us;
  uint32_t msem_cap;
};
constexpr int PAYLOAD_ID_OFF = offsetof(payload, index);

rpc::LoadBalancer<payload, PAYLOAD_ID_OFF> *load_balancer[16];
rpc::FanOuter<payload, PAYLOAD_ID_OFF> *fan_outer;

/* server-side stat */
constexpr uint64_t kRPCSStatPort = 8002;
constexpr uint64_t kRPCSStatMagic = 0xDEADBEEF;
struct sstat_raw {
  uint64_t total;
  uint64_t busy;
  uint64_t mem_accesses;
  double energy_consumed;
  unsigned int num_cores;
  unsigned int max_cores;
  uint64_t cupdate_rx;
  uint64_t ecredit_tx;
  uint64_t credit_tx;
  uint64_t req_rx;
  uint64_t req_dropped;
  uint64_t resp_tx;
};

constexpr uint64_t kShenangoStatPort = 40;
constexpr uint64_t kShenangoStatMagic = 0xDEADBEEF;
struct shstat_raw {
  uint64_t rx_pkts;
  uint64_t tx_pkts;
  uint64_t rx_bytes;
  uint64_t tx_bytes;
  uint64_t drops;
  uint64_t rx_tcp_ooo;
};

struct sstat {
  double cpu_usage;
  double membw_usage;
  double power_usage;
  double rx_pps;
  double tx_pps;
  double rx_bps;
  double tx_bps;
  double rx_drops_pps;
  double rx_ooo_pps;
  double cupdate_rx_pps;
  double ecredit_tx_pps;
  double credit_tx_cps;
  double req_rx_pps;
  double req_drop_rate;
  double resp_tx_pps;
};

/* client-side stat */
struct cstat_raw {
  double offered_rps;
  double rps;
  double cpu_bound_req_rps;
  double mem_bound_req_rps;
  double lock_bound_req_rps;
  double goodput;
  double cpu_bound_req_goodput;
  double mem_bound_req_goodput;
  double lock_bound_req_goodput;
  double min_percli_tput;
  double max_percli_tput;
  uint64_t ecredit_rx;
  uint64_t cupdate_tx;
  uint64_t resp_rx;
  uint64_t req_tx;
  uint64_t credit_expired;
  uint64_t req_dropped;
};

struct cstat {
  double offered_rps;
  double rps;
  double cpu_bound_req_rps;
  double mem_bound_req_rps;
  double lock_bound_req_rps;
  double goodput;
  double cpu_bound_req_goodput;
  double mem_bound_req_goodput;
  double lock_bound_req_goodput;
  double min_percli_tput;
  double max_percli_tput;
  double ecredit_rx_pps;
  double cupdate_tx_pps;
  double resp_rx_pps;
  double req_tx_pps;
  double credit_expired_cps;
  double req_dropped_rps;
};

struct work_unit {
  double start_us;
  double duration_us;
  uint64_t req_type;
  uint64_t work_itr;
  int hash;
  bool success;
  bool is_monster;
  uint64_t credit;
  uint64_t tsc;
  uint32_t cpu;
  uint64_t server_queue;
  uint64_t server_time;
  uint64_t timing;
  uint64_t work_us;
  uint32_t msem_cap;
};

class NetBarrier {
 public:
  static constexpr uint64_t npara = 10;
  NetBarrier(int npeers) {
    threads /= total_agents;

    is_leader_ = true;
    std::unique_ptr<rt::TcpQueue> q(
        rt::TcpQueue::Listen({0, kBarrierPort}, 4096));
    aggregator_ = std::move(std::unique_ptr<rt::TcpQueue>(
        rt::TcpQueue::Listen({0, kBarrierPort + 1}, 4096)));
    for (int i = 0; i < npeers; i++) {
      rt::TcpConn *c = q->Accept();
      if (c == nullptr) panic("couldn't accept a connection");
      conns.emplace_back(c);
      BUG_ON(c->WriteFull(&threads, sizeof(threads)) <= 0);
      BUG_ON(c->WriteFull(&total_agents, sizeof(total_agents)) <= 0);
      BUG_ON(c->WriteFull(&cpu_bound_work_itr, sizeof(cpu_bound_work_itr)) <= 0);
      BUG_ON(c->WriteFull(&mem_bound_work_itr, sizeof(mem_bound_work_itr)) <= 0);
      BUG_ON(c->WriteFull(&lock_bound_work_itr, sizeof(lock_bound_work_itr)) <= 0);
      BUG_ON(c->WriteFull(&req_mix[0], sizeof(req_mix)) <= 0);
      BUG_ON(c->WriteFull(&slo, sizeof(slo)) <= 0);
      BUG_ON(c->WriteFull(&offered_load, sizeof(offered_load)) <= 0);
      BUG_ON(c->WriteFull(&do_load_shift, sizeof(do_load_shift)) <= 0);
      BUG_ON(c->WriteFull(&num_servers, sizeof(num_servers)) <= 0);
      BUG_ON(c->WriteFull(raddr, sizeof(netaddr) * num_servers) <= 0);
      BUG_ON(c->WriteFull(nconn, sizeof(int) * num_servers) <= 0);
      for (size_t j = 0; j < npara; j++) {
        rt::TcpConn *c = aggregator_->Accept();
        if (c == nullptr) panic("couldn't accept a connection");
        agg_conns_.emplace_back(c);
      }
    }
  }

  NetBarrier(netaddr leader) {
    auto c = rt::TcpConn::Dial({0, 0}, {leader.ip, kBarrierPort});
    if (c == nullptr) panic("barrier");
    conns.emplace_back(c);
    is_leader_ = false;
    BUG_ON(c->ReadFull(&threads, sizeof(threads)) <= 0);
    BUG_ON(c->ReadFull(&total_agents, sizeof(total_agents)) <= 0);
    BUG_ON(c->ReadFull(&cpu_bound_work_itr, sizeof(cpu_bound_work_itr)) <= 0);
    BUG_ON(c->ReadFull(&mem_bound_work_itr, sizeof(mem_bound_work_itr)) <= 0);
    BUG_ON(c->ReadFull(&lock_bound_work_itr, sizeof(lock_bound_work_itr)) <= 0);
    BUG_ON(c->ReadFull(&req_mix[0], sizeof(req_mix)) <= 0);
    BUG_ON(c->ReadFull(&slo, sizeof(slo)) <= 0);
    BUG_ON(c->ReadFull(&offered_load, sizeof(offered_load)) <= 0);
    BUG_ON(c->ReadFull(&do_load_shift, sizeof(do_load_shift)) <= 0);
    BUG_ON(c->ReadFull(&num_servers, sizeof(num_servers)) <= 0);
    BUG_ON(c->ReadFull(raddr, sizeof(netaddr) * num_servers) <= 0);
    BUG_ON(c->ReadFull(nconn, sizeof(int) * num_servers) <= 0);
    for (size_t i = 0; i < npara; i++) {
      auto c = rt::TcpConn::Dial({0, 0}, {master.ip, kBarrierPort + 1});
      BUG_ON(c == nullptr);
      agg_conns_.emplace_back(c);
    }
  }

  bool Barrier() {
    char buf[1];
    if (is_leader_) {
      for (auto &c : conns) {
        if (c->ReadFull(buf, 1) != 1) return false;
      }
      for (auto &c : conns) {
        if (c->WriteFull(buf, 1) != 1) return false;
      }
    } else {
      if (conns[0]->WriteFull(buf, 1) != 1) return false;
      if (conns[0]->ReadFull(buf, 1) != 1) return false;
    }
    return true;
  }

  bool StartExperiment() { return Barrier(); }

  bool EndExperiment(std::vector<work_unit> &w, struct cstat_raw *csr) {
    if (is_leader_) {
      for (auto &c : conns) {
        struct cstat_raw rem_csr;
        BUG_ON(c->ReadFull(&rem_csr, sizeof(rem_csr)) <= 0);
        csr->offered_rps += rem_csr.offered_rps;
        csr->rps += rem_csr.rps;
        csr->cpu_bound_req_rps += rem_csr.cpu_bound_req_rps;
        csr->mem_bound_req_rps += rem_csr.mem_bound_req_rps;
        csr->lock_bound_req_rps += rem_csr.lock_bound_req_rps;
        csr->goodput += rem_csr.goodput;
        csr->cpu_bound_req_goodput += rem_csr.cpu_bound_req_goodput;
        csr->mem_bound_req_goodput += rem_csr.mem_bound_req_goodput;
        csr->lock_bound_req_goodput += rem_csr.lock_bound_req_goodput;
        csr->min_percli_tput =
            MIN(rem_csr.min_percli_tput, csr->min_percli_tput);
        csr->max_percli_tput =
            MAX(rem_csr.max_percli_tput, csr->max_percli_tput);
        csr->ecredit_rx += rem_csr.ecredit_rx;
        csr->cupdate_tx += rem_csr.cupdate_tx;
        csr->resp_rx += rem_csr.resp_rx;
        csr->req_tx += rem_csr.req_tx;
        csr->credit_expired += rem_csr.credit_expired;
        csr->req_dropped += rem_csr.req_dropped;
      }
    } else {
      BUG_ON(conns[0]->WriteFull(csr, sizeof(*csr)) <= 0);
    }
    GatherSamples(w);
    BUG_ON(!Barrier());
    return is_leader_;
  }

  bool IsLeader() { return is_leader_; }

 private:
  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  std::unique_ptr<rt::TcpQueue> aggregator_;
  std::vector<std::unique_ptr<rt::TcpConn>> agg_conns_;
  bool is_leader_;

  void GatherSamples(std::vector<work_unit> &w) {
    std::vector<rt::Thread> th;
    if (is_leader_) {
      std::unique_ptr<std::vector<work_unit>> samples[agg_conns_.size()];
      for (size_t i = 0; i < agg_conns_.size(); ++i) {
        th.emplace_back(rt::Thread([&, i] {
          size_t nelem;
          BUG_ON(agg_conns_[i]->ReadFull(&nelem, sizeof(nelem)) <= 0);

          if (likely(nelem > 0)) {
            work_unit *wunits = new work_unit[nelem];
            BUG_ON(agg_conns_[i]->ReadFull(wunits, sizeof(work_unit) * nelem) <=
                   0);
            std::vector<work_unit> v(wunits, wunits + nelem);
            delete[] wunits;

            samples[i].reset(new std::vector<work_unit>(std::move(v)));
          } else {
            samples[i].reset(new std::vector<work_unit>());
          }
        }));
      }

      for (auto &t : th) t.Join();
      for (size_t i = 0; i < agg_conns_.size(); ++i) {
        auto &v = *samples[i];
        w.insert(w.end(), v.begin(), v.end());
      }
    } else {
      for (size_t i = 0; i < agg_conns_.size(); ++i) {
        th.emplace_back(rt::Thread([&, i] {
          size_t elems = w.size() / npara;
          work_unit *start = w.data() + elems * i;
          if (i == npara - 1) elems += w.size() % npara;
          BUG_ON(agg_conns_[i]->WriteFull(&elems, sizeof(elems)) <= 0);
          if (likely(elems > 0))
            BUG_ON(agg_conns_[i]->WriteFull(start, sizeof(work_unit) * elems) <=
                   0);
        }));
      }
      for (auto &t : th) t.Join();
    }
  }
};

static NetBarrier *b;

void RPCSStatWorker(std::unique_ptr<rt::TcpConn> c) {
  while (true) {
    // Receive an uptime request.
    uint64_t magic;
    ssize_t ret = c->ReadFull(&magic, sizeof(magic));
    if (ret != static_cast<ssize_t>(sizeof(magic))) {
      if (ret == 0 || ret == -ECONNRESET) break;
      log_err("read failed, ret = %ld", ret);
      break;
    }

    // Check for the right magic value.
    if (ntoh64(magic) != kRPCSStatMagic) break;

    int fd;
    char buffer[1024];
    uint64_t busy = 0;
    uint64_t total = 0;
    char *token = NULL;

    /* Calculate the cycles spent by this process */
    fd = open("/proc/self/stat", O_RDONLY);
    assert(fd != -1);
    ret = read(fd, buffer, sizeof(buffer) - 1);
    assert(ret >= 0);
    buffer[ret] = '\0';
    for (int i = 1; i <= 15; i++) {
        token = strtok(i == 1 ? buffer : NULL, " ");
        assert(token);
        if (i == 14) busy += (uint64_t)atol(token);
        if (i == 15) busy += (uint64_t)atol(token);
    }

    /* Calculate the total cycles spent */
    fd = open("/proc/stat", O_RDONLY);
    assert(fd != -1);
    ret = read(fd, buffer, sizeof(buffer) - 1);
    assert(ret >= 0);
    buffer[ret] = '\0';
    for (int i = 1; i <= 8; i++) {
        token = strtok(i == 1 ? buffer : NULL, " ");
        assert(token);
        total += (uint64_t)atol(token);
    }

    sstat_raw u = {total,
                   busy,
                   rt::RuntimeGlobMemAccesses(),
                   rt::RuntimeGlobEnergyConsumed(),
                   rt::RuntimeMaxCores(),
                   static_cast<unsigned int>(sysconf(_SC_NPROCESSORS_ONLN)),
                   rpc::RpcServerStatCupdateRx(),
                   rpc::RpcServerStatEcreditTx(),
                   rpc::RpcServerStatCreditTx(),
                   rpc::RpcServerStatReqRx(),
                   rpc::RpcServerStatReqDropped(),
                   rpc::RpcServerStatRespTx()};

    // Send an uptime response.
    ssize_t sret = c->WriteFull(&u, sizeof(u));
    if (sret != sizeof(u)) {
      if (sret == -EPIPE || sret == -ECONNRESET) break;
      log_err("write failed, ret = %ld", sret);
      break;
    }
  }
}

void RPCSStatServer() {
  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kRPCSStatPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { RPCSStatWorker(std::unique_ptr<rt::TcpConn>(c)); })
        .Detach();
  }
}

sstat_raw ReadRPCSStat() {
  std::unique_ptr<rt::TcpConn> c(
      rt::TcpConn::Dial({0, 0}, {raddr[0].ip, kRPCSStatPort}));
  uint64_t magic = hton64(kRPCSStatMagic);
  ssize_t ret = c->WriteFull(&magic, sizeof(magic));
  if (ret != static_cast<ssize_t>(sizeof(magic)))
    panic("sstat request failed, ret = %ld", ret);
  sstat_raw u;
  ret = c->ReadFull(&u, sizeof(u));
  if (ret != static_cast<ssize_t>(sizeof(u)))
    panic("sstat response failed, ret = %ld", ret);
  return sstat_raw{u.total, u.busy, u.mem_accesses, u.energy_consumed, u.num_cores, u.max_cores, u.cupdate_rx,
      u.ecredit_tx, u.credit_tx, u.req_rx, u.req_dropped, u.resp_tx};
}

shstat_raw ReadShenangoStat() {
  char *buf_;
  std::string buf;
  std::map<std::string, uint64_t> smap;
  std::unique_ptr<rt::TcpConn> c(
      rt::TcpConn::Dial({0, 0}, {raddr[0].ip, kShenangoStatPort}));
  uint64_t magic = hton64(kShenangoStatMagic);
  ssize_t ret = c->WriteFull(&magic, sizeof(magic));
  if (ret != static_cast<ssize_t>(sizeof(magic)))
    panic("Shenango stat request failed, ret = %ld", ret);

  size_t resp_len;
  ret = c->ReadFull(&resp_len, sizeof(resp_len));
  if (ret != static_cast<ssize_t>(sizeof(resp_len)))
    panic("Shenango stat response failed, ret = %ld", ret);

  buf_ = (char *)malloc(resp_len);

  ret = c->ReadFull(buf_, resp_len);
  if (ret != static_cast<ssize_t>(resp_len))
    panic("Shenango stat response failed, ret = %ld", ret);

  buf = std::string(buf_);

  size_t pos_com = 0;
  size_t pos_col = 0;
  std::string token;
  std::string key;
  uint64_t value;

  while ((pos_com = buf.find(",")) != std::string::npos) {
    token = buf.substr(0, pos_com);
    pos_col = token.find(":");
    if (pos_col == std::string::npos) continue;

    key = token.substr(0, pos_col);
    value = std::stoull(token.substr(pos_col + 1, pos_com));

    smap[key] = value;

    buf.erase(0, pos_com + 1);
  }

  free(buf_);

  return shstat_raw{smap["rx_packets"], smap["tx_packets"],
                    smap["rx_bytes"],   smap["tx_bytes"],
                    smap["drops"],      smap["rx_tcp_out_of_order"]};
}

constexpr uint64_t kNetbenchPort = 8001;


// The maximum lateness to tolerate before dropping egress samples.
constexpr uint64_t kMaxCatchUpUS = 5;

void RpcServer(struct srpc_ctx *ctx) {
  // Validate and parse the request.
  if (unlikely(ctx->req_len != sizeof(payload))) {
    log_err("got invalid RPC len %ld", ctx->req_len);
    return;
  }
  const payload *in = reinterpret_cast<const payload *>(ctx->req_buf);

  // Perform the synthetic work.
  uint64_t workn = ntoh64(in->work_itr);
  int core_id = get_current_affinity();

  uint64_t start = microtime();
  assert(workn > 0);
  if (in->req_type == CPU_BOUND) {
    cpu_bound_workers[core_id]->Work(workn);
  } else if (in->req_type == MEM_BOUND) {
    if (!use_msem) {
      mem_bound_workers[core_id]->Work(workn);
    } else {
      assert(msem != nullptr);
      if (msem->TryWait()) {
        mem_bound_workers[core_id]->Work(workn);
        msem->Post();
      } else {
        ctx->drop = true;
        return;
      }
    }
  } else if (in->req_type == LOCK_BOUND) {
	  if (cong_aware_mutex_lock_if_uncongested(&mut)) {
        lock_bound_workers[core_id]->Work(workn);
		cong_aware_mutex_unlock(&mut);
	  } else {
        ctx->drop = true;
        return;
	  }
  } else {
	  // Not possible
	  assert(false);
  }
  uint64_t end = microtime();

  // Craft a response.
  ctx->resp_len = sizeof(payload);
  payload *out = reinterpret_cast<payload *>(ctx->resp_buf);
  memcpy(out, in, sizeof(*out));
  out->tsc_end = hton64(rdtscp(&out->cpu));
  out->cpu = hton32(out->cpu);
  out->server_queue = hton64(rt::RuntimeQueueUS());
  out->work_us = hton64(end - start);
  if (use_msem) {
      out->msem_cap = hton32(msem->GetCapacity());
  } else {
      out->msem_cap = 0;
  }
}

#ifdef PROFILE_ST

#define NUM_ITRS (10000)
#define CPU_WORK_ITRS (4000)
#define MEM_WORK_ITRS (78)
#define LOCK_WORK_ITRS (4000)

void NetbenchProfileServiceTimes() {
    std::vector<uint64_t> times(NUM_ITRS);
    uint64_t start;
    uint64_t end;

    for (int i = 0; i < NUM_ITRS; ++i) {
        start = microtime();
        cpu_bound_workers[0]->Work(CPU_WORK_ITRS);
        end = microtime();
        times[i] = end - start;
    }

    // Print the times
    std::sort(times.begin(), times.end());
    printf("CPU-bound operation times:\n");
    printf("\tmin:%ld us\n", times[0]);
    printf("\tp50:%ld us\n", times[NUM_ITRS * 0.50]);
    printf("\tp90:%ld us\n", times[NUM_ITRS * 0.90]);
    printf("\tp99:%ld us\n", times[NUM_ITRS * 0.99]);
    printf("\tmax:%ld us\n", times[NUM_ITRS - 1]);


    for (int i = 0; i < NUM_ITRS; ++i) {
        start = microtime();
        mem_bound_workers[0]->Work(MEM_WORK_ITRS);
        end = microtime();
        times[i] = end - start;
    }

    // Print the times
    std::sort(times.begin(), times.end());
    printf("Memory bandwidth-bound operation times:\n");
    printf("\tmin:%ld us\n", times[0]);
    printf("\tp50:%ld us\n", times[NUM_ITRS * 0.50]);
    printf("\tp90:%ld us\n", times[NUM_ITRS * 0.90]);
    printf("\tp99:%ld us\n", times[NUM_ITRS * 0.99]);
    printf("\tmax:%ld us\n", times[NUM_ITRS - 1]);

    for (int i = 0; i < NUM_ITRS; ++i) {
        start = microtime();
        lock_bound_workers[0]->Work(LOCK_WORK_ITRS);
        end = microtime();
        times[i] = end - start;
    }

    // Print the times
    std::sort(times.begin(), times.end());
    printf("Lock-bound operation times:\n");
    printf("\tmin:%ld us\n", times[0]);
    printf("\tp50:%ld us\n", times[NUM_ITRS * 0.50]);
    printf("\tp90:%ld us\n", times[NUM_ITRS * 0.90]);
    printf("\tp99:%ld us\n", times[NUM_ITRS * 0.99]);
    printf("\tmax:%ld us\n", times[NUM_ITRS - 1]);
}
#endif

void ServerHandler(void *arg) {
  rt::Thread([] { RPCSStatServer(); }).Detach();
  int num_cores = rt::RuntimeMaxCores();

  for (int i = 0; i < num_cores; ++i) {
    cpu_bound_workers[i] = SyntheticWorkerFactory("sqrt");
    if (cpu_bound_workers[i] == nullptr) panic("cannot create worker");
    mem_bound_workers[i] = SyntheticWorkerFactory("membwantagonist:32768:0:0");
    if (mem_bound_workers[i] == nullptr) panic("cannot create worker");
    lock_bound_workers[i] = SyntheticWorkerFactory("sqrt");
    if (lock_bound_workers[i] == nullptr) panic("cannot create worker");
  }

#ifdef PROFILE_ST
  // Profile the request execution times
  NetbenchProfileServiceTimes();
#endif

  // Create the memory semaphore object
  if (use_msem) {
      msem = MemSemaphore::GetInstance();
  }

  // Initialize the lock object
  cong_aware_mutex_init(&mut, CONG_AWARE_MUTEX_POLICY_QLEN);

  int ret = rpc::RpcServerEnable(RpcServer);
  if (ret) panic("couldn't enable RPC server");

  // waits forever.
  rt::WaitGroup(1).Wait();
}

void LoadBalancer(struct srpc_ctx *ctx) {
  // Validate and parse the request
  if (unlikely(ctx->req_len != sizeof(payload))) {
    log_err("got invalid RPC len %ld", ctx->req_len);
    return;
  }
  const payload *in = reinterpret_cast<const payload *>(ctx->req_buf);

  LBCTX<payload> *resp_ctx = load_balancer[0]->Send(
			(void *)ctx->req_buf, sizeof(payload), in->hash);
  resp_ctx->Wait();

  // Craft a response
  ctx->resp_len = sizeof(payload);
  ctx->ds_credit = load_balancer[0]->Credit();
  ctx->drop = resp_ctx->IsDropped();

  payload *out = reinterpret_cast<payload *>(ctx->resp_buf);
  memcpy(out, in, sizeof(*out));

  out->tsc_end = hton64(rdtscp(&out->cpu));
  out->cpu = hton32(out->cpu);
  out->server_queue = hton64(rt::RuntimeQueueUS());

  delete resp_ctx;
}

void LBLocalDropHandler(struct crpc_ctx *c) {
  LBCTX<payload> *ctx =
	  rpc::LoadBalancer<payload, PAYLOAD_ID_OFF>::GetCTX(c->buf);

  memcpy(&ctx->resp, c->buf, c->len);
  ctx->dropped = true;
  ctx->Done();
}

void LBRemoteDropHandler(void *buf, size_t len, void *arg) {
  assert(len == sizeof(payload));
  LBCTX<payload> *ctx =
	  rpc::LoadBalancer<payload, PAYLOAD_ID_OFF>::GetCTX((char *)buf);

  ctx->dropped = true;
  ctx->Done();
}

void LBHandler(void *arg) {
  rt::Thread([] { RPCSStatServer(); }).Detach();

  load_balancer[0] = new rpc::LoadBalancer<payload, PAYLOAD_ID_OFF>(raddr,
			num_servers, kNumDupClient, nconn[0], LBLocalDropHandler,
			LBRemoteDropHandler);

  /* Start Server */
  int ret = rpc::RpcServerEnable(LoadBalancer);
  if (ret) panic("couldn't start LB server");
  // waits forever.
  rt::WaitGroup(1).Wait();
}

void FanOut(struct srpc_ctx *ctx) {
  // Validate and parse the request
  if (unlikely(ctx->req_len != sizeof(payload))) {
    log_err("got invalid RPC len %ld", ctx->req_len);
    return;
  }
  const payload *in = reinterpret_cast<const payload *>(ctx->req_buf);

  FOCTX<payload> *resp_ctx = fan_outer->Send(
			(void *)ctx->req_buf, sizeof(payload), in->hash);

  resp_ctx->Wait();

  // Craft a response
  ctx->resp_len = sizeof(payload);
  ctx->ds_credit = fan_outer->Credit();
  ctx->drop = resp_ctx->IsDropped();

  payload *out = reinterpret_cast<payload *>(ctx->resp_buf);
  memcpy(out, in, sizeof(*out));

  out->tsc_end = hton64(rdtscp(&out->cpu));
  out->cpu = hton32(out->cpu);
  out->server_queue = hton64(rt::RuntimeQueueUS());

  delete resp_ctx;
}

void FOLocalDropHandler(struct crpc_ctx *c) {
  FOCTX<payload> *ctx = rpc::FanOuter<payload, PAYLOAD_ID_OFF>::GetCTX(c->buf);
  int idx = ctx->num_resp++;

  memcpy(&ctx->resp[idx], c->buf, c->len);
  ctx->dropped[idx] = true;
  ctx->Done();
}

void FORemoteDropHandler(void *buf, size_t len, void *arg) {
  assert(len == sizeof(payload));
  FOCTX<payload> *ctx =
	  rpc::FanOuter<payload, PAYLOAD_ID_OFF>::GetCTX((char *)buf);
  int idx = ctx->num_resp++;

  ctx->dropped[idx] = true;
  ctx->Done();
}

void FOHandler(void *arg) {
  rt::Thread([] { RPCSStatServer(); }).Detach();

  fan_outer = new rpc::FanOuter<payload, PAYLOAD_ID_OFF>(raddr,
			num_servers, kNumDupClient, nconn[0],
			FOLocalDropHandler, FORemoteDropHandler);

  /* Start server */
  int ret = rpc::RpcServerEnable(FanOut);
  if (ret) panic("couldn't start FO server");
  // waits forever.
  rt::WaitGroup(1).Wait();
}

void Sequential(struct srpc_ctx *ctx) {
  uint64_t ds_credit;
  bool success = true;
  // Validate and parse the request
  if (unlikely(ctx->req_len != sizeof(payload))) {
    log_err("got invalid RPC len %ld", ctx->req_len);
    return;
  }
  const payload *in = reinterpret_cast<const payload *>(ctx->req_buf);

  LBCTX<payload> *resp_ctx;
  for(int i = 0; i < num_servers; ++i) {
    resp_ctx = load_balancer[i]->Send(
			(void *)ctx->req_buf, sizeof(payload), in->hash);
    resp_ctx->Wait();

    if (resp_ctx->IsDropped()) {
      success = false;
      break;
    }
  }

  ds_credit = load_balancer[0]->Credit();
  for (int i = 1; i < num_servers; ++i) {
    ds_credit = MIN(ds_credit, load_balancer[i]->Credit());
  }

  // Craft a response
  ctx->resp_len = sizeof(payload);
  ctx->ds_credit = ds_credit;
  ctx->drop = !success;

  payload *out = reinterpret_cast<payload *>(ctx->resp_buf);
  memcpy(out, in, sizeof(*out));

  out->tsc_end = hton64(rdtscp(&out->cpu));
  out->cpu = hton32(out->cpu);
  out->server_queue = hton64(rt::RuntimeQueueUS());

  delete resp_ctx;
}


void SEQHandler(void *arg) {
  rt::Thread([] { RPCSStatServer(); }).Detach();

  for(int i = 0; i < num_servers; ++i) {
    load_balancer[i] = new rpc::LoadBalancer<payload, PAYLOAD_ID_OFF>(
			raddr + i, 1, kNumDupClient, nconn[i],
			LBLocalDropHandler, LBRemoteDropHandler);
  }

  /* Start server */
  int ret = rpc::RpcServerEnable(Sequential);
  if (ret) panic("couldn't start FO server");
  // waits forever.
  rt::WaitGroup(1).Wait();
}

template <class Arrival>
std::vector<work_unit> GenerateWork(Arrival a, double cur_us,
                                    double last_us, bool is_monster,
                                    uint64_t cur_mem_bound_work_itr) {
  std::vector<work_unit> w;
  uint64_t req_type;
  uint64_t work_itr;

  while (true) {
    cur_us += a();
    if (cur_us > last_us) break;

	// Generate an operation randomly
	double sample = ((double)rand() / (double)RAND_MAX) * 100.0;
	for (req_type = 0; req_type < NUM_REQUEST_TYPES; req_type++) {
		sample -= req_mix[req_type];
		if (sample < 0) {
			break;
		}
	}
	assert(req_type < NUM_REQUEST_TYPES);

	if (req_type == CPU_BOUND) {
		work_itr = cpu_bound_work_itr;
	} else if (req_type == MEM_BOUND) {
		work_itr = cur_mem_bound_work_itr;
	} else if (req_type == LOCK_BOUND) {
		work_itr = lock_bound_work_itr;
	} else {
		assert(false);
	}

    w.emplace_back(work_unit{cur_us, 0, req_type, work_itr,
                rand(), false, is_monster});
  }

  return w;
}

std::vector<work_unit> ClientWorker(
    rpc::RpcClient *c, rt::WaitGroup *starter, rt::WaitGroup *starter2,
    std::function<std::vector<work_unit>()> wf) {
  std::vector<work_unit> w(wf());

  std::vector<rt::Thread> ths;

  // Start the receiver thread.
  for(int i = 0; i < c->NumConns(); ++i) {
    ths.emplace_back(rt::Thread([&, i] {
      payload rp;

      while (true) {
        ssize_t ret = c->Recv(&rp, sizeof(rp), i, (void *)w.data());
        if (ret != static_cast<ssize_t>(sizeof(rp))) {
          if (ret == 0 || ret < 0) break;
          panic("read failed, ret = %ld", ret);
        }

        uint64_t idx = ntoh64(rp.index);

        w[idx].duration_us = microtime() - w[idx].timing;
        w[idx].success = true;
        w[idx].credit = c->Credit();
        w[idx].tsc = ntoh64(rp.tsc_end);
        w[idx].cpu = ntoh32(rp.cpu);
        w[idx].server_queue = ntoh64(rp.server_queue);
        w[idx].work_us = ntoh64(rp.work_us);
        w[idx].server_time = w[idx].work_us + w[idx].server_queue;
        w[idx].msem_cap = ntoh32(rp.msem_cap);
      }
    }));
  }

  // Synchronized start of load generation.
  starter->Done();
  starter2->Wait();

  barrier();
  auto expstart = steady_clock::now();
  barrier();

  payload p;
  auto wsize = w.size();

  for (unsigned int i = 0; i < wsize; ++i) {
    barrier();
    auto now = steady_clock::now();
    barrier();
    if (duration_cast<sec>(now - expstart).count() < w[i].start_us) {
      rt::Sleep(w[i].start_us - duration_cast<sec>(now - expstart).count());
    }

    if (i > 1 && w[i-1].start_us <= kWarmUpTime &&
	w[i].start_us >= kWarmUpTime)
      c->StatClear();

    if (duration_cast<sec>(now - expstart).count() - w[i].start_us >
        kMaxCatchUpUS)
      continue;

    w[i].timing = microtime();

    // Send an RPC request.
    p.req_type = w[i].req_type;
    p.work_itr = hton64(w[i].work_itr);
    p.index = hton64(i);
    p.hash = hton64(w[i].hash);
    ssize_t ret = c->Send(&p, sizeof(p), w[i].hash, (void *)w.data());
    if (ret == -ENOBUFS) continue;
    if (ret != static_cast<ssize_t>(sizeof(p)))
      panic("write failed, ret = %ld", ret);
  }

  rt::Sleep((int)(kRTT + 2));
  BUG_ON(c->Shutdown(SHUT_RDWR));

  for (auto &th : ths)
    th.Join();

  return w;
}

void ClientLocalDropHandler(struct crpc_ctx *c) {
  payload *req = reinterpret_cast<payload *>(c->buf);
  uint64_t idx = ntoh64(req->index);
  work_unit *w = reinterpret_cast<work_unit *>(c->arg);

  w[idx].duration_us = 0;
  w[idx].success = false;
}

void ClientRemoteDropHandler(void *buf, size_t len, void *arg) {
  assert(len == sizeof(payload));
  payload *req = (payload *)buf;
  uint64_t idx = ntoh64(req->index);
  work_unit *w = reinterpret_cast<work_unit *>(arg);

  w[idx].duration_us = microtime() - w[idx].timing;
  w[idx].success = false;
}

std::vector<work_unit> RunExperiment(
    int threads, struct cstat_raw *csr, struct sstat *ss, double *elapsed,
    std::function<std::vector<work_unit>()> wf,
    std::function<std::vector<work_unit>()> wf2) {
  // Create one TCP connection per thread.
  std::vector<std::unique_ptr<rpc::RpcClient>> clients;
  sstat_raw s1, s2;
  shstat_raw sh1, sh2;

  int server_idx;
  int conn_idx;

  for (int i = 0; i < threads; ++i) {
    struct rpc_session_info info = {.session_type = 0};
    std::unique_ptr<rpc::RpcClient> outc(rpc::RpcClient::Dial(raddr[0], i + 1,
					ClientLocalDropHandler,
					ClientRemoteDropHandler,
					&info));
    if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");

    server_idx = 0;
    conn_idx = 1;

    // Add the connections to other server replicas
    while (server_idx < num_servers) {
      if (conn_idx >= nconn[server_idx]) {
        ++server_idx;
        conn_idx = 0;
        continue;
      }
      outc->AddConnection(raddr[server_idx]);
      ++conn_idx;
    }

    clients.emplace_back(std::move(outc));
  }

  // Launch a worker thread for each connection.
  rt::WaitGroup starter(threads);
  rt::WaitGroup starter2(1);

  std::vector<rt::Thread> th;
  std::unique_ptr<std::vector<work_unit>> samples[threads];

  th.emplace_back(rt::Thread([&] {
    auto v = ClientWorker(clients[0].get(), &starter, &starter2, wf2);
    samples[0].reset(new std::vector<work_unit>(std::move(v)));
  }));

  for (int i = 1; i < threads; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      srand(time(NULL) * (i+1));
      auto v = ClientWorker(clients[i].get(), &starter, &starter2, wf);
      samples[i].reset(new std::vector<work_unit>(std::move(v)));
    }));
  }

  if (!b || b->IsLeader()) {
    s1 = ReadRPCSStat();
    sh1 = ReadShenangoStat();
  }

  // Give the workers time to initialize, then start recording.
  starter.Wait();
  if (b && !b->StartExperiment()) {
    exit(0);
  }
  starter2.Done();

  // |--- start experiment duration timing ---|
  barrier();
  timex = std::time(nullptr);
  auto start = steady_clock::now();
  barrier();

  // Clear the stat after warmup time
  rt::Sleep(kWarmUpTime);
  if (!b || b->IsLeader()) {
    s1 = ReadRPCSStat();
    sh1 = ReadShenangoStat();
  }
  for (auto &c : clients) {
    c->StatClear();
  }

  // Wait for the workers to finish.
  for (auto &t : th) {
    t.Join();
  }

  // |--- end experiment duration timing ---|
  barrier();
  auto finish = steady_clock::now();
  barrier();

  if (!b || b->IsLeader()) {
    s2 = ReadRPCSStat();
    sh2 = ReadShenangoStat();
  }

  // Force the connections to close.
  for (auto &c : clients) c->Abort();

  double elapsed_ = duration_cast<sec>(finish - start).count();
  elapsed_ -= kWarmUpTime;

  // Aggregate client stats
  if (csr) {
    for (auto &c : clients) {
      csr->ecredit_rx += c->StatEcreditRx();
      csr->cupdate_tx += c->StatCupdateTx();
      csr->resp_rx += c->StatRespRx();
      csr->req_tx += c->StatReqTx();
      csr->credit_expired += c->StatCreditExpired();
      c->Close();
    }
  }

  // Aggregate all the samples together.
  std::vector<work_unit> w;
  double min_throughput = 0.0;
  double max_throughput = 0.0;
  uint64_t good_resps = 0;
  uint64_t cpu_bound_req_good_resps = 0;
  uint64_t mem_bound_req_good_resps = 0;
  uint64_t lock_bound_req_good_resps = 0;
  uint64_t resps = 0;
  uint64_t cpu_bound_req_resps = 0;
  uint64_t mem_bound_req_resps = 0;
  uint64_t lock_bound_req_resps = 0;
  uint64_t offered = 0;
  uint64_t client_drop = 0;

  for (int i = 0; i < threads; ++i) {
    auto &v = *samples[i];
    double throughput;
    int slo_success;
    int cpu_bound_slo_success;
    int mem_bound_slo_success;
    int lock_bound_slo_success;
    int resp_success;
    int cpu_bound_req_success;
    int mem_bound_req_success;
    int lock_bound_req_success;

    // Remove requests arrived during warm-up periods
    v.erase(std::remove_if(v.begin(), v.end(),
                        [](const work_unit &s) {
                          return ((s.start_us + s.duration_us) < kWarmUpTime);
                        }),
            v.end());

    offered += v.size();
    client_drop += std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return (s.duration_us == 0);
    });

    // Remove local drops
    v.erase(std::remove_if(v.begin(), v.end(),
                        [](const work_unit &s) {
                          return (s.duration_us == 0);
                        }),
            v.end());
    resp_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success;
    });
    cpu_bound_req_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success && s.req_type == CPU_BOUND;
    });
    mem_bound_req_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success && s.req_type == MEM_BOUND;
    });
    lock_bound_req_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success && s.req_type == LOCK_BOUND;
    });
    slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success && s.duration_us < slo;
    });
    cpu_bound_slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success && s.duration_us < slo && s.req_type == CPU_BOUND;
    });
    mem_bound_slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success && s.duration_us < slo && s.req_type == MEM_BOUND;
    });
    lock_bound_slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
      return s.success && s.duration_us < slo && s.req_type == LOCK_BOUND;
    });
    throughput = static_cast<double>(resp_success) / elapsed_ * 1000000;

    resps += resp_success;
    cpu_bound_req_resps += cpu_bound_req_success;
    mem_bound_req_resps += mem_bound_req_success;
    lock_bound_req_resps += lock_bound_req_success;
    good_resps += slo_success;
    cpu_bound_req_good_resps += cpu_bound_slo_success;
    mem_bound_req_good_resps += mem_bound_slo_success;
    lock_bound_req_good_resps += lock_bound_slo_success;
    if (i == 0) {
      min_throughput = throughput;
      max_throughput = throughput;
    } else {
      min_throughput = MIN(throughput, min_throughput);
      max_throughput = MAX(throughput, max_throughput);
    }

    w.insert(w.end(), v.begin(), v.end());
  }

  // Report results.
  if (csr) {
    csr->offered_rps = static_cast<double>(offered) / elapsed_ * 1000000;
    csr->rps = static_cast<double>(resps) / elapsed_ * 1000000;
    csr->cpu_bound_req_rps = static_cast<double>(cpu_bound_req_resps) / elapsed_ * 1000000;
    csr->mem_bound_req_rps = static_cast<double>(mem_bound_req_resps) / elapsed_ * 1000000;
    csr->lock_bound_req_rps = static_cast<double>(lock_bound_req_resps) / elapsed_ * 1000000;
    csr->goodput = static_cast<double>(good_resps) / elapsed_ * 1000000;
    csr->cpu_bound_req_goodput = static_cast<double>(cpu_bound_req_good_resps) / elapsed_ * 1000000;
    csr->mem_bound_req_goodput = static_cast<double>(mem_bound_req_good_resps) / elapsed_ * 1000000;
    csr->lock_bound_req_goodput = static_cast<double>(lock_bound_req_good_resps) / elapsed_ * 1000000;
    csr->req_dropped = client_drop;
    csr->min_percli_tput = min_throughput;
    csr->max_percli_tput = max_throughput;
  }

  if ((!b || b->IsLeader()) && ss) {
    uint64_t total = s2.total - s1.total;
    uint64_t busy = s2.busy - s1.busy;
    ss->cpu_usage = static_cast<double>(busy) / static_cast<double>(total);

    uint64_t mem_accesses = s2.mem_accesses - s1.mem_accesses;
    ss->membw_usage = static_cast<double>(mem_accesses) / elapsed_ * 1000000;

    double energy_consumed = s2.energy_consumed - s1.energy_consumed;
    ss->power_usage = energy_consumed / elapsed_ * 1000000;

    uint64_t cupdate_rx_pkts = s2.cupdate_rx - s1.cupdate_rx;
    uint64_t ecredit_tx_pkts = s2.ecredit_tx - s1.ecredit_tx;
    uint64_t credit_tx = s2.credit_tx - s1.credit_tx;
    uint64_t req_rx_pkts = s2.req_rx - s1.req_rx;
    uint64_t req_drop_pkts = s2.req_dropped - s1.req_dropped;
    uint64_t resp_tx_pkts = s2.resp_tx - s1.resp_tx;
    ss->cupdate_rx_pps = static_cast<double>(cupdate_rx_pkts) / elapsed_ * 1000000;
    ss->ecredit_tx_pps = static_cast<double>(ecredit_tx_pkts) / elapsed_ * 1000000;
    ss->credit_tx_cps = static_cast<double>(credit_tx) / elapsed_ * 1000000;
    ss->req_rx_pps = static_cast<double>(req_rx_pkts) / elapsed_ * 1000000;
    ss->req_drop_rate =
        static_cast<double>(req_drop_pkts) / static_cast<double>(req_rx_pkts);
    ss->resp_tx_pps = static_cast<double>(resp_tx_pkts) / elapsed_ * 1000000;

    uint64_t rx_pkts = sh2.rx_pkts - sh1.rx_pkts;
    uint64_t tx_pkts = sh2.tx_pkts - sh1.tx_pkts;
    uint64_t rx_bytes = sh2.rx_bytes - sh1.rx_bytes;
    uint64_t tx_bytes = sh2.tx_bytes - sh1.tx_bytes;
    uint64_t drops = sh2.drops - sh1.drops;
    uint64_t rx_tcp_ooo = sh2.rx_tcp_ooo - sh1.rx_tcp_ooo;
    ss->rx_pps = static_cast<double>(rx_pkts) / elapsed_ * 1000000;
    ss->tx_pps = static_cast<double>(tx_pkts) / elapsed_ * 1000000;
    ss->rx_bps = static_cast<double>(rx_bytes) / elapsed_ * 8000000;
    ss->tx_bps = static_cast<double>(tx_bytes) / elapsed_ * 8000000;
    ss->rx_drops_pps = static_cast<double>(drops) / elapsed_ * 1000000;
    ss->rx_ooo_pps = static_cast<double>(rx_tcp_ooo) / elapsed_ * 1000000;
  }

  *elapsed = elapsed_;

  return w;
}

void PrintHeader(std::ostream &os) {
  os << "num_threads,"
     << "offered_load,"
     << "throughput,"
     << "cpu_bound_req_throughput,"
     << "mem_bound_req_throughput,"
     << "lock_bound_req_throughput,"
     << "goodput,"
     << "cpu_bound_req_goodput,"
     << "mem_bound_req_goodput,"
     << "lock_bound_req_goodput,"
     << "cpu,"
     << "membw,"
     << "power,"
     << "min,"
     << "mean,"
     << "p50,"
     << "cpu_bound_req_p50,"
     << "mem_bound_req_p50,"
     << "lock_bound_req_p50,"
     << "p90,"
     << "cpu_bound_req_p90,"
     << "mem_bound_req_p90,"
     << "lock_bound_req_p90,"
     << "p99,"
     << "cpu_bound_req_p99,"
     << "mem_bound_req_p99,"
     << "lock_bound_req_p99,"
     << "p999,"
     << "p9999,"
     << "max,"
     << "reject_min,"
     << "reject_mean,"
     << "reject_p50,"
     << "reject_p99,"
     << "p1_credit,"
     << "mean_credit,"
     << "p99_credit,"
     << "p1_q,"
     << "mean_q,"
     << "p99_q,"
     << "mean_stime,"
     << "p99_stime,"
     << "server:rx_pps,"
     << "server:tx_pps,"
     << "server:rx_bps,"
     << "server:tx_bps,"
     << "server:rx_drops_pps,"
     << "server:rx_ooo_pps,"
     << "server:cupdate_rx_pps,"
     << "server:ecredit_tx_pps,"
     << "server:credit_tx_cps,"
     << "server:req_rx_pps,"
     << "server:req_drop_rate,"
     << "server:resp_tx_pps,"
     << "client:min_tput,"
     << "client:max_tput,"
     << "client:ecredit_rx_pps,"
     << "client:cupdate_tx_pps,"
     << "client:resp_rx_pps,"
     << "client:req_tx_pps,"
     << "client:credit_expired_cps,"
     << "client:req_dropped_rps" << std::endl;
}

void PrintStatResults(std::vector<work_unit> w, struct cstat *cs,
                      struct sstat *ss) {
  if (w.size() == 0) {
    std::cout << std::setprecision(4) << std::fixed << threads * total_agents
              << "," << cs->offered_rps << ","
              << "-" << std::endl;
    return;
  }

  std::vector<work_unit> rejected;

  std::copy_if(w.begin(), w.end(), std::back_inserter(rejected), [](work_unit &s) {
    return !s.success;
  });

  uint64_t reject_cnt = rejected.size();
  uint64_t reject_min = 0;
  uint64_t reject_p50 = 0;
  double reject_mean = 0.0;
  uint64_t reject_p99 = 0;

  if (reject_cnt > 0) {
    double sum;

    std::sort(rejected.begin(), rejected.end(),
	      [](const work_unit &s1, const work_unit &s2) {
        return s1.duration_us < s2.duration_us;
    });
    sum = std::accumulate(rejected.begin(), rejected.end(), 0.0,
			  [](double s, const work_unit &c) {
			      return s + c.duration_us;
			  });

    reject_min = rejected[0].duration_us;
    reject_mean = static_cast<double>(sum) / reject_cnt;
    reject_p50 = rejected[(reject_cnt - 1) * 0.5].duration_us;
    reject_p99 = rejected[(reject_cnt - 1) * 0.99].duration_us;
  }

  w.erase(std::remove_if(w.begin(), w.end(),
			 [](const work_unit &s) {
			   return !s.success;
	}), w.end());


  // Get the p99 times for CPU-bound and Memory-bound requests
  std::vector<work_unit> cpu_bound_work;
  std::vector<work_unit> mem_bound_work;
  std::vector<work_unit> lock_bound_work;
  double cpu_bound_work_count = 0;
  double cpu_bound_req_p50 = 0;
  double cpu_bound_req_p90 = 0;
  double cpu_bound_req_p99 = 0;
  double mem_bound_work_count = 0;
  double mem_bound_req_p50 = 0;
  double mem_bound_req_p90 = 0;
  double mem_bound_req_p99 = 0;
  double lock_bound_work_count = 0;
  double lock_bound_req_p50 = 0;
  double lock_bound_req_p90 = 0;
  double lock_bound_req_p99 = 0;

  std::copy_if(w.begin(), w.end(), std::back_inserter(cpu_bound_work), [](work_unit &s) {
          return s.success && s.req_type == CPU_BOUND;
    });
  std::sort(cpu_bound_work.begin(), cpu_bound_work.end(),
            [](const work_unit &s1, const work_unit &s2) {
                return s1.duration_us < s2.duration_us;
            });
  cpu_bound_work_count = static_cast<double>(cpu_bound_work.size());
  if (cpu_bound_work_count) {
      cpu_bound_req_p50 = cpu_bound_work[cpu_bound_work_count * 0.50].duration_us;
      cpu_bound_req_p90 = cpu_bound_work[cpu_bound_work_count * 0.90].duration_us;
      cpu_bound_req_p99 = cpu_bound_work[cpu_bound_work_count * 0.99].duration_us;
  }

  std::copy_if(w.begin(), w.end(), std::back_inserter(mem_bound_work), [](work_unit &s) {
          return s.success && s.req_type == MEM_BOUND;
      });
  std::sort(mem_bound_work.begin(), mem_bound_work.end(),
            [](const work_unit &s1, const work_unit &s2) {
                return s1.duration_us < s2.duration_us;
            });
  mem_bound_work_count = static_cast<double>(mem_bound_work.size());
  if (mem_bound_work_count) {
      mem_bound_req_p50 = mem_bound_work[mem_bound_work_count * 0.50].duration_us;
      mem_bound_req_p90 = mem_bound_work[mem_bound_work_count * 0.90].duration_us;
      mem_bound_req_p99 = mem_bound_work[mem_bound_work_count * 0.99].duration_us;
  }

  std::copy_if(w.begin(), w.end(), std::back_inserter(lock_bound_work), [](work_unit &s) {
          return s.success && s.req_type == LOCK_BOUND;
      });
  std::sort(lock_bound_work.begin(), lock_bound_work.end(),
            [](const work_unit &s1, const work_unit &s2) {
                return s1.duration_us < s2.duration_us;
            });
  lock_bound_work_count = static_cast<double>(lock_bound_work.size());
  if (lock_bound_work_count) {
      lock_bound_req_p50 = lock_bound_work[lock_bound_work_count * 0.50].duration_us;
      lock_bound_req_p90 = lock_bound_work[lock_bound_work_count * 0.90].duration_us;
      lock_bound_req_p99 = lock_bound_work[lock_bound_work_count * 0.99].duration_us;
  }

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.duration_us < s2.duration_us;
  });
  double sum = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.duration_us; });
  double mean = sum / w.size();
  double count = static_cast<double>(w.size());
  double p50 = w[count * 0.5].duration_us;
  double p90 = w[count * 0.9].duration_us;
  double p99 = w[count * 0.99].duration_us;
  double p999 = w[count * 0.999].duration_us;
  double p9999 = w[count * 0.9999].duration_us;
  double min = w[0].duration_us;
  double max = w[w.size() - 1].duration_us;

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.credit < s2.credit;
  });
  double sum_credit = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.credit; });
  double mean_credit = sum_credit / w.size();
  double p1_credit = w[count * 0.01].credit;
  double p99_credit = w[count * 0.99].credit;

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.server_queue < s2.server_queue;
  });
  double sum_que = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.server_queue; });
  double mean_que = sum_que / w.size();
  double p1_que = w[count * 0.01].server_queue;
  double p99_que = w[count * 0.99].server_queue;

  std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
    return s1.server_time < s2.server_time;
  });
  double sum_stime = std::accumulate(
      w.begin(), w.end(), 0.0,
      [](double s, const work_unit &c) { return s + c.server_time; });
  double mean_stime = sum_stime / w.size();
  double p99_stime = w[count * 0.99].server_time;

  // Save the work units in a file
  if (do_load_shift) {
    std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
      return s1.start_us < s2.start_us; // is this correct, or should it be start+duration??
    });
    std::ofstream all_tasks_file;
    all_tasks_file.open ("all_tasks.csv");
    all_tasks_file << "start_us,req_type,work_us,msem_cap,duration_us,tsc,server_queue,server_time" << std::endl;
    all_tasks_file << std::setprecision(8) << std::fixed;
    for (unsigned int i = 0; i < w.size(); ++i) {
      all_tasks_file << w[i].start_us << ","
                     << w[i].req_type << ","
                     << w[i].work_us << ","
                     << w[i].msem_cap << ","
                     << w[i].duration_us << ","
                     << w[i].tsc << ","
                     << w[i].server_queue << ","
                     << w[i].server_time << std::endl;
    }
    all_tasks_file.close();
  }

  std::cout << std::setprecision(4) << std::fixed << threads * total_agents << ","
	    << cs->offered_rps << "," << cs->rps << ","
	    << cs->cpu_bound_req_rps << "," << cs->mem_bound_req_rps << "," << cs->lock_bound_req_rps << ","
	    << cs->goodput << "," << cs->cpu_bound_req_goodput << "," << cs->mem_bound_req_goodput << "," << cs->lock_bound_req_goodput << ","
		<< ss->cpu_usage << "," << ss->membw_usage << ","
		<< ss->power_usage << "," << min << "," << mean << ","
	    << p50 << "," << cpu_bound_req_p50 << "," << mem_bound_req_p50 << "," << lock_bound_req_p50 << ","
	    << p90 << "," << cpu_bound_req_p90 << "," << mem_bound_req_p90 << "," << lock_bound_req_p90 << ","
	    << p99 << "," << cpu_bound_req_p99 << "," << mem_bound_req_p99 << "," << lock_bound_req_p99 << ","
	    << p999 << "," << p9999 << "," << max << ","
	    << reject_min << "," << reject_mean << "," << reject_p50 << ","
	    << reject_p99 << ","
	    << p1_credit << "," << mean_credit << "," << p99_credit << ","
	    << p1_que << ","
	    << mean_que << "," << p99_que << "," << mean_stime << ","
	    << p99_stime << "," << ss->rx_pps << "," << ss->tx_pps << ","
	    << ss->rx_bps << "," << ss->tx_bps << "," << ss->rx_drops_pps << ","
	    << ss->rx_ooo_pps << "," << ss->cupdate_rx_pps << ","
	    << ss->ecredit_tx_pps << "," << ss->credit_tx_cps << ","
	    << ss->req_rx_pps << "," << ss->req_drop_rate << ","
	    << ss->resp_tx_pps << ","
	    << cs->min_percli_tput << "," << cs->max_percli_tput << ","
	    << cs->ecredit_rx_pps << "," << cs->cupdate_tx_pps << ","
	    << cs->resp_rx_pps << "," << cs->req_tx_pps << ","
	    << cs->credit_expired_cps << "," << cs->req_dropped_rps << std::endl;

  csv_out << std::setprecision(4) << std::fixed << threads * total_agents << ","
	  << cs->offered_rps << "," << cs->rps << ","
      << cs->cpu_bound_req_rps << "," << cs->mem_bound_req_rps << "," << cs->lock_bound_req_rps << ","
      << cs->goodput << "," << cs->cpu_bound_req_goodput << "," << cs->mem_bound_req_goodput << "," << cs->lock_bound_req_goodput << ","
      << ss->cpu_usage << "," << ss->membw_usage << ","
      << ss->power_usage << "," << min << "," << mean << ","
	  << p50 << "," << cpu_bound_req_p50 << "," << mem_bound_req_p50 << "," << lock_bound_req_p50 << ","
	  << p90 << "," << cpu_bound_req_p90 << "," << mem_bound_req_p90 << "," << lock_bound_req_p90 << ","
	  << p99 << "," << cpu_bound_req_p99 << "," << mem_bound_req_p99 << "," << lock_bound_req_p99 << ","
	  << p999 << "," << p9999 << "," << max << ","
	  << reject_min << "," << reject_mean << "," << reject_p50 << ","
	  << reject_p99 << ","
	  << p1_credit << "," << mean_credit << "," << p99_credit << ","
	  << p1_que << ","
	  << mean_que << "," << p99_que << "," << mean_stime << ","
	  << p99_stime << "," << ss->rx_pps << "," << ss->tx_pps << ","
	  << ss->rx_bps << "," << ss->tx_bps << "," << ss->rx_drops_pps << ","
	  << ss->rx_ooo_pps << "," << ss->cupdate_rx_pps << ","
	  << ss->ecredit_tx_pps << "," << ss->credit_tx_cps << ","
	  << ss->req_rx_pps << "," << ss->req_drop_rate << ","
	  << ss->resp_tx_pps << ","
	  << cs->min_percli_tput << "," << cs->max_percli_tput << ","
	  << cs->ecredit_rx_pps << "," << cs->cupdate_tx_pps << ","
	  << cs->resp_rx_pps << "," << cs->req_tx_pps << ","
	  << cs->credit_expired_cps << "," << cs->req_dropped_rps
	  << std::endl << std::flush;

  json_out << "{"
           << "\"num_threads\":" << threads * total_agents << ","
           << "\"offered_load\":" << cs->offered_rps << ","
           << "\"throughput\":" << cs->rps << ","
           << "\"cpu_bound_req_throughput\":" << cs->cpu_bound_req_rps << ","
           << "\"mem_bound_req_throughput\":" << cs->mem_bound_req_rps << ","
           << "\"lock_bound_req_throughput\":" << cs->lock_bound_req_rps << ","
           << "\"goodput\":" << cs->goodput << ","
           << "\"cpu_bound_req_goodput\":" << cs->cpu_bound_req_goodput << ","
           << "\"mem_bound_req_goodput\":" << cs->mem_bound_req_goodput << ","
           << "\"lock_bound_req_goodput\":" << cs->lock_bound_req_goodput << ","
           << "\"cpu\":" << ss->cpu_usage << ","
           << "\"membw\":" << ss->membw_usage << ","
           << "\"power\":" << ss->power_usage << ","
           << "\"min\":" << min << ","
           << "\"mean\":" << mean << ","
           << "\"p50\":" << p50 << ","
           << "\"cpu_bound_req_p50\":" << cpu_bound_req_p50 << ","
           << "\"mem_bound_req_p50\":" << mem_bound_req_p50 << ","
           << "\"lock_bound_req_p50\":" << lock_bound_req_p50 << ","
           << "\"p90\":" << p90 << ","
           << "\"cpu_bound_req_p90\":" << cpu_bound_req_p90 << ","
           << "\"mem_bound_req_p90\":" << mem_bound_req_p90 << ","
           << "\"lock_bound_req_p90\":" << lock_bound_req_p90 << ","
           << "\"p99\":" << p99 << ","
           << "\"cpu_bound_req_p99\":" << cpu_bound_req_p99 << ","
           << "\"mem_bound_req_p99\":" << mem_bound_req_p99 << ","
           << "\"lock_bound_req_p99\":" << lock_bound_req_p99 << ","
           << "\"p999\":" << p999 << ","
           << "\"p9999\":" << p9999 << ","
           << "\"max\":" << max << ","
           << "\"reject_min\":" << reject_min << ","
           << "\"reject_mean\":" << reject_mean << ","
           << "\"reject_p50\":" << reject_p50 << ","
           << "\"reject_p99\":" << reject_p99 << ","
           << "\"p1_credit\":" << p1_credit << ","
           << "\"mean_credit\":" << mean_credit << ","
           << "\"p99_credit\":" << p99_credit << ","
           << "\"p1_q\":" << p1_que << ","
           << "\"mean_q\":" << mean_que << ","
           << "\"p99_q\":" << p99_que << ","
           << "\"mean_stime\":" << mean_stime << ","
           << "\"p99_stime\":" << p99_stime << ","
           << "\"server:rx_pps\":" << ss->rx_pps << ","
           << "\"server:tx_pps\":" << ss->tx_pps << ","
           << "\"server:rx_bps\":" << ss->rx_bps << ","
           << "\"server:tx_bps\":" << ss->tx_bps << ","
           << "\"server:rx_drops_pps\":" << ss->rx_drops_pps << ","
           << "\"server:rx_ooo_pps\":" << ss->rx_ooo_pps << ","
           << "\"server:cupdate_rx_pps\":" << ss->cupdate_rx_pps << ","
           << "\"server:ecredit_tx_pps\":" << ss->ecredit_tx_pps << ","
           << "\"server:credit_tx_cps\":" << ss->credit_tx_cps << ","
           << "\"server:req_rx_pps\":" << ss->req_rx_pps << ","
           << "\"server:req_drop_rate\":" << ss->req_drop_rate << ","
           << "\"server:resp_tx_pps\":" << ss->resp_tx_pps << ","
           << "\"client:min_tput\":" << cs->min_percli_tput << ","
           << "\"client:max_tput\":" << cs->max_percli_tput << ","
           << "\"client:ecredit_rx_pps\":" << cs->ecredit_rx_pps << ","
           << "\"client:cupdate_tx_pps\":" << cs->cupdate_tx_pps << ","
           << "\"client:resp_rx_pps\":" << cs->resp_rx_pps << ","
           << "\"client:req_tx_pps\":" << cs->req_tx_pps << ","
           << "\"client:credit_expired_cps\":" << cs->credit_expired_cps << ","
           << "\"client:req_dropped_rps\":" << cs->req_dropped_rps << "},"
           << std::endl
           << std::flush;
}

void LoadShiftExperiment(int threads) {
  struct sstat ss;
  struct cstat_raw csr;
  struct cstat cs;
  double elapsed;

  memset(&csr, 0, sizeof(csr));

  std::vector<work_unit> w = RunExperiment(threads, &csr, &ss, &elapsed,[=] {
    std::mt19937 rg(rand());
    std::vector<work_unit> w_temp;
    uint64_t last_us = 0;
    for (auto &test : load_shift_tests) {
      double rate = test.rate / (double) total_agents;
      std::exponential_distribution<double> rd(
          1.0 / (1000000.0 / (rate / static_cast<double>(threads))));
      auto work = GenerateWork(std::bind(rd, rg), last_us,
                               last_us + test.duration, false,
                               test.mem_bound_work_itr);
      last_us = work.back().start_us;
      w_temp.insert(w_temp.end(), work.begin(), work.end());
    }
    return w_temp;
  },
  [=] {
    std::mt19937 rg(rand());
    std::vector<work_unit> w_temp;
    uint64_t last_us = 0;
    for (auto &test : load_shift_tests) {
      double rate = test.rate / (double) total_agents;
      std::exponential_distribution<double> rd(
          1.0 / (1000000.0 / (rate / static_cast<double>(threads))));
      auto work = GenerateWork(std::bind(rd, rg), last_us,
                               last_us + test.duration, true,
                               test.mem_bound_work_itr);
      last_us = work.back().start_us;
      w_temp.insert(w_temp.end(), work.begin(), work.end());
    }
    return w_temp;
  });

  if (b) {
    if (!b->EndExperiment(w, &csr)) return;
  }

  cs = cstat{csr.offered_rps,
             csr.rps,
             csr.cpu_bound_req_rps,
             csr.mem_bound_req_rps,
             csr.lock_bound_req_rps,
             csr.goodput,
             csr.cpu_bound_req_goodput,
             csr.mem_bound_req_goodput,
             csr.lock_bound_req_goodput,
             csr.min_percli_tput,
             csr.max_percli_tput,
             static_cast<double>(csr.ecredit_rx) / elapsed * 1000000,
             static_cast<double>(csr.cupdate_tx) / elapsed * 1000000,
             static_cast<double>(csr.resp_rx) / elapsed * 1000000,
             static_cast<double>(csr.req_tx) / elapsed * 1000000,
             static_cast<double>(csr.credit_expired) / elapsed * 1000000,
             static_cast<double>(csr.req_dropped) / elapsed * 1000000};

  // Print the results.
  PrintStatResults(w, &cs, &ss);
}

void SteadyStateExperiment(int threads, double offered_rps) {
  struct sstat ss;
  struct cstat_raw csr;
  struct cstat cs;
  double elapsed;

  memset(&csr, 0, sizeof(csr));

  std::vector<work_unit> w = RunExperiment(threads, &csr, &ss, &elapsed,
					   [=] {
    std::mt19937 rg(rand());
    std::exponential_distribution<double> rd(
        1.0 / (1000000.0 / (offered_rps / static_cast<double>(threads))));
    return GenerateWork(std::bind(rd, rg), 0, kExperimentTime, false,
        mem_bound_work_itr);
  },
  [=] {
    std::mt19937 rg(rand());
    std::exponential_distribution<double> rd(
        1.0 / (1000000.0 / (offered_rps / static_cast<double>(threads))));
    return GenerateWork(std::bind(rd, rg), 0, kExperimentTime, true,
        mem_bound_work_itr);
  });

  if (b) {
    if (!b->EndExperiment(w, &csr)) return;
  }

  cs = cstat{csr.offered_rps,
             csr.rps,
             csr.cpu_bound_req_rps,
             csr.mem_bound_req_rps,
             csr.lock_bound_req_rps,
             csr.goodput,
             csr.cpu_bound_req_goodput,
             csr.mem_bound_req_goodput,
             csr.lock_bound_req_goodput,
             csr.min_percli_tput,
             csr.max_percli_tput,
             static_cast<double>(csr.ecredit_rx) / elapsed * 1000000,
             static_cast<double>(csr.cupdate_tx) / elapsed * 1000000,
             static_cast<double>(csr.resp_rx) / elapsed * 1000000,
             static_cast<double>(csr.req_tx) / elapsed * 1000000,
             static_cast<double>(csr.credit_expired) / elapsed * 1000000,
             static_cast<double>(csr.req_dropped) / elapsed * 1000000};
  // Print the results.
  PrintStatResults(w, &cs, &ss);
}

int StringToAddr(const char *str, uint32_t *addr) {
  uint8_t a, b, c, d;

  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;

  *addr = MAKE_IP_ADDR(a, b, c, d);
  return 0;
}

void calculate_rates() {
  offered_loads.push_back(offered_load / (double)total_agents);
}

void AgentHandler(void *arg) {
  master.port = kBarrierPort;
  b = new NetBarrier(master);
  BUG_ON(!b);

  calculate_rates();

  if (do_load_shift) {
    LoadShiftExperiment(threads);
  } else {
    for (double i : offered_loads) {
      SteadyStateExperiment(threads, i);
    }
  }
}

void ClientHandler(void *arg) {
  int pos;

  if (total_agents > 1) {
    b = new NetBarrier(total_agents - 1);
    BUG_ON(!b);
  }

  calculate_rates();

  json_out.open("output.json");
  csv_out.open("output.csv", std::fstream::out | std::fstream::app);
  json_out << "[";

  /* Print Header */
  PrintHeader(std::cout);

  if (do_load_shift) {
    LoadShiftExperiment(threads);
    rt::Sleep(1000000);
  } else {
    for (double i : offered_loads) {
      SteadyStateExperiment(threads, i);
      rt::Sleep(1000000);
    }
  }

  pos = json_out.tellp();
  json_out.seekp(pos - 2);
  json_out << "]";
  json_out.close();
  csv_out.close();
}

}  // anonymous namespace

void print_lb_usage() {
  std::cerr << "usage: [alg] [cfg_file] lb [server_ip #1] [nconn #1]\n"
	    << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
	    << "\tcfg_file: Shenango configuration file\n"
	    << "\tserver_ip: server IP address\n"
	    << "\tnconn: the number of parallel connection to the server"
	    << std:: endl;
}

void print_fo_usage() {
  std::cerr << "usage: [alg] [cfg_file] fo [server_ip #1] [nconn #1]\n"
	    << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
	    << "\tcfg_file: Shenango configuration file\n"
	    << "\tserver_ip: server IP address\n"
	    << "\tnconn: the number of parallel connection to the server"
	    << std:: endl;
}

void print_seq_usage() {
  std::cerr << "usage: [alg] [cfg_file] seq [server_ip #1] [nconn #1]\n"
	    << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
	    << "\tcfg_file: Shenango configuration file\n"
	    << "\tserver_ip: server IP address\n"
	    << "\tnconn: the number of parallel connection to the server"
	    << std:: endl;
}

void print_client_usage() {
  std::cerr << "usage: [alg] [cfg_file] client [nclients] "
      << "[cpu_bound_work_itr] [mem_bound_work_itr] [lock_bound_work_itr]"
      << "[req_mix] [slo] [nagents] "
      << "[offered_load] [server_ip #1] [nconn #1] ...\n"
      << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
      << "\tcfg_file: Shenango configuration file\n"
      << "\tnclients: the number of client connections\n"
      << "\tservice_us: average request processing time (in us)\n"
      << "\tservice_type: request processing type (exp/const/bimod/hetero)\n"
      << "\tcpu_bound_work_itr: CPU-bound workload iterations\n"
      << "\tmem_bound_work_itr: Memory-bound workload iterations\n"
      << "\tlock_bound_work_itr: Lock-bound workload iterations\n"
      << "\treq_mix: Request mix for different types of requests (should add up to 100%)\n"
      << "\tslo: RPC service level objective (in us)\n"
      << "\tnagents: the number of agents\n"
      << "\toffered_load: load geneated by client and agents in requests per second\n"
      << "\tserver_ip: server IP address\n"
      << "\tnconn: the number of parallel connection to the server"
      << std::endl;
}

int main(int argc, char *argv[]) {
  int ret, i;

  if (argc < 4) {
    std::cerr << "usage: [alg] [cfg_file] [cmd] ...\n"
	      << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
	      << "\tcfg_file: Shenango configuration file\n"
	      << "\tcmd: netbenchd command (server/client/agent)" << std::endl;
    return -EINVAL;
  }

  std::string olc = argv[1]; // overload control
  if (olc.compare("breakwater") == 0) {
    crpc_ops = &cbw_ops;
    srpc_ops = &sbw_ops;
  } else if (olc.compare("protego") == 0) {
    crpc_ops = &cbw_ops;
    srpc_ops = &sbw2_ops;
  } else if (olc.compare("pcc") == 0) {
    crpc_ops = &cpcc_ops;
    srpc_ops = &spcc_ops;
  } else if (olc.compare("seda") == 0) {
    crpc_ops = &csd_ops;
    srpc_ops = &ssd_ops;
  } else if (olc.compare("dagor") == 0) {
    crpc_ops = &cdg_ops;
    srpc_ops = &sdg_ops;
  } else if (olc.compare("nocontrol") == 0) {
    crpc_ops = &cnc_ops;
    srpc_ops = &snc_ops;
  } else {
    std::cerr << "invalid algorithm: " << olc << std::endl;
    std::cerr << "usage: [alg] [cfg_file] [cmd] ...\n"
	      << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
	      << "\tcfg_file: Shenango configuration file\n"
	      << "\tcmd: netbenchd command (server/client/agent)" << std::endl;
    return -EINVAL;
  }

  std::string cmd = argv[3];
  if (cmd.compare("server") == 0) {
    // Server
    if (argc < 5) {
      printf("usage: [alg] [cfg_file] server [msem/no_msem]\n");
        return -EINVAL;
    }
    std::string use_msem_arg = argv[4]; // memory semaphore setting
    if (use_msem_arg.compare("msem") == 0) {
      use_msem = true;
    } else if (use_msem_arg.compare("no_msem") == 0) {
      use_msem = false;
    } else {
      printf("usage: [alg] [cfg_file] server [msem/no_msem]\n");
      return -EINVAL;
    }

    ret = runtime_init(argv[2], ServerHandler, NULL);
    if (ret) {
      printf("failed to start runtime\n");
      return ret;
    }
  } else if (cmd.compare("agent") == 0) {
    // Agent
    if (argc < 5 || StringToAddr(argv[4], &master.ip)) {
      std::cerr << "usage: [alg] [cfg_file] agent [client_ip]\n"
	        << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
		<< "\tcfg_file: Shenango configuration file\n"
		<< "\tclient_ip: Client IP address" << std::endl;
      return -EINVAL;
    }

    ret = runtime_init(argv[2], AgentHandler, NULL);
    if (ret) {
      printf("failed to start runtime\n");
      return ret;
    }
  } else if (cmd.compare("lb") == 0) {
    // Load-Balancer
    if (argc < 6) {
      print_lb_usage();
      return -EINVAL;
    }

    num_servers = argc - 4;
    if (num_servers % 2 != 0) {
      print_lb_usage();
      return -EINVAL;
    }
    num_servers /= 2;

    for (i = 0; i < num_servers; ++i) {
      ret = StringToAddr(argv[4+2*i], &raddr[i].ip);
      if (ret) {
        std::cerr << "[Error] Cannot parse server IP: " << argv[4+2*i]
		  << std::endl;
	return -EINVAL;
      }
      raddr[i].port = kNetbenchPort;
      nconn[i] = std::stoi(argv[5+2*i], nullptr, 0);
    }

    ret = runtime_init(argv[2], LBHandler, NULL);
    if (ret) {
      std::cerr << "[Error] Failed to start runtime" << std::endl;
      return ret;
    }
  } else if (cmd.compare("fo") == 0) {
    // Load-Balancer
    if (argc < 6) {
      print_fo_usage();
      return -EINVAL;
    }

    num_servers = argc - 4;
    if (num_servers % 2 != 0) {
      print_fo_usage();
      return -EINVAL;
    }
    num_servers /= 2;

    for (i = 0; i < num_servers; ++i) {
      ret = StringToAddr(argv[4+2*i], &raddr[i].ip);
      if (ret) {
        std::cerr << "[Error] Cannot parse server IP: " << argv[4+2*i]
		  << std::endl;
	return -EINVAL;
      }
      raddr[i].port = kNetbenchPort;
      nconn[i] = std::stoi(argv[5+2*i], nullptr, 0);
    }

    ret = runtime_init(argv[2], FOHandler, NULL);
    if (ret) {
      std::cerr << "[Error] Failed to start runtime" << std::endl;
      return ret;
    }
  } else if (cmd.compare("seq") == 0) {
    // Load-Balancer
    if (argc < 6) {
      print_seq_usage();
      return -EINVAL;
    }

    num_servers = argc - 4;
    if (num_servers % 2 != 0) {
      print_seq_usage();
      return -EINVAL;
    }
    num_servers /= 2;

    for (i = 0; i < num_servers; ++i) {
      ret = StringToAddr(argv[4+2*i], &raddr[i].ip);
      if (ret) {
        std::cerr << "[Error] Cannot parse server IP: " << argv[4+2*i]
		  << std::endl;
	return -EINVAL;
      }
      raddr[i].port = kNetbenchPort;
      nconn[i] = std::stoi(argv[5+2*i], nullptr, 0);
    }

    ret = runtime_init(argv[2], SEQHandler, NULL);
    if (ret) {
      std::cerr << "[Error] Failed to start runtime" << std::endl;
      return ret;
    }
  } else if (cmd.compare("client") != 0) {
    std::cerr << "invalid command: " << cmd << std::endl;
    std::cerr << "usage: [alg] [cfg_file] [cmd] ...\n"
	      << "\talg: overload control algorithms (breakwater/seda/dagor)\n"
	      << "\tcfg_file: Shenango configuration file\n"
	      << "\tcmd: netbenchd command (server/client/agent)" << std::endl;
    return -EINVAL;
  }

  if (argc < 14) {
    print_client_usage();
    return -EINVAL;
  }

  threads = std::stoi(argv[4], nullptr, 0);
  cpu_bound_work_itr = std::stoi(argv[5], nullptr, 0);
  mem_bound_work_itr = std::stoi(argv[6], nullptr, 0);
  lock_bound_work_itr = std::stoi(argv[7], nullptr, 0);

  std::string token;
  std::string req_mix_str = argv[8];
  std::stringstream ss(req_mix_str);
  while (std::getline(ss, token, ',')) {
	  std::string key;
	  double value;
	  std::stringstream pair(token);
	  if (std::getline(pair, key, ':')) {
		  std::string value_str;
		  if (std::getline(pair, value_str)) {
			  value = std::stod(value_str);
			  if (key == "cpu") req_mix[CPU_BOUND] = value;
			  else if (key == "mem") req_mix[MEM_BOUND] = value;
			  else if (key == "lock") req_mix[LOCK_BOUND] = value;
			  else {
				  printf("incorrect req_mix\n");;
				  return -EINVAL;
			  }
		  } else {
			  printf("incorrect req_mix\n");;
			  return -EINVAL;
		  }
	  } else {
		  printf("incorrect req_mix\n");;
		  return -EINVAL;
	  }
  }
  double req_mix_sum = 0;
  for (int i = 0; i < NUM_REQUEST_TYPES; ++i) {
	  req_mix_sum += req_mix[i];
  }
  if (req_mix_sum != 100) {
	  printf("incorrect req_mix\n");;
	  return -EINVAL;
  }

  slo = std::stoi(argv[9], nullptr, 0);
  total_agents += std::stoi(argv[10], nullptr, 0);
  offered_load = std::stod(argv[11], nullptr);
  std::string load_shift_arg = argv[12];
  if (load_shift_arg == "no_load_shift") {
      do_load_shift = 0;
  } else if (load_shift_arg == "load_shift") {
      do_load_shift = 1;
  } else {
      std::cerr << "Invalid load shift argument\n";
      return -EINVAL;
  }

  num_servers = argc - 13;
  if (num_servers % 2 != 0) {
    print_client_usage();
    return -EINVAL;
  }
  num_servers /= 2;

  if (num_servers > 16) {
    std::cerr << "[Warning] the number of server exceeds 16."
	      << std::endl;
    num_servers = 16;
  }

  for(i = 0; i < num_servers; ++i) {
    int nconn_;

    ret = StringToAddr(argv[13+2*i], &raddr[i].ip);
    if (ret) {
      std::cerr << "[Error] Cannot parse server IP:" << argv[13+2*i]
	        << std::endl;
      return -EINVAL;
    }
    raddr[i].port = kNetbenchPort;

    nconn_ = std::stoi(argv[14+2*i], nullptr, 0);
    if (nconn_ > 16) {
      std::cerr << "[Warning] the number of parallel connection exceeds 16."
	        << std::endl;
      nconn_ = 16;
    }
    nconn[i] = nconn_;
  }

  ret = runtime_init(argv[2], ClientHandler, NULL);
  if (ret) {
    printf("failed to start runtime\n");
    return ret;
  }

  return 0;
}
