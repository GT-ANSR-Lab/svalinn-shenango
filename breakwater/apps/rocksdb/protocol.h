#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <cstdint>

#define ROCKSDB_STAT_REQ_MAGIC (0xDEADBEEF)

/* Statistics request message */
struct RocksDBStatReq {
    uint64_t magic;
};

/* Statistics response message */
struct RocksDBStatResp {
    uint64_t total;
    uint64_t busy;
    uint64_t mem_accesses;
    double energy_consumed;
    unsigned int num_cores;
    unsigned int max_cores;
    uint64_t winu_rx;
    uint64_t winu_tx;
    uint64_t win_tx;
    uint64_t req_rx;
    uint64_t req_dropped;
    uint64_t resp_tx;
};

#define ROCKSDB_REQ_MAGIC (0xFEED)
#define ROCKSDB_RESP_MAGIC (0xF00D)
#define MAX_KEY_SIZE (32u)

/* RocksDB request message */
struct RocksDBReq {
    uint64_t magic;
    uint64_t opaque;
    bool is_skey;
    int key_len;
    char key[MAX_KEY_SIZE + 1];
};

/* RocksDB response message */
struct RocksDBResp {
    uint64_t magic;
    uint64_t opaque;

    /* Per-request stats */
    uint64_t st_us;
};


#endif  /* __PROTOCOL_H__ */
