extern "C" {
#include <base/time.h>
#include <base/log.h>
#include <net/ip.h>
#include <runtime/smalloc.h>
#include <unistd.h>
#include <breakwater/breakwater.h>
#include <breakwater/seda.h>
#include <breakwater/dagor.h>
#include <breakwater/nocontrol.h>
}

#include "cc/net.h"
#include "cc/runtime.h"
#include "cc/sync.h"
#include "cc/thread.h"
#include "cc/timer.h"
#include "proto.h"
#include "breakwater/rpc++.h"

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
constexpr int kValueLen = 5;

const struct crpc_ops *crpc_ops;
const struct srpc_ops *srpc_ops;

namespace {

    using namespace std::chrono;
    using sec = duration<double, std::micro>;

    // <- ARGUMENTS FOR EXPERIMENT ->
    // the number of worker threads to spawn.
    int threads;
    // the remote UDP address of the server.
    netaddr raddr, master;
    int wtype;
    // RPC service level objective (in us)
    int slo;
    // maximum key index
    int max_key_idx;
    // Short (CPU-bound) key size
    int skey_size = 0;
    // Short (CPU-bound) key count
    int skey_count = 0;
    // Large (Memory-bound) key size
    int lkey_size = 0;
    // Large (Memory-bound) key count
    int lkey_count = 0;
    // Short (CPU-bound) key percentage
    int skey_pcnt = 0;

    std::ofstream json_out;
    std::ofstream csv_out;

    int total_agents = 1;
    // Total duration of the experiment in us
    constexpr uint64_t kWarmUpTime = 4000000;
    constexpr uint64_t kExperimentTime = 8000000;
    // RTT
    constexpr uint64_t kRTT = 10;

    std::vector<double> offered_loads;
    double offered_load;

    /* server-side stat */
    constexpr uint64_t kRPCSStatPort = 8002;
    constexpr uint64_t kRPCSStatMagic = 0xDEADBEEF;
    struct sstat_raw {
        uint64_t total;
        uint64_t busy;
        uint64_t mem_accesses;
        unsigned int num_cores;
        unsigned int max_cores;
        uint64_t winu_rx;
        uint64_t winu_tx;
        uint64_t win_tx;
        uint64_t req_rx;
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
        double rx_pps;
        double tx_pps;
        double rx_bps;
        double tx_bps;
        double rx_drops_pps;
        double rx_ooo_pps;
        double winu_rx_pps;
        double winu_tx_pps;
        double win_tx_wps;
        double req_rx_pps;
        double resp_tx_pps;
    };

    /* client-side stat */
    struct cstat_raw {
        double offered_rps;
        double rps;
        double set_rps;
        double skey_rps;
        double lkey_rps;
        double goodput;
        double set_goodput;
        double skey_goodput;
        double lkey_goodput;
        double min_percli_tput;
        double max_percli_tput;
        uint64_t winu_rx;
        uint64_t winu_tx;
        uint64_t resp_rx;
        uint64_t req_tx;
        uint64_t win_expired;
        uint64_t req_dropped;
    };

    struct cstat {
        double offered_rps;
        double rps;
        double set_rps;
        double skey_rps;
        double lkey_rps;
        double goodput;
        double set_goodput;
        double skey_goodput;
        double lkey_goodput;
        double min_percli_tput;
        double max_percli_tput;
        double winu_rx_pps;
        double winu_tx_pps;
        double resp_rx_pps;
        double req_tx_pps;
        double win_expired_wps;
        double req_dropped_rps;
    };

