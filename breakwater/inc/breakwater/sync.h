/*
 * sync.h - sync abstraction for overload control
 */

#pragma once

#include <base/lock.h>
#include <base/atomic.h>
#include <runtime/thread.h>
#include <runtime/sync.h>

/* A simple congestion-aware mutex with Protego's AQM policy */
bool mutex_lock_if_uncongested(mutex_t *m);
bool mutex_lock_is_congested(mutex_t *m);
bool is_slo_violated();

/* A simple congestion-aware condition variable with Protego's AQM policy */
bool condvar_wait_if_uncongested(condvar_t *cv, mutex_t *m);
bool condvar_is_congested(condvar_t *cv);

/* A congestion-aware mutex with an improved AQM policy that considers the
 * length of the queue along with the latency. Protego's AQM policy only
 * considers the instantaneous queueing delay, and can suffer if a burst
 * of requests are queued at the same time. To tackle this we need to factor
 * in the length of the queue as well.
 *
 */

typedef enum cong_aware_mutex_policy {

	CONG_AWARE_MUTEX_POLICY_QDELAY = 0,
	CONG_AWARE_MUTEX_POLICY_QLEN,

} cong_aware_mutex_policy_t;

typedef struct cong_aware_mutex {

	cong_aware_mutex_policy_t pol;

	/* The underlying vanilla mutex */
	mutex_t m;

	/* Estimate of the critical section run time */
	atomic_t hold_time;

	/* Last time the mutex was acquired and released */
	atomic_t last_acq;
	atomic_t last_rel;

	/* Number of threads waiting */
	atomic_t num_th;

} cong_aware_mutex_t;


void cong_aware_mutex_init(cong_aware_mutex_t *cam, cong_aware_mutex_policy_t pol);
void cong_aware_mutex_lock(cong_aware_mutex_t *cam);
bool cong_aware_mutex_try_lock(cong_aware_mutex_t *cam);
bool cong_aware_mutex_is_congested(cong_aware_mutex_t *cam);
bool cong_aware_mutex_lock_if_uncongested(cong_aware_mutex_t *cam);
void cong_aware_mutex_unlock(cong_aware_mutex_t *cam);
