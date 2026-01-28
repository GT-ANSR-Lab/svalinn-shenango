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
#include "protocol.hh"
#include "m_semaphore.hpp"

#include <getopt.h>
#include <cstdio>
#include <string>
#include <iostream>
#include <vector>
#include <unistd.h>
#include <thread>
#include <random>

#include <DataFrame/DataFrame.h>
#include <DataFrame/DataFrameFinancialVisitors.h>
#include <DataFrame/DataFrameMLVisitors.h>
#include <DataFrame/DataFrameStatsVisitors.h>
#include <DataFrame/Utils/DateTime.h>


// Forward declare the functions required to execute each operation
void DataFrameMaxOpHandler();
void DataFrameRmvOpHandler();
void DataFrameKmeansOpHandler();
void DataFramePpoOpHandler();
void DataFrameDecomOpHandler();
void DataFrameDecayOpHandler();
void DataFrameAdOpHandler();


// Program settings
struct {
    std::string config_path = "";
    std::string ovld_cntl_algo = "nocontrol";
    int port = 0;
    std::string csv_path = "";
    bool use_msem = false;
} g_settings;


// Overload controller ops
const struct crpc_ops *crpc_ops;
const struct srpc_ops *srpc_ops;


//
// Global DataFrame state
//
// Dataframe object
hmdf::StdDataFrame<hmdf::DateTime> g_df;
// Operation Handler table
void (*g_op_handler[DATAFRAME_OP_NUM_OPS])() = {
    [DATAFRAME_OP_MAX] = DataFrameMaxOpHandler,
    [DATAFRAME_OP_RMV] = DataFrameRmvOpHandler,
    [DATAFRAME_OP_KMEANS] = DataFrameKmeansOpHandler,
    [DATAFRAME_OP_PPO] = DataFramePpoOpHandler,
    [DATAFRAME_OP_DECOM] = DataFrameDecomOpHandler,
    [DATAFRAME_OP_DECAY] = DataFrameDecayOpHandler,
    [DATAFRAME_OP_AD] = DataFrameAdOpHandler,
};
// Information of whether a request is memory-bandwidth bound
bool g_is_op_membw_bound[DATAFRAME_OP_NUM_OPS] = {
    [DATAFRAME_OP_MAX] = false,
    [DATAFRAME_OP_RMV] = false,
    [DATAFRAME_OP_KMEANS] = false,
    [DATAFRAME_OP_PPO] = false,
    [DATAFRAME_OP_DECOM] = true,
    [DATAFRAME_OP_DECAY] = true,
    [DATAFRAME_OP_AD] = true,
};
// Memory semaphore handle
static MemSemaphore *g_msem = nullptr;


// Help message
void Help() {
    std::cerr << "Usage: dataframe_server --config_path <Path to Caladan config file> " << std::endl
              << "       --ovld_cntl_algo <Overload controller algorithm> " << std::endl
              << "       --port <Server's TCP port number> " << std::endl
              << "       --csv_path <Path to the typed CSV dataset> " << std::endl
              << "       --use_msem" << std::endl;
}


