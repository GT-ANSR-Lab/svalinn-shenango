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


void cong_aware_mutex_init(cong_aware_mutex_t *cam, cong_aware_mutex_policy_t pol) {

	cam->pol = pol;
	mutex_init(&cam->m);
	atomic_write(&cam->hold_time, 0);
	atomic_write(&cam->last_acq, 0);
	atomic_write(&cam->last_rel, 0);
	atomic_write(&cam->num_th, 0);
}

void cong_aware_mutex_lock(cong_aware_mutex_t *cam) {

	atomic_inc(&cam->num_th);

	/* Acquire the mutex directly */
	mutex_lock(&cam->m);

	/* Update the state */
	atomic_dec(&cam->num_th);
	atomic_write(&cam->last_acq, microtime());
}

bool cong_aware_mutex_try_lock(cong_aware_mutex_t *cam) {

	if (mutex_try_lock(&cam->m)) {
		atomic_write(&cam->last_acq, microtime());
	}

	return false;
}

bool cong_aware_mutex_is_congested(cong_aware_mutex_t *cam) {

	uint64_t latency_budget;
	uint64_t est_wait_time;

    if (!get_rpc_ctx()) {
        return false;
    }

    if (srpc_ops == &sbw2_ops) {
        /* If protego */
		latency_budget = SBW_LATENCY_BUDGET;
    } else if (srpc_ops == &spcc_ops) {
        /* If pcc */
		latency_budget = SPCC_QDELAY_BUDGET;
    } else {
		/* Other overload controllers do not support these mutexes */
		return false;
	}

	/* Get an estimate of the wait time */
	if (cam->pol == CONG_AWARE_MUTEX_POLICY_NONE) {
		/* No policy, behave similar to a vanilla mutex */
		return false;
	} else if (cam->pol == CONG_AWARE_MUTEX_POLICY_QDELAY) {
		est_wait_time = mutex_queue_us(&cam->m);
	} else if (cam->pol == CONG_AWARE_MUTEX_POLICY_QLEN) {
		est_wait_time = atomic_read(&cam->hold_time) * atomic_read(&cam->num_th);
	} else {
		/* Not possible */
	}

	/* Check if the request has enough budget */
	return ((get_acc_qdel_us() + est_wait_time) > latency_budget);
}

bool cong_aware_mutex_lock_if_uncongested(cong_aware_mutex_t *cam) {

    if (cong_aware_mutex_is_congested(cam)) {
        return false;
    }

	cong_aware_mutex_lock(cam);
	return true;
}

void cong_aware_mutex_unlock(cong_aware_mutex_t *cam) {

	uint64_t last_hold_time;
	uint64_t hold_time;

	/* Update the state */
	atomic_write(&cam->last_rel, microtime());
	last_hold_time = atomic_read(&cam->last_rel) - atomic_read(&cam->last_acq);
	hold_time = atomic_read(&cam->hold_time) * 0.2 + last_hold_time * 0.8;
	atomic_write(&cam->hold_time, hold_time);

	/* Release the mutex */
	mutex_unlock(&cam->m);
}


