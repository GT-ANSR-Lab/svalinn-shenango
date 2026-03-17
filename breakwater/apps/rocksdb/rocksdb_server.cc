extern "C" {
#include <base/log.h>
#include <base/time.h>
#include <net/ip.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
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
#include "protocol.h"
#include "m_semaphore.hpp"

#include <getopt.h>
#include <cstdio>
#include <string>
#include <iostream>
#include <vector>
#include <unistd.h>
#include <thread>
#include <random>
#include <rocksdb/db.h>


// Program settings
struct {
    std::string config_path = "";
    std::string ovld_cntl_algo = "nocontrol";
    int port = 0;
    std::string db_path = "";
    int skey_size = 0;
    int skey_count = 0;
    int lkey_size = 0;
    int lkey_count = 0;
    bool use_msem = false;
} g_settings;


// Overload controller ops
const struct crpc_ops *crpc_ops;
const struct srpc_ops *srpc_ops;


// RocksDB instance handler
rocksdb::DB *g_db;
// Per-core buffers to read the responses into
std::vector<std::string> g_value_buf;
// Memory semaphore handle
static MemSemaphore *g_msem = nullptr;


// Help message
void Help() {
    std::cerr << "Usage: rocksdb_server --config_path <Path to Caladan config file> " << std::endl
              << "       --ovld_cntl_algo <Overload controller algorithm> " << std::endl
              << "       --port <Server's TCP port number> " << std::endl
              << "       --db_path <Path to tmpfs rocksdb instance> " << std::endl
              << "       --skey_size <Size of short keys> " << std::endl
              << "       --skey_count <Number of short keys> " << std::endl
              << "       --lkey_size <Size of large keys> " << std::endl
              << "       --lkey_count <Number of large keys> " << std::endl
              << "       --use_msem" << std::endl;
}


// Parse the command line arguments
void ParseArguments(int argc, char *argv[]) {

    // Command line arguments
    static struct option long_opts[] = {
        {"config_path", required_argument, nullptr, 0},
        {"ovld_cntl_algo", required_argument, nullptr, 0},
        {"port", required_argument, nullptr, 0},
        {"db_path", required_argument, nullptr, 0},
        {"skey_size", required_argument, nullptr, 0},
        {"skey_count", required_argument, nullptr, 0},
        {"lkey_size", required_argument, nullptr, 0},
        {"lkey_count", required_argument, nullptr, 0},
        {"use_msem", no_argument, nullptr, 0},
        {nullptr, 0, nullptr, 0} // End of options
    };

    int opt = 0;
    int opt_idx = 0;

    // Extract the arguments
    while ((opt = getopt_long(argc, argv, "", long_opts,
                              &opt_idx)) != -1) {
        if (opt != 0) {
            std::cerr << "Unknown option\n";
            Help();
            exit(1);
        }

        std::string opt_name = long_opts[opt_idx].name;
        std::string opt_arg;
        if (optarg) {
            opt_arg = optarg;
        }

        if (opt_name == "config_path") {
            g_settings.config_path = opt_arg;
        } else if (opt_name == "ovld_cntl_algo") {
            g_settings.ovld_cntl_algo = opt_arg;
        } else if (opt_name == "port") {
            g_settings.port = std::stoi(opt_arg);
        } else if (opt_name == "db_path") {
            g_settings.db_path = opt_arg;
        } else if (opt_name == "skey_size") {
            g_settings.skey_size = std::stoi(opt_arg);
        } else if (opt_name == "skey_count") {
            g_settings.skey_count = std::stoi(opt_arg);
        } else if (opt_name == "lkey_size") {
            g_settings.lkey_size = std::stoi(opt_arg);
        } else if (opt_name == "lkey_count") {
            g_settings.lkey_count = std::stoi(opt_arg);
        } else if (opt_name == "use_msem") {
            g_settings.use_msem = true;
        }
    }

    // Validate the arguments
    if ((g_settings.config_path == "") ||
        (g_settings.ovld_cntl_algo == "") ||
        (g_settings.port == 0) ||
        (g_settings.db_path == "") ||
        (g_settings.skey_size == 0) ||
        (g_settings.skey_count == 0) ||
        (g_settings.lkey_size == 0) ||
        (g_settings.lkey_count == 0)) {
        Help();
        exit(1);
    }
}


// Set the required overload controller ops
void OverloadControlInit() {

    std::string& oc = g_settings.ovld_cntl_algo;

    if (oc == "breakwater") {
        crpc_ops = &cbw_ops;
        srpc_ops = &sbw_ops;
    } else if (oc == "protego") {
        crpc_ops = &cbw_ops;
        srpc_ops = &sbw2_ops;
    } else if (oc == "pcc") {
        crpc_ops = &cpcc_ops;
        srpc_ops = &spcc_ops;
    } else if (oc == "dagor") {
        crpc_ops = &cdg_ops;
        srpc_ops = &sdg_ops;
    } else if (oc == "seda") {
        crpc_ops = &csd_ops;
        srpc_ops = &ssd_ops;
    } else if (oc == "nocontrol") {
        crpc_ops = &cnc_ops;
        srpc_ops = &snc_ops;
    } else {
        std::cerr << "Invalid overload control algorithm\n";
        exit(1);
    }
}


// Open the RocksDB instance
void OpenRocksDB() {

    rocksdb::Options options;

    // Set the options for optimizing the in-memory key-value store
    options.create_if_missing = false;
    options.compression = rocksdb::kNoCompression;
    options.disable_auto_compactions = true;
    options.allow_mmap_reads = true;
    options.allow_mmap_writes = true;
    options.IncreaseParallelism(0);

    // Open the database
    rocksdb::DB* db;
    rocksdb::Status status = rocksdb::DB::Open(options,
                                               g_settings.db_path, &db);
    assert(status.ok());

    // Set the global handler
    g_db = db;
}


// Process the requests from a single client requesting statistics
void RocksDBStatWorker(std::unique_ptr<rt::TcpConn> c) {

    while (true) {

        // Receive an uptime request.
        RocksDBStatReq req;
        ssize_t ret = c->ReadFull(&req, sizeof(req));
        if (ret != static_cast<ssize_t>(sizeof(req))) {
            if (ret == 0 || ret == -ECONNRESET) break;
            log_err("read failed, ret = %ld", ret);
            break;
        }

        // Check for the right magic value.
        if (ntoh64(req.magic) != ROCKSDB_STAT_REQ_MAGIC) {
            break;
        }

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

        // Prepare the response stats message
        RocksDBStatResp resp = {total,
                                busy,
				rt::RuntimeGlobMemAccesses(),
                                rt::RuntimeMaxCores(),
                                static_cast<unsigned int>(sysconf(_SC_NPROCESSORS_ONLN)),
                                rpc::RpcServerStatCupdateRx(),
                                rpc::RpcServerStatEcreditTx(),
                                rpc::RpcServerStatCreditTx(),
                                rpc::RpcServerStatReqRx(),
                                rpc::RpcServerStatReqDropped(),
                                rpc::RpcServerStatRespTx()};

        // Send an uptime response.
        ret = c->WriteFull(&resp, sizeof(resp));
        if (ret != sizeof(resp)) {
            if (ret == -EPIPE || ret == -ECONNRESET) break;
            log_err("write failed, ret = %ld", ret);
            break;
        }
    }
}


// Server for accepting client requests for statistics
void RocksDBStatServer() {

    // Open the stats server port
    std::unique_ptr<rt::TcpQueue> q(
        rt::TcpQueue::Listen({0, (uint16_t)(g_settings.port + 1)}, 4096));
    if (q == nullptr) {
        panic("couldn't listen for connections");
    }

    // Accept stats requests
    while (true) {
        rt::TcpConn *c = q->Accept();
        if (c == nullptr) {
            panic("couldn't accept a connection");
        }
        rt::Thread([=] { RocksDBStatWorker(std::unique_ptr<rt::TcpConn>(c)); })
            .Detach();
    }
}


// The main per-request handler
void RocksDBRequestHandler(struct srpc_ctx *ctx) {

    int core_id = get_current_affinity();
    const RocksDBReq *req = reinterpret_cast<const RocksDBReq *>(ctx->req_buf);

    // Validate the request
    if (unlikely(ctx->req_len != sizeof(RocksDBReq))) {
        log_err("got invalid RPC len %ld", ctx->req_len);
        return;
    }
    if (unlikely(ntoh64(req->magic) != ROCKSDB_REQ_MAGIC)) {
        log_err("got corrupted RPC");
        return;
    }

    // Perform the key read
    uint64_t st_us = microtime();
    rocksdb::ReadOptions read_opts;
    rocksdb::Slice key(req->key, ntoh32(req->key_len));
    rocksdb::Status status;
    if (req->is_skey) {
        // For short keys simply run the request
        status = g_db->Get(read_opts, key, &g_value_buf[core_id]);
    } else {
        // For long keys control using memory semaphore
        if (!g_settings.use_msem) {
            status = g_db->Get(read_opts, key, &g_value_buf[core_id]);
        } else {
            if (g_msem->TryWait()) {
                status = g_db->Get(read_opts, key, &g_value_buf[core_id]);
                g_msem->Post();
            } else {
                ctx->drop = true;
                return;
            }
        }
    }

    assert(status.ok());
    st_us = microtime() - st_us;

    // Prepare the response message
    RocksDBResp *resp = reinterpret_cast<RocksDBResp *>(ctx->resp_buf);
    resp->magic = hton64(ROCKSDB_RESP_MAGIC);
    resp->opaque = req->opaque;
    resp->st_us = hton64(st_us);
    ctx->resp_len = sizeof(RocksDBResp);
}


#ifdef PROFILE_ST

#define NUM_ITRS (10000)

void RocksDBProfileServiceTimes() {

    std::vector<uint64_t> times(NUM_ITRS);
    uint64_t start;
    uint64_t end;
    std::string skey;
    std::string lkey;
    int key;

    for (int i = 0; i < NUM_ITRS; ++i) {

        // Generate a random skey
        key = rand() % g_settings.skey_count;
        skey = "skey-" + std::to_string(key);

        // Perform the get
        start = microtime();
        rocksdb::ReadOptions read_opts;
        rocksdb::Status status;
        status = g_db->Get(read_opts, skey, &g_value_buf[0]);
        assert(status.ok());
        end = microtime();
        times[i] = end - start;
    }

    // Print the times
    std::sort(times.begin(), times.end());
    printf("Short Keys Service Times:\n");
    printf("\tmin:%ld us\n", times[0]);
    printf("\tp50:%ld us\n", times[NUM_ITRS * 0.50]);
    printf("\tp90:%ld us\n", times[NUM_ITRS * 0.90]);
    printf("\tp99:%ld us\n", times[NUM_ITRS * 0.99]);
    printf("\tmax:%ld us\n", times[NUM_ITRS - 1]);


    for (int i = 0; i < NUM_ITRS; ++i) {

        // Generate a random lkey
        key = rand() % g_settings.lkey_count;
        lkey = "lkey-" + std::to_string(key);

        // Perform the get
        start = microtime();
        rocksdb::ReadOptions read_opts;
        rocksdb::Status status;
        status = g_db->Get(read_opts, lkey, &g_value_buf[0]);
        assert(status.ok());
        end = microtime();
        times[i] = end - start;
    }

    // Print the times
    std::sort(times.begin(), times.end());
    printf("Long Keys Service Times:\n");
    printf("\tmin:%ld us\n", times[0]);
    printf("\tp50:%ld us\n", times[NUM_ITRS * 0.50]);
    printf("\tp90:%ld us\n", times[NUM_ITRS * 0.90]);
    printf("\tp99:%ld us\n", times[NUM_ITRS * 0.99]);
    printf("\tmax:%ld us\n", times[NUM_ITRS - 1]);
}
#endif


// Entry point in Caladan's runtime
void RocksDBServerMain(void *arg) {

    int ret;
    int num_cores = rt::RuntimeMaxCores();

    // Allocate the per-core structures
    g_value_buf.resize(num_cores);
    for (int i = 0; i < num_cores; ++i) {
        g_value_buf[i].resize(g_settings.lkey_size + 1);
    }

    // Start the stats server
    rt::Thread([] { RocksDBStatServer(); }).Detach();

    // Open the database
    OpenRocksDB();

#ifdef PROFILE_ST
    // Profile the request execution times
    RocksDBProfileServiceTimes();
#endif

    // Create the memory semaphore object
    if (g_settings.use_msem) {
        g_msem = MemSemaphore::GetInstance();
    }

    // Start the RPC server
    ret = rpc::RpcServerEnable(RocksDBRequestHandler);
    if (ret) {
        panic("couldn't enable RPC server");
    }

    // Wait forever
    rt::WaitGroup(1).Wait();
}

int main(int argc, char *argv[]) {

    int ret;

    // Get command line args
    ParseArguments(argc, argv);

    // Initialize the requested overload controller
    OverloadControlInit();

    // Initialize and start the Caladan runtime
    ret = runtime_init(g_settings.config_path.c_str(), RocksDBServerMain, nullptr);
    if (ret) {
        std::cerr << "Failed to start Caladan runtime\n";
        return 1;
    }

    return 0;
}
