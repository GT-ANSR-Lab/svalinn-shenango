/*
 * pcc.h - PCC implementation for receiver-based admission control in the RPC layer
 */

#pragma once

#include <base/types.h>
#include <base/atomic.h>
#include <runtime/sync.h>

#include "rpc.h"

/* for RPC server */

struct spcc_ctx {
    struct srpc_ctx     cmn;
    uint64_t            ts_sent;
    uint64_t            opaque;
};

struct cpcc_session;
/* for RPC client-connection */
struct cpcc_conn {
    struct crpc_conn    cmn;
    struct cpcc_session *session;

    /* credit-related variables */
    bool            waiting_resp;
    uint32_t        credit;
    uint32_t        credit_used;

    /* per-connection stats */
    uint64_t        ecredit_rx_;
    uint64_t        cupdate_tx_;
    uint64_t        resp_rx_;
    uint64_t        req_tx_;
    uint64_t        credit_expired_;
};

/* PCC controller specific definitions. */

/* State of the controller. */
enum spcc_ctl_state {
    /* Decision making states. */
    SPCC_CTL_STATE_DECISION_MAKING_SET_RATE,
    SPCC_CTL_STATE_DECISION_MAKING_START_MONITOR,
    SPCC_CTL_STATE_DECISION_MAKING_END_MONITOR,
    SPCC_CTL_STATE_DECISION_MAKING_END,

    /* Rate adjusting states. */
    SPCC_CTL_STATE_RATE_ADJUSTING_SET_RATE,
    SPCC_CTL_STATE_RATE_ADJUSTING_START_MONITOR,
    SPCC_CTL_STATE_RATE_ADJUSTING_END_MONITOR,
    SPCC_CTL_STATE_RATE_ADJUSTING_RESET_RATE,
    SPCC_CTL_STATE_RATE_ADJUSTING_END,
};

/* Rate change direction picked by the controller. */
enum spcc_dir {
    SPCC_DIR_DECR = -1,
    SPCC_DIR_BASE = 0,
    SPCC_DIR_INCR = 1,
};


/* for RPC client */
struct cpcc_session {
    struct crpc_session cmn;

    uint64_t        id;
    uint64_t        req_id;
    int             next_conn_idx;
    bool            running;
    bool            init;
    mutex_t         lock;

    /* timer for request expire in the queue */
    waitgroup_t     timer_waiter;
    condvar_t       timer_cv;

    /* a queue of pending RPC requests */
    uint32_t            head;
    uint32_t            tail;
    struct crpc_ctx     *qreq[CRPC_QLEN];

    /* per-client stat */
    uint64_t        req_dropped_;
};