    struct work_unit {
        double start_us, duration_us, latency_us;
        int hash;
        bool success;
        uint64_t window;
        uint64_t tsc;
        uint32_t cpu;
        uint64_t server_queue;
        uint64_t client_queue;
        bool is_skey;
        bool is_set;
        int key;
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
            for (int i = 1; i <= npeers; i++) {
                rt::TcpConn *c = q->Accept();
                if (c == nullptr) panic("couldn't accept a connection");
                conns.emplace_back(c);
                BUG_ON(c->WriteFull(&threads, sizeof(threads)) <= 0);
                BUG_ON(c->WriteFull(&raddr, sizeof(raddr)) <= 0);
                BUG_ON(c->WriteFull(&wtype, sizeof(wtype)) <= 0);
                BUG_ON(c->WriteFull(&total_agents, sizeof(total_agents)) <= 0);
                BUG_ON(c->WriteFull(&slo, sizeof(slo)) <= 0);
                BUG_ON(c->WriteFull(&max_key_idx, sizeof(max_key_idx)) <= 0);
                BUG_ON(c->WriteFull(&skey_size, sizeof(skey_size)) <= 0);
                BUG_ON(c->WriteFull(&skey_count, sizeof(skey_count)) <= 0);
                BUG_ON(c->WriteFull(&lkey_size, sizeof(lkey_size)) <= 0);
                BUG_ON(c->WriteFull(&lkey_count, sizeof(lkey_count)) <= 0);
                BUG_ON(c->WriteFull(&skey_pcnt, sizeof(skey_pcnt)) <= 0);
                BUG_ON(c->WriteFull(&offered_load, sizeof(offered_load)) <= 0);
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
            BUG_ON(c->ReadFull(&raddr, sizeof(raddr)) <= 0);
            BUG_ON(c->ReadFull(&wtype, sizeof(wtype)) <= 0);
            BUG_ON(c->ReadFull(&total_agents, sizeof(total_agents)) <= 0);
            BUG_ON(c->ReadFull(&slo, sizeof(slo)) <= 0);
            BUG_ON(c->ReadFull(&max_key_idx, sizeof(max_key_idx)) <= 0);
            BUG_ON(c->ReadFull(&skey_size, sizeof(skey_size)) <= 0);
            BUG_ON(c->ReadFull(&skey_count, sizeof(skey_count)) <= 0);
            BUG_ON(c->ReadFull(&lkey_size, sizeof(lkey_size)) <= 0);
            BUG_ON(c->ReadFull(&lkey_count, sizeof(lkey_count)) <= 0);
            BUG_ON(c->ReadFull(&skey_pcnt, sizeof(skey_pcnt)) <= 0);
            BUG_ON(c->ReadFull(&offered_load, sizeof(offered_load)) <= 0);
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
                    csr->set_rps += rem_csr.set_rps;
                    csr->skey_rps += rem_csr.skey_rps;
                    csr->lkey_rps += rem_csr.lkey_rps;
                    csr->goodput += rem_csr.goodput;
                    csr->set_goodput += rem_csr.set_goodput;
                    csr->skey_goodput += rem_csr.skey_goodput;
                    csr->lkey_goodput += rem_csr.lkey_goodput;
                    csr->min_percli_tput = MIN(rem_csr.min_percli_tput,
                                               csr->min_percli_tput);
                    csr->max_percli_tput = MAX(rem_csr.max_percli_tput,
                                               csr->max_percli_tput);
                    csr->winu_rx += rem_csr.winu_rx;
                    csr->winu_tx += rem_csr.winu_tx;
                    csr->resp_rx += rem_csr.resp_rx;
                    csr->req_tx += rem_csr.req_tx;
                    csr->win_expired += rem_csr.win_expired;
                    csr->req_dropped += rem_csr.req_dropped;
                }
            } else {
                BUG_ON(conns[0]->WriteFull(csr, sizeof(*csr)) <= 0);
            }
            GatherSamples(w);
            BUG_ON(!Barrier());
            return is_leader_;
        }

        bool IsLeader() {
            return is_leader_;
        }

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
                                    BUG_ON(agg_conns_[i]->WriteFull(start, sizeof(work_unit) * elems)
                                           <= 0);
                            }));
                }
                for (auto &t : th) t.Join();
            }
        }
    };

    static NetBarrier *b;

    sstat_raw ReadRPCSStat() {
        std::unique_ptr<rt::TcpConn> c(
            rt::TcpConn::Dial({0, 0}, {raddr.ip, kRPCSStatPort}));
        uint64_t magic = hton64(kRPCSStatMagic);
        ssize_t ret = c->WriteFull(&magic, sizeof(magic));
        if (ret != static_cast<ssize_t>(sizeof(magic)))
            panic("sstat request failed, ret = %ld", ret);
        sstat_raw u;
        ret = c->ReadFull(&u, sizeof(u));
        if (ret != static_cast<ssize_t>(sizeof(u)))
            panic("sstat response failed, ret = %ld", ret);
        return sstat_raw{u.total, u.busy, u.mem_accesses, u.num_cores, u.max_cores, u.winu_rx,
                u.winu_tx, u.win_tx, u.req_rx, u.resp_tx};
    }

    shstat_raw ReadShenangoStat() {
        char *buf_;
        std::string buf;
        std::map<std::string, uint64_t> smap;
        std::unique_ptr<rt::TcpConn> c(
            rt::TcpConn::Dial({0,0}, {raddr.ip, kShenangoStatPort}));
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
            if (pos_col == std::string::npos)
                continue;

            key = token.substr(0, pos_col);
            value = std::stoull(token.substr(pos_col+1, pos_com));

            smap[key] = value;

            buf.erase(0, pos_com + 1);
        }

        free(buf_);

        return shstat_raw{smap["rx_packets"], smap["tx_packets"], smap["rx_bytes"],
	            smap["tx_bytes"], smap["drops"], smap["rx_tcp_out_of_order"]};
    }

    // The maximum lateness to tolerate before dropping egress samples.
    constexpr uint64_t kMaxCatchUpUS = 5;

    template <class Arrival>
    std::vector<work_unit> GenerateWork(Arrival a, double cur_us,
                                        double last_us) {
        std::vector<work_unit> w;

        while (true) {
            cur_us += a();
            if (cur_us > last_us) break;

            // Construct the work unit
            work_unit wu;
            wu.start_us = cur_us;
            wu.duration_us = 0;
            wu.latency_us = 0;
            wu.hash = rand();
            wu.success = false;
            wu.is_skey = true;
            wu.is_set = false;

            if (wtype == 1) {   // SET
                wu.is_set = true;
            }

            if (wtype == 3) {   // USR
                if (rand() % 1000 >= 998) {
                    wu.is_set = true;
                }
            }

            if (wtype == 4) {   // BIMOD_GET
                if ((rand() % 100) < skey_pcnt) {
                    wu.key = rand() % skey_count;
                } else {
                    wu.is_skey = false;
                    wu.key = rand() % lkey_count;
                }
            }

            if (wtype == 5) {   // BIMOD_VAR
                if ((rand() % 100) < 82) {
                    // SET request
                    wu.is_set = true;
                    wu.key = rand() % skey_count;
                } else {
                    // GET request
                    if ((rand() % 100) < skey_pcnt) {
                        wu.key = rand() % skey_count;
                    } else {
                        wu.is_skey = false;
                        wu.key = rand() % lkey_count;
                    }
                }
            }

            w.emplace_back(wu);
        }

        return w;
    }

    void GenerateRandomString(char *buffer, int len, uint64_t hash) {
        int i;
        uint64_t tmp_hash = hash;

        for(i = 0; i < len; ++i) {
            buffer[i] = (tmp_hash % 94) + 33;
            tmp_hash = (tmp_hash >> 1);
        }
    }

    std::vector<work_unit> ClientWorker(
        rpc::RpcClient *c, rt::WaitGroup *starter, rt::WaitGroup *starter2,
        std::function<std::vector<work_unit>()> wf) {

        std::vector<work_unit> w(wf());
        std::vector<uint64_t> timings;
        timings.reserve(w.size());

        // Start the receiver thread.
        auto th = rt::Thread([&] {
                // This should use glibc's malloc behind the scenes,
                // hence allocate in the control path
                char resp[4096];

                while (true) {
                    ssize_t ret = c->Recv(resp, 4096, 0, nullptr);

                    struct MemcachedRespHdr *hdr;
                    hdr = reinterpret_cast<struct MemcachedRespHdr *>(resp);
                    ntoh(hdr);

                    if (ret != static_cast<ssize_t>(sizeof(MemcachedRespHdr) + hdr->total_body_length)) {
                        if (ret == 0 || ret < 0) break;
                        panic("read failed, ret = %ld", ret);
                    }

                    uint64_t now = microtime();
                    uint32_t idx = hdr->opaque;
                    w[idx].duration_us = now - timings[idx];
                    w[idx].success = true;
                    w[idx].latency_us = w[idx].duration_us - w[idx].client_queue;
                    w[idx].window = c->Credit();
                    w[idx].tsc = 0;
                    w[idx].cpu = 0;
                    w[idx].server_queue = 0;
                }
            });

        // Synchronized start of load generation.
        starter->Done();
        starter2->Wait();

        // This should use glibc's malloc behind the scenes,
        // hence allocate in the control path

        char buf[4096];
        char value[4096];

        barrier();
        auto expstart = steady_clock::now();
        barrier();

        auto wsize = w.size();
        struct MemcachedReqHdr *hdr;
        int buflen;

        for (unsigned int i = 0; i < wsize; ++i) {
            barrier();
            auto now = steady_clock::now();
            barrier();
            if (duration_cast<sec>(now - expstart).count() < w[i].start_us) {
                rt::Sleep(w[i].start_us - duration_cast<sec>(now - expstart).count());
            }
            if (duration_cast<sec>(now - expstart).count() - w[i].start_us > kMaxCatchUpUS)
                continue;

            timings[i] = microtime();

            if (wtype == 1) { // SET
                std::string key = std::to_string(w[i].hash % max_key_idx);
                GenerateRandomString(value, kValueLen, w[i].hash);
                buflen = ConstructMemcachedSetReq(buf, 4096, i,
                                                  key.c_str(), key.length(),
                                                  value, kValueLen, 1);
            } else if (wtype == 2) { // GET
                std::string key = std::to_string(w[i].hash % max_key_idx);
                buflen = ConstructMemcachedGetReq(buf, 4096, i,
                                                  key.c_str(), key.length(), 1);
            } else if (wtype == 3) { // USR
                std::string key = std::to_string(w[i].hash % max_key_idx);
                if (!w[i].is_set) {
                    buflen = ConstructMemcachedGetReq(buf, 4096, i,
                                                      key.c_str(), key.length(), 1);
                } else {
                    GenerateRandomString(value, kValueLen, w[i].hash);
                    buflen = ConstructMemcachedSetReq(buf, 4096, i,
                                                      key.c_str(), key.length(),
                                                      value, kValueLen, 1);
                }
            } else if (wtype == 4) { // BIMOD_GET
                std::string key = (w[i].is_skey ? "skey-" : "lkey-") + \
                    std::to_string(w[i].key);
                buflen = ConstructMemcachedGetReq(buf, 4096, i,
                                                  key.c_str(), key.length(),
                                                  w[i].is_skey);
            } else if (wtype == 5) { // BIMOD_VAR
                std::string key = (w[i].is_skey ? "skey-" : "lkey-") + \
                    std::to_string(w[i].key);
                if (w[i].is_set) {
                    GenerateRandomString(value, skey_size, w[i].hash);
                    buflen = ConstructMemcachedSetReq(buf, 4096, i,
                                                      key.c_str(), key.length(),
                                                      value, skey_size,
                                                      w[i].is_skey);
                } else {
                    buflen = ConstructMemcachedGetReq(buf, 4096, i,
                                                      key.c_str(), key.length(),
                                                      w[i].is_skey);
                }

            } else {
                panic("unsupported workload type\n");
            }

            hdr = reinterpret_cast<struct MemcachedReqHdr *>(buf);
            hton(hdr);

            // Send an RPC request.
            ssize_t ret = c->Send(buf, buflen, w[i].hash, nullptr);
            if (ret == -ENOBUFS) continue;
            if (ret != static_cast<ssize_t>(buflen))
                panic("write failed, ret = %ld", ret);
        }

        rt::Sleep((int)(kRTT + 2));
        BUG_ON(c->Shutdown(SHUT_RDWR));
        th.Join();

        return w;
    }

    std::vector<work_unit> RunExperiment(
        int threads, struct cstat_raw *csr, struct sstat *ss, double *elapsed,
        std::function<std::vector<work_unit>()> wf) {

        std::vector<std::unique_ptr<rpc::RpcClient>> conns;

        // Create one TCP connection per thread.
        for (int i = 0; i < threads; ++i) {
            struct rpc_session_info info = {.session_type = 0};
            std::unique_ptr<rpc::RpcClient> outc(rpc::RpcClient::Dial(raddr, i+1, nullptr, nullptr, &info));
            if (unlikely(outc == nullptr)) panic("couldn't connect to raddr.");
            conns.emplace_back(std::move(outc));
        }

        // Launch a worker thread for each connection.
        rt::WaitGroup starter(threads);
        rt::WaitGroup starter2(1);

        std::vector<rt::Thread> th;
        std::unique_ptr<std::vector<work_unit>> samples[threads];
        for (int i = 0; i < threads; ++i) {
            th.emplace_back(rt::Thread([&, i] {
                        auto v = ClientWorker(conns[i].get(), &starter, &starter2, wf);
                        samples[i].reset(new std::vector<work_unit>(std::move(v)));
                    }));
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
        sstat_raw s1, s2;
        shstat_raw sh1, sh2;

        if (!b || b->IsLeader()) {
            s1 = ReadRPCSStat();
            sh1 = ReadShenangoStat();
        }

        // Wait for the workers to finish.
        for (auto &t : th) t.Join();

        // |--- end experiment duration timing ---|
        barrier();
        auto finish = steady_clock::now();
        barrier();

        if (!b || b->IsLeader()) {
            s2 = ReadRPCSStat();
            sh2 = ReadShenangoStat();
        }

        // Force the connections to close.
        for (auto &c : conns) c->Abort();

        double elapsed_ = duration_cast<sec>(finish - start).count();
        elapsed_ -= kWarmUpTime;

        // Aggregate client stats
        if (csr) {
            for (auto &c : conns) {
                csr->winu_rx += c->StatEcreditRx();
                csr->winu_tx += c->StatCupdateTx();
                csr->resp_rx += c->StatRespRx();
                csr->req_tx += c->StatReqTx();
                csr->win_expired += c->StatCreditExpired();
                csr->req_dropped += c->StatReqDropped();
                c->Close();
            }
        }

        // Aggregate all the samples together.
        std::vector<work_unit> w;
        double min_throughput = 0.0;
        double max_throughput = 0.0;
        uint64_t good_resps = 0;
        uint64_t resps = 0;
        uint64_t set_resps = 0;
        uint64_t skey_resps = 0;
        uint64_t lkey_resps = 0;
        uint64_t set_good_resps = 0;
        uint64_t skey_good_resps = 0;
        uint64_t lkey_good_resps = 0;
        uint64_t offered = 0;
        uint64_t client_drop = 0;

        for (int i = 0; i < threads; ++i) {
            auto &v = *samples[i];
            double throughput;
            int slo_success;
            int set_slo_success;
            int skey_slo_success;
            int lkey_slo_success;
            int resp_success;
            int set_resp_success;
            int skey_resp_success;
            int lkey_resp_success;

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
            set_resp_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
                    return s.success && s.is_set;
                });
            skey_resp_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
                    return s.success && !s.is_set && s.is_skey;
                });
            lkey_resp_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
                    return s.success && !s.is_set && !s.is_skey;
                });
            slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
                    return s.success && s.duration_us < slo;
                });
            set_slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
                    return s.success && s.is_set && s.duration_us < slo;
                });
            skey_slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
                    return s.success && !s.is_set && s.is_skey && s.duration_us < slo;
                });
            lkey_slo_success = std::count_if(v.begin(), v.end(), [](const work_unit &s) {
                    return s.success && !s.is_set && !s.is_skey && s.duration_us < slo;
                });

            resps += resp_success;
            set_resps += set_resp_success;
            skey_resps += skey_resp_success;
            lkey_resps += lkey_resp_success;
            good_resps += slo_success;
            set_good_resps += set_slo_success;
            skey_good_resps += skey_slo_success;
            lkey_good_resps += lkey_slo_success;

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
            csr->set_rps = static_cast<double>(set_resps) / elapsed_ * 1000000;
            csr->skey_rps = static_cast<double>(skey_resps) / elapsed_ * 1000000;
            csr->lkey_rps = static_cast<double>(lkey_resps) / elapsed_ * 1000000;
            csr->goodput = static_cast<double>(good_resps) / elapsed_ * 1000000;
            csr->set_goodput = static_cast<double>(set_good_resps) / elapsed_ * 1000000;
            csr->skey_goodput = static_cast<double>(skey_good_resps) / elapsed_ * 1000000;
            csr->lkey_goodput = static_cast<double>(lkey_good_resps) / elapsed_ * 1000000;
            csr->min_percli_tput = min_throughput;
            csr->max_percli_tput = max_throughput;
        }

        if ((!b || b->IsLeader()) && ss) {
            uint64_t total = s2.total - s1.total;
            uint64_t busy = s2.busy - s1.busy;
            ss->cpu_usage = static_cast<double>(busy) / static_cast<double>(total);

	    uint64_t mem_accesses = s2.mem_accesses - s1.mem_accesses;
	    ss->membw_usage = static_cast<double>(mem_accesses) / elapsed_ * 1000000;

            uint64_t winu_rx_pkts = s2.winu_rx - s1.winu_rx;
            uint64_t winu_tx_pkts = s2.winu_tx - s1.winu_tx;
            uint64_t win_tx_wins = s2.win_tx - s1.win_tx;
            uint64_t req_rx_pkts = s2.req_rx - s1.req_rx;
            uint64_t resp_tx_pkts = s2.resp_tx - s1.resp_tx;
            ss->winu_rx_pps = static_cast<double>(winu_rx_pkts) / elapsed_ * 1000000;
            ss->winu_tx_pps = static_cast<double>(winu_tx_pkts) / elapsed_ * 1000000;
            ss->win_tx_wps = static_cast<double>(win_tx_wins) / elapsed_ * 1000000;
            ss->req_rx_pps = static_cast<double>(req_rx_pkts) / elapsed_ * 1000000;
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

    void PrintHeader(std::ostream& os) {
        os << "num_threads," << "offered_load," << "throughput," << "set_throughput," << "skey_throughput,"
           << "lkey_throughput," << "goodput," << "set_goodput," << "skey_goodput," << "lkey_goodput," << "cpu," << "membw," << "min,"
           << "mean," << "set_mean," << "skey_mean," << "lkey_mean," << "p50," << "set_p50," << "skey_p50," << "lkey_p50,"
           << "p90," << "set_p90," << "skey_p90," << "lkey_p90,"
           << "p99," << "set_p99," << "skey_p99," << "lkey_p99," << "p999," << "p9999," << "max,"
           << "lmin," << "lmean," << "lp50," << "lp90," << "lp99," << "lp999," << "lp9999,"
           << "lmax," << "p1_win," << "mean_win," << "p99_win," << "p1_q," << "mean_q,"
           << "p99_q," << "server:rx_pps," << "server:tx_pps," << "server:rx_bps,"
           << "server:tx_bps," << "server:rx_drops_pps," << "server:rx_ooo_pps," << "server:winu_rx_pps,"
           << "server:winu_tx_pps," << "server:win_tx_wps," << "server:req_rx_pps,"
           << "server:resp_tx_pps," << "client:min_tput," << "client:max_tput,"
           << "client:winu_rx_pps," << "client:winu_tx_pps," << "client:resp_rx_pps,"
           << "client:req_tx_pps," << "client:win_expired_wps," << "client:req_dropped_rps"
           << std::endl;
    }

    void PrintStatResults(std::vector<work_unit> w, struct cstat *cs,
                          struct sstat *ss) {
        if (w.size() == 0) {
            std::cout << std::setprecision(4) << std::fixed << threads * total_agents
                      << "," << cs->offered_rps << "," << "-" << std::endl;
            return;
        }

        // Remove unsuccessful requests
        w.erase(std::remove_if(w.begin(), w.end(),
                               [](const work_unit &s) {
                                   return !s.success;
                               }), w.end());

        // Get the p99 times for short and long requests separately
        std::vector<work_unit> set_work;
        std::vector<work_unit> skey_work;
        std::vector<work_unit> lkey_work;
        double set_work_count = 0;
        double set_mean = 0;
        double set_p50 = 0;
        double set_p90 = 0;
        double set_p99 = 0;
        double skey_work_count = 0;
        double skey_mean = 0;
        double skey_p50 = 0;
        double skey_p90 = 0;
        double skey_p99 = 0;
        double lkey_work_count = 0;
        double lkey_mean = 0;
        double lkey_p50 = 0;
        double lkey_p90 = 0;
        double lkey_p99 = 0;

        // For set
        std::copy_if(w.begin(), w.end(), std::back_inserter(set_work), [](work_unit &s) {
                return s.success && s.is_set;
            });
        std::sort(set_work.begin(), set_work.end(),
                  [](const work_unit &s1, const work_unit &s2) {
                      return s1.duration_us < s2.duration_us;
                  });
        double set_sum = std::accumulate(
            set_work.begin(), set_work.end(), 0.0,
            [](double s, const work_unit &c) { return s + c.duration_us; });

        // For skey
        std::copy_if(w.begin(), w.end(), std::back_inserter(skey_work), [](work_unit &s) {
                return s.success && !s.is_set && s.is_skey;
            });
        std::sort(skey_work.begin(), skey_work.end(),
                  [](const work_unit &s1, const work_unit &s2) {
                      return s1.duration_us < s2.duration_us;
                  });
        double skey_sum = std::accumulate(
            skey_work.begin(), skey_work.end(), 0.0,
            [](double s, const work_unit &c) { return s + c.duration_us; });

        // For lkey
        std::copy_if(w.begin(), w.end(), std::back_inserter(lkey_work), [](work_unit &s) {
                return s.success && !s.is_set && !s.is_skey;
            });
        std::sort(lkey_work.begin(), lkey_work.end(),
                  [](const work_unit &s1, const work_unit &s2) {
                      return s1.duration_us < s2.duration_us;
                  });
        double lkey_sum = std::accumulate(
            lkey_work.begin(), lkey_work.end(), 0.0,
            [](double s, const work_unit &c) { return s + c.duration_us; });

        set_work_count = static_cast<double>(set_work.size());
        if (set_work_count) {
            set_mean = set_sum / set_work_count;
            set_p50 = set_work[set_work_count * 0.50].duration_us;
            set_p90 = set_work[set_work_count * 0.90].duration_us;
            set_p99 = set_work[set_work_count * 0.99].duration_us;
        }
        skey_work_count = static_cast<double>(skey_work.size());
        if (skey_work_count) {
            skey_mean = skey_sum / skey_work_count;
            skey_p50 = skey_work[skey_work_count * 0.50].duration_us;
            skey_p90 = skey_work[skey_work_count * 0.90].duration_us;
            skey_p99 = skey_work[skey_work_count * 0.99].duration_us;
        }
        lkey_work_count = static_cast<double>(lkey_work.size());
        if (lkey_work_count) {
            lkey_mean = lkey_sum / lkey_work_count;
            lkey_p50 = lkey_work[lkey_work_count * 0.50].duration_us;
            lkey_p90 = lkey_work[lkey_work_count * 0.90].duration_us;
            lkey_p99 = lkey_work[lkey_work_count * 0.99].duration_us;
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
                return s1.latency_us < s2.latency_us;
            });
        double lsum = std::accumulate(
            w.begin(), w.end(), 0.0,
            [](double s, const work_unit &c) { return s + c.latency_us; });
        double lmean = lsum / w.size();
        double lp50 = w[count * 0.5].latency_us;
        double lp90 = w[count * 0.9].latency_us;
        double lp99 = w[count * 0.99].latency_us;
        double lp999 = w[count * 0.999].latency_us;
        double lp9999 = w[count * 0.9999].latency_us;
        double lmin = w[0].latency_us;
        double lmax = w[w.size() - 1].latency_us;

        std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
                return s1.window < s2.window;
            });
        double sum_win = std::accumulate(
            w.begin(), w.end(), 0.0,
            [](double s, const work_unit &c) { return s + c.window; });
        double mean_win = sum_win / w.size();
        double p1_win = w[count * 0.01].window;
        double p99_win = w[count * 0.99].window;

        std::sort(w.begin(), w.end(), [](const work_unit &s1, const work_unit &s2) {
                return s1.server_queue < s2.server_queue;
            });
        double sum_que = std::accumulate(
            w.begin(), w.end(), 0.0,
            [](double s, const work_unit &c) { return s + c.server_queue; });
        double mean_que = sum_que / w.size();
        double p1_que = w[count * 0.01].server_queue;
        double p99_que = w[count * 0.99].server_queue;

        double sum_cque = std::accumulate(
            w.begin(), w.end(), 0.0,
            [](double s, const work_unit &c) {return s + c.client_queue; });
        double mean_cque = sum_cque / w.size();

        std::cout << std::setprecision(4) << std::fixed << threads * total_agents << ","
                  << cs->offered_rps << "," << cs->rps << "," << cs->set_rps << "," << cs->skey_rps << ","
                  << cs->lkey_rps << "," << cs->goodput << "," << cs->set_goodput << "," << cs->skey_goodput << ","
                  << cs->lkey_goodput << "," << ss->cpu_usage << "," << ss->membw_usage << ","
                  << min << "," << mean << "," << set_mean << "," << skey_mean << "," << lkey_mean << ","
                  << p50 << "," << set_p50 << "," << skey_p50 << ","
                  << lkey_p50 << "," << p90 << "," << set_p90 << "," << skey_p90 << "," << lkey_p90 << ","
                  << p99 << "," << set_p99 << "," << skey_p99 << "," << lkey_p99 << ","
                  << p999 << "," << p9999 << "," << max << "," << lmin << "," << lmean << ","
                  << lp50 << "," << lp90 << "," << lp99 << "," << lp999 << ","
                  << lp9999 << "," << lmax << "," << p1_win << ","
                  << mean_win << "," << p99_win << "," << p1_que << "," << mean_que << ","
                  << p99_que << "," << ss->rx_pps << "," << ss->tx_pps << ","
                  << ss->rx_bps << "," << ss->tx_bps << "," << ss->rx_drops_pps << ","
                  << ss->rx_ooo_pps << "," << ss->winu_rx_pps << "," << ss->winu_tx_pps << ","
                  << ss->win_tx_wps << "," << ss->req_rx_pps << "," << ss->resp_tx_pps << ","
                  << cs->min_percli_tput << "," << cs->max_percli_tput << ","
                  << mean_cque << "," << cs->winu_rx_pps << "," << cs->resp_rx_pps << ","
                  << cs->req_tx_pps << "," << cs->win_expired_wps << ","
                  << cs->req_dropped_rps << std::endl;

        csv_out << std::setprecision(4) << std::fixed << threads * total_agents << ","
                << cs->offered_rps << "," << cs->rps << "," << cs->set_rps << "," << cs->skey_rps << ","
                << cs->lkey_rps << "," << cs->goodput << "," << cs->set_goodput << "," << cs->skey_goodput << ","
                << cs->lkey_goodput << "," << ss->cpu_usage << "," << ss->membw_usage << ","
                << min << "," << mean << "," << set_mean << "," << skey_mean << "," << lkey_mean << ","
                << p50 << "," << set_p50 << "," << skey_p50 << ","
                << lkey_p50 << "," << p90 << "," << set_p90 << "," << skey_p90 << "," << lkey_p90 << ","
                << p99 << "," << set_p99 << "," << skey_p99 << "," << lkey_p99 << ","
                << p999 << "," << p9999 << "," << max << "," << lmin << "," << lmean << ","
                << lp50 << "," << lp90 << "," << lp99 << "," << lp999 << ","
                << lp9999 << "," << lmax << "," << p1_win << ","
                << mean_win << "," << p99_win << "," << p1_que << "," << mean_que << ","
                << p99_que << "," << ss->rx_pps << "," << ss->tx_pps << ","
                << ss->rx_bps << "," << ss->tx_bps << "," << ss->rx_drops_pps << ","
                << ss->rx_ooo_pps << "," << ss->winu_rx_pps << "," << ss->winu_tx_pps << ","
                << ss->win_tx_wps << "," << ss->req_rx_pps << "," << ss->resp_tx_pps << ","
                << cs->min_percli_tput << "," << cs->max_percli_tput << ","
                << mean_cque << "," << cs->winu_rx_pps << "," << cs->resp_rx_pps << ","
                << cs->req_tx_pps << "," << cs->win_expired_wps << ","
                << cs->req_dropped_rps << std::endl;


        json_out << "{"
                 << "\"num_threads\":" << threads * total_agents << ","
                 << "\"offered_load\":" << cs->offered_rps << ","
                 << "\"throughput\":" << cs->rps << ","
                 << "\"set_throughput\":" << cs->set_rps << ","
                 << "\"skey_throughput\":" << cs->skey_rps << ","
                 << "\"lkey_throughput\":" << cs->lkey_rps << ","
                 << "\"goodput\":" << cs->goodput << ","
                 << "\"set_goodput\":" << cs->set_goodput << ","
                 << "\"skey_goodput\":" << cs->skey_goodput << ","
                 << "\"lkey_goodput\":" << cs->lkey_goodput << ","
                 << "\"cpu\":" << ss->cpu_usage << ","
                 << "\"membw\":" << ss->membw_usage << ","
                 << "\"min\":" << min << ","
                 << "\"mean\":" << mean << ","
                 << "\"set_mean\":" << set_mean << ","
                 << "\"skey_mean\":" << skey_mean << ","
                 << "\"lkey_mean\":" << lkey_mean << ","
                 << "\"p50\":" << p50 << ","
                 << "\"set_p50\":" << set_p50 << ","
                 << "\"skey_p50\":" << skey_p50 << ","
                 << "\"lkey_p50\":" << lkey_p50 << ","
                 << "\"p90\":" << p90 << ","
                 << "\"set_p90\":" << set_p90 << ","
                 << "\"skey_p90\":" << skey_p90 << ","
                 << "\"lkey_p90\":" << lkey_p90 << ","
                 << "\"p99\":" << p99 << ","
                 << "\"set_p99\":" << set_p99 << ","
                 << "\"skey_p99\":" << skey_p99 << ","
                 << "\"lkey_p99\":" << lkey_p99 << ","
                 << "\"p999\":" << p999 << ","
                 << "\"p9999\":" << p9999 << ","
                 << "\"max\":" << max << ","
                 << "\"lmin\":" << lmin << ","
                 << "\"lmean\":" << lmean << ","
                 << "\"lp50\":" << lp50 << ","
                 << "\"lp90\":" << lp90 << ","
                 << "\"lp99\":" << lp99 << ","
                 << "\"lp999\":" << lp999 << ","
                 << "\"lp9999\":" << lp9999 << ","
                 << "\"lmax\":" << lmax << ","
                 << "\"p1_win\":" << p1_win << ","
                 << "\"mean_win\":" << mean_win << ","
                 << "\"p99_win\":" << p99_win << ","
                 << "\"p1_q\":" << p1_que << ","
                 << "\"mean_q\":" << mean_que << ","
                 << "\"p99_q\":" << p99_que << ","
                 << "\"server:rx_pps\":" << ss->rx_pps << ","
                 << "\"server:tx_pps\":" << ss->tx_pps << ","
                 << "\"server:rx_bps\":" << ss->rx_bps << ","
                 << "\"server:tx_bps\":" << ss->tx_bps << ","
                 << "\"server:rx_drops_pps\":" << ss->rx_drops_pps << ","
                 << "\"server:rx_ooo_pps\":" << ss->rx_ooo_pps << ","
                 << "\"server:winu_rx_pps\":" << ss->winu_rx_pps << ","
                 << "\"server:winu_tx_pps\":" << ss->winu_tx_pps << ","
                 << "\"server:win_tx_wps\":" << ss->win_tx_wps << ","
                 << "\"server:req_rx_pps\":" << ss->req_rx_pps << ","
                 << "\"server:resp_tx_pps\":" << ss->resp_tx_pps << ","
                 << "\"client:min_tput\":" << cs->min_percli_tput << ","
                 << "\"client:max_tput\":" << cs->max_percli_tput << ","
                 << "\"client:mean_q\":" << mean_cque << ","
                 << "\"client:winu_rx_pps\":" << cs->winu_rx_pps << ","
                 << "\"client:winu_tx_pps\":" << cs->winu_tx_pps << ","
                 << "\"client:resp_rx_pps\":" << cs->resp_rx_pps << ","
                 << "\"client:req_tx_pps\":" << cs->req_tx_pps << ","
                 << "\"client:win_expired_wps\":" << cs->win_expired_wps << ","
                 << "\"client:req_dropped_rps\":" << cs->req_dropped_rps
                 << "}," << std::endl << std::flush;
    }

    void SteadyStateExperiment(int threads, double offered_rps) {
        struct sstat ss;
        struct cstat_raw csr;
        struct cstat cs;
        double elapsed;

        memset(&csr, 0, sizeof(csr));
        std::vector<work_unit> w = RunExperiment(threads, &csr, &ss, &elapsed,
                                                 [=]() {
                                                     std::mt19937 rg(rand());
                                                     std::exponential_distribution<double> rd(
                                                         1.0 / (1000000.0 / (offered_rps / static_cast<double>(threads))));
                                                     return GenerateWork(std::bind(rd, rg), 0, kExperimentTime);
                                                 });

        if (b) {
            if (!b->EndExperiment(w, &csr))
                return;
        }

        cs = cstat{csr.offered_rps,
                   csr.rps,
                   csr.set_rps,
                   csr.skey_rps,
                   csr.lkey_rps,
                   csr.goodput,
                   csr.set_goodput,
                   csr.skey_goodput,
                   csr.lkey_goodput,
                   csr.min_percli_tput,
                   csr.max_percli_tput,
                   static_cast<double>(csr.winu_rx) / elapsed * 1000000,
                   static_cast<double>(csr.winu_tx) / elapsed * 1000000,
                   static_cast<double>(csr.resp_rx) / elapsed * 1000000,
                   static_cast<double>(csr.req_tx) / elapsed * 1000000,
                   static_cast<double>(csr.win_expired) / elapsed * 1000000,
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

        for (double i : offered_loads) {
            SteadyStateExperiment(threads, i);
        }
    }

    void ClientHandler(void *arg) {
        int pos;

        if (total_agents > 1) {
            b = new NetBarrier(total_agents - 1);
            BUG_ON(!b);
        }

        calculate_rates();

        std::string wname;
        if (wtype == 1)
            wname = std::string("set");
        else if (wtype == 2)
            wname = std::string("get");
        else if (wtype == 3)
            wname = std::string("usr");
        else if (wtype == 4)
            wname = std::string("bimod_get");
        else if (wtype == 5)
            wname = std::string("bimod_var");
        else
            wname = std::string("unknown");

        json_out.open("output.json");
        csv_out.open("output.csv", std::fstream::out | std::fstream::app);

        json_out << "[";
        PrintHeader(std::cout);

        for (double i : offered_loads) {
            SteadyStateExperiment(threads, i);
        }

        pos = json_out.tellp();
        json_out.seekp(pos-2);
        json_out << "]";
        json_out.close();
        csv_out.close();
    }

}  // anonymous namespace

