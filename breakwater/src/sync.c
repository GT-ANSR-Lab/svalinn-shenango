/*
 * sync abstraction for overload control
 */

#include <base/stddef.h>
#include <runtime/runtime.h>
#include <runtime/sync.h>
#include <runtime/timer.h>
#include <breakwater/rpc.h>
#include <breakwater/sync.h>

#include "bw2_config.h"
#include "pcc_config.h"

inline bool mutex_lock_is_congested(mutex_t *m)
{
    if (!get_rpc_ctx()) {
        return false;
    }

    if (srpc_ops == &sbw2_ops) {
        /* If protego */
        return ((get_acc_qdel() + mutex_queue_tsc(m)) \
                > (SBW_LATENCY_BUDGET * cycles_per_us));
    } else if (srpc_ops == &spcc_ops) {
        /* If pcc */
        return ((get_acc_qdel() + mutex_queue_tsc(m)) \
                > (SPCC_QDELAY_BUDGET * cycles_per_us));
    }

    /* Other algorithms do no define queueing budget */
    return false;
}

/**
 * mutex_lock_if_uncongested - acquire a mutex
 * if the mutex is not congested
 * @m: the mutex to acquire
 *
 * Returns true if the acquire was successful
 */
inline bool mutex_lock_if_uncongested(mutex_t *m)
{
    if (mutex_lock_is_congested(m)) {
        return false;
    }

	mutex_lock(m);

	return true;
}

inline bool is_slo_violated()
{
    if (srpc_ops == &sbw2_ops) {
        /* If protego */
        return (get_acc_qdel() > (SBW_LATENCY_BUDGET * cycles_per_us));
    } else if (srpc_ops == &spcc_ops) {
        /* If pcc */
        return (get_acc_qdel() > (SPCC_QDELAY_BUDGET * cycles_per_us));
    }

    /* Other algorithms do no define queueing budget */
    return false;
}

/**
 * condvar_is_congested - check whether the condvar is congested
 * return true is condvar is congested
 * @cv: condvar to check congestion
 *
 */
inline bool condvar_is_congested(condvar_t *cv)
{
    if (!get_rpc_ctx()) {
        return false;
    }

    if (srpc_ops == &sbw2_ops) {
        /* If protego */
        return ((get_acc_qdel() + condvar_queue_tsc(cv)) \
                > (SBW_LATENCY_BUDGET * cycles_per_us));
    } else if (srpc_ops == &spcc_ops) {
        /* If pcc */
        return ((get_acc_qdel() + condvar_queue_tsc(cv)) \
                > (SPCC_QDELAY_BUDGET * cycles_per_us));
    }

    /* Other algorithms do no define queueing budget */
    return false;
}

/**
 * condvar_wait_if_uncongested - wait for condvar
 * if the condvar is not congested
 * @cv: condvar to wait
 * @m: the currently held mutex that projects the condition
 */
inline bool condvar_wait_if_uncongested(condvar_t *cv, mutex_t *m)
{
	assert_mutex_held(m);

    if (condvar_is_congested(cv)) {
        return false;
    }

	condvar_wait(cv, m);
	return true;
}
