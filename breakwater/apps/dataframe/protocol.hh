#ifndef __PROTOCOL_HH__
#define __PROTOCOL_HH__

#include <cstdint>

#define DATAFRAME_STAT_REQ_MAGIC (0xDEADBEEF)

/* Statistics request message */
struct DataFrameStatReq {
    uint64_t magic;
};

/* Statistics response message */
struct DataFrameStatResp {
    uint64_t total;
    uint64_t busy;
    unsigned int num_cores;
    unsigned int max_cores;
    uint64_t winu_rx;
    uint64_t winu_tx;
    uint64_t win_tx;
    uint64_t req_rx;
    uint64_t req_dropped;
    uint64_t resp_tx;
};

#define DATAFRAME_REQ_MAGIC (0xFEED)
#define DATAFRAME_RESP_MAGIC (0xF00D)
#define MAX_KEY_SIZE (32u)

/* Types of operations supported by the DataFrame server */
enum DataFrameOp {
    DATAFRAME_OP_MAX = 0,
    DATAFRAME_OP_RMV,
    DATAFRAME_OP_KMEANS,
    DATAFRAME_OP_PPO,
    DATAFRAME_OP_DECOM,
    DATAFRAME_OP_DECAY,
    DATAFRAME_OP_AD,
    DATAFRAME_OP_NUM_OPS
};

/* DataFrame request message */
struct DataFrameReq {
    uint64_t magic;
    uint64_t opaque;
    uint32_t op;
};

/* DataFrame response message */
struct DataFrameResp {
    uint64_t magic;
    uint64_t opaque;

    /* Per-request stats */
    uint64_t st_us;
};


#endif  /* __PROTOCOL_HH__ */