int main(int argc, char *argv[]) {
    int ret;

    if (argc < 4) {
        std::cerr << "usage: [alg] [cfg_file] [cmd] ..." << std::endl;
        return -EINVAL;
    }

    std::string olcalg = argv[1];
    if (olcalg.compare("breakwater") == 0) {
        crpc_ops = &cbw_ops;
        srpc_ops = &sbw_ops;
    } else if (olcalg.compare("protego") == 0) {
        crpc_ops = &cbw_ops;
        srpc_ops = &sbw2_ops;
    } else if (olcalg.compare("pcc") == 0) {
        crpc_ops = &cpcc_ops;
        srpc_ops = &spcc_ops;
    } else if (olcalg.compare("dagor") == 0) {
        crpc_ops = &cdg_ops;
        srpc_ops = &sdg_ops;
    } else if (olcalg.compare("seda") == 0) {
        crpc_ops = &csd_ops;
        srpc_ops = &ssd_ops;
    } else if (olcalg.compare("nocontrol") == 0) {
        crpc_ops = &cnc_ops;
        srpc_ops = &snc_ops;
    } else {
        std::cerr << "Invalid overload control algorithm: " << olcalg << std::endl;
        return -EINVAL;
    }

    std::string cmd = argv[3];
    if (cmd.compare("agent") == 0) {
        if (argc < 5 || StringToAddr(argv[4], &master.ip)) {
            std::cerr << "usage: [alg] [cfg_file] agent [ip_address]"
                      << std::endl;
            return -EINVAL;
        }

        ret = runtime_init(argv[2], AgentHandler, NULL);
        if (ret) {
            printf("failed to start runtime\n");
            return ret;
        }
    } else if (cmd.compare("client") != 0) {
        std::cerr << "invalid command: " << cmd << std::endl;
        return -EINVAL;
    }

    if (argc < 16) {
        std::cerr << "usage: [alg] [cfg_file] [client|agent] [#threads] [remote_ip]"
                  << " [SET|GET|USR|BIMOD_GET] [max_key_idx] [skey_size] [skey_count]"
                  << " [lkey_size] [lkey_count] [skey_pcnt]"
                  << " [slo] [npeers] [offered_load]"
                  << std::endl;
        return -EINVAL;
    }

    threads = std::stoi(argv[4], nullptr, 0);

    ret = StringToAddr(argv[5], &raddr.ip);
    if (ret) return -EINVAL;

    std::string wtype_ = argv[6];
    if (wtype_.compare("SET") == 0)
        wtype = 1;
    else if (wtype_.compare("GET") == 0)
        wtype = 2;
    else if (wtype_.compare("USR") == 0)
        wtype = 3;
    else if (wtype_.compare("BIMOD_GET") == 0)
        wtype = 4;
    else if (wtype_.compare("BIMOD_VAR") == 0)
        wtype = 5;
    else {
        std::cerr << "invalid workload type: " << wtype_ << std::endl;
        return -EINVAL;
    }

    max_key_idx = std::stoi(argv[7], nullptr, 0);
    skey_size = std::stoi(argv[8], nullptr, 0);
    skey_count = std::stoi(argv[9], nullptr, 0);
    lkey_size = std::stoi(argv[10], nullptr, 0);
    lkey_count = std::stoi(argv[11], nullptr, 0);
    skey_pcnt = std::stoi(argv[12], nullptr, 0);
    slo = std::stoi(argv[13], nullptr, 0);
    total_agents += std::stoi(argv[14], nullptr, 0);
    offered_load = std::stod(argv[15], nullptr);

    ret = runtime_init(argv[2], ClientHandler, NULL);
    if (ret) {
        printf("failed to start runtime\n");
        return ret;
    }

    return 0;
}
