/*
 * pcc_proto.h - RPC protocol definitions for PCC
 */

#pragma once

#include <base/types.h>

#define PCC_REQ_MAGIC   0x63727063 /* 'crpc' */
#define PCC_RESP_MAGIC  0x73727063 /* 'srpc' */

enum {
    PCC_OP_CALL = 0,   /* performs a procedure call */
    PCC_OP_CREDIT,     /* just updates the credit (no call) */
    PCC_OP_MAX,        /* maximum number of opcodes */
};

#define PCC_CFLAG_DSYNC 0x01

#define PCC_SFLAG_DROP  0x01

/* header used for CLIENT -> SERVER */
struct cpcc_hdr {
    uint32_t    magic;      /* must be set to RPC_REQ_MAGIC */
    uint32_t    op;         /* the opcode */
    size_t      len;        /* length of request in bytes */
    uint64_t    id;         /* Request / Response ID */
    uint64_t    demand;     /* the demanded window size */
    uint64_t    ts_sent;
    uint8_t     flags;
};

/* header used for SERVER -> CLIENT */
struct spcc_hdr {
    uint32_t    magic;      /* must be set to RPC_RESP_MAGIC */
    uint32_t    op;         /* the opcode */
    size_t      len;        /* length of response in bytes */
    uint64_t    id;         /* Request / Response ID */
    uint64_t    credit;     /* the offered window size */
    uint64_t    ts_sent;
    uint8_t     flags;
};