// Parse the command line arguments
void ParseArguments(int argc, char *argv[]) {

    // Command line arguments
    static struct option long_opts[] = {
        {"config_path", required_argument, nullptr, 0},
        {"ovld_cntl_algo", required_argument, nullptr, 0},
        {"port", required_argument, nullptr, 0},
        {"csv_path", required_argument, nullptr, 0},
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
        } else if (opt_name == "csv_path") {
            g_settings.csv_path = opt_arg;
        } else if (opt_name == "use_msem") {
            g_settings.use_msem = true;
        }
    }

    // Validate the arguments
    if ((g_settings.config_path == "") ||
        (g_settings.ovld_cntl_algo == "") ||
        (g_settings.port == 0) ||
        (g_settings.csv_path == "")) {
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


// Open and preprocess the CSV dataset
void DataFrameInit() {

    // Open the dataset
    std::ifstream file(g_settings.csv_path);
    if (!file) {
        std::cerr << "Unable to open file " << g_settings.csv_path << std::endl;
        return;
    }
    g_df.read(file, hmdf::io_format::csv2);

    // Fill any missing entries
    g_df.fill_missing<double, 4>({ "Close", "Open", "High", "Low" },
                                 hmdf::fill_policy::fill_forward);

    // Calculate the returns and load them as a column
    hmdf::ReturnVisitor<double> return_v{ hmdf::return_policy::log };
    const auto &return_result = \
        g_df.single_act_visit<double>("Close", return_v).get_result();
    g_df.load_column("Return", std::move(return_result));
    g_df.get_column<double>("Return")[0] = 0;
}


// Process the requests from a single client requesting statistics
void DataFrameStatWorker(std::unique_ptr<rt::TcpConn> c) {

    while (true) {

        // Receive an uptime request.
        DataFrameStatReq req;
        ssize_t ret = c->ReadFull(&req, sizeof(req));
        if (ret != static_cast<ssize_t>(sizeof(req))) {
            if (ret == 0 || ret == -ECONNRESET) break;
            log_err("read failed, ret = %ld", ret);
            break;
        }

        // Check for the right magic value.
        if (ntoh64(req.magic) != DATAFRAME_STAT_REQ_MAGIC) {
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
        DataFrameStatResp resp = {total,
                                  busy,
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
void DataFrameStatServer() {

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
        rt::Thread([=] { DataFrameStatWorker(std::unique_ptr<rt::TcpConn>(c)); })
            .Detach();
    }
}


// Maximum operation handler
void DataFrameMaxOpHandler() {
    hmdf::MaxVisitor<double, hmdf::DateTime> max_v;
    g_df.single_act_visit<double>("Close", max_v);
}


// Rolling mid value operation handler
void DataFrameRmvOpHandler() {
    hmdf::RollingMidValueVisitor<double, hmdf::DateTime> rmv_v(15);
    g_df.single_act_visit<double, double>("Low", "High", rmv_v);
}


// K-means operation handler
void DataFrameKmeansOpHandler() {
    hmdf::KMeansVisitor<4, double, hmdf::DateTime> kmeans_v{ 10 };
    g_df.single_act_visit<double>("Return", kmeans_v);
}


// Percentage Price Oscillator operation handler
void DataFramePpoOpHandler() {
    hmdf::PercentPriceOSCIVisitor<double, hmdf::DateTime> ppo_v(5, 9, 4);
    g_df.single_act_visit<double>("Close", ppo_v);
}


// Decomposition operation handler
void DataFrameDecomOpHandler() {
    hmdf::DecomposeVisitor<double, hmdf::DateTime> decom_v{ 170, 0.01, 1 };
    g_df.single_act_visit<double>("Return", decom_v);
}


// Decay operation handler
void DataFrameDecayOpHandler() {
    hmdf::DecayVisitor<double, hmdf::DateTime> decay_v(5, true);
    g_df.single_act_visit<double>("Close", decay_v);
}


// Accumuluation-Distribution operation handler
void DataFrameAdOpHandler() {
    hmdf::AccumDistVisitor<double, hmdf::DateTime> ad_v;
    g_df.single_act_visit<double, double, double, double, double>(
        "Low", "High", "Open", "Close", "Volume", ad_v);
}


// The main per-request handler
void DataFrameRequestHandler(struct srpc_ctx *ctx) {

    DataFrameReq *req = reinterpret_cast<DataFrameReq *>(ctx->req_buf);

    // Validate the request
    if (unlikely(ctx->req_len != sizeof(DataFrameReq))) {
        log_err("got invalid RPC len %ld", ctx->req_len);
        return;
    }
    if (unlikely(ntoh64(req->magic) != DATAFRAME_REQ_MAGIC)) {
        log_err("got corrupted RPC");
        return;
    }

    // Perform the operation
    uint64_t st_us = microtime();
    req->op = ntoh32(req->op);
    assert(req->op < DATAFRAME_OP_NUM_OPS);

    if (!g_is_op_membw_bound[req->op]) {
        // CPU-bound request, we simply run
        g_op_handler[req->op]();
    } else {
        // Memory bandwidth bound request, we use memory semaphore
        if (!g_settings.use_msem) {
            g_op_handler[req->op]();
        } else {
            if (g_msem->TryWait()) {
                g_op_handler[req->op]();
                g_msem->Post();
            } else {
                ctx->drop = true;
                return;
            }
        }
    }
    st_us = microtime() - st_us;

    // Prepare the response message
    DataFrameResp *resp = reinterpret_cast<DataFrameResp *>(ctx->resp_buf);
    resp->magic = hton64(DATAFRAME_RESP_MAGIC);
    resp->opaque = req->opaque;
    resp->st_us = hton64(st_us);
    ctx->resp_len = sizeof(DataFrameResp);
}


// Entry point in Caladan's runtime
void DataFrameServerMain(void *arg) {

    int ret;

    // Start the stats server
    rt::Thread([] { DataFrameStatServer(); }).Detach();

    // Initialize the dataset
    DataFrameInit();

    // Create the memory semaphore object
    g_msem = MemSemaphore::GetInstance();

    // Start the RPC server
    ret = rpc::RpcServerEnable(DataFrameRequestHandler);
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
    ret = runtime_init(g_settings.config_path.c_str(),
                       DataFrameServerMain, nullptr);
    if (ret) {
        std::cerr << "Failed to start Caladan runtime\n";
        return 1;
    }

    return 0;
}
