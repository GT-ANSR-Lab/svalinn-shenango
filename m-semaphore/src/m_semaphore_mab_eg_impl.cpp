extern "C" {
#include <numa.h>
#include <sched.h>
#include "base/time.h"
#include "base/list.h"
#include "runtime/thread.h"
}

#include <cstdio>
#include <cmath>
#include <cassert>
#include "cc/runtime.h"
#include "m_semaphore_mab_eg_impl.hpp"


// Structure for storing some state of a waiting thread.
struct WaiterThread {
    thread_t         *th;
    uint64_t          enque_tsc;
    struct list_node  link;
};


MemSemaphoreMabEgImpl::MemSemaphoreMabEgImpl(uint32_t init_cap) {

    // Initialize the basic state
    m_cap = init_cap;
    m_count = 0;
    m_max_cap = rt::RuntimeMaxCores();
    m_max_membw = 0.0;
    for (uint32_t i = 1; i <= m_max_cap; ++i) {
        m_ewma_rewards[i] = 0.0;
    }

    // Configure the memory PMC object (required to measure membw)
    m_mem_pmc.Init();
    m_num_mem_ch = m_mem_pmc.GetActiveMemChan();
    m_last_bytes = m_mem_pmc.GetMemAccesses();

    // Set the control time
    m_last_time = microtime();

    // Initialize the waiter state
    list_head_init(&m_waiters);
    m_num_waiters = 0;
    m_oldest_tsc = UINT64_MAX;

#ifdef M_SEM_DEBUG
    m_avg_count = 0;
    m_avg_cap = 0;

    printf("[%ld] Initial Capacity=%d, Maximum Capacity=%d,"
           " Number of Memory Channels=%ld\n",
           m_last_time, init_cap, m_max_cap, m_num_mem_ch);
#endif
}


MemSemaphoreMabEgImpl::~MemSemaphoreMabEgImpl() {

    assert(!m_count);
}


void MemSemaphoreMabEgImpl::UpdateCapacity() {

    assert(m_spin.IsHeld());

    uint64_t now_time = microtime();

    // Check if we need to perform control
    if (now_time - m_last_time < CTL_DELAY_US) {
        return;
    }

    // Get the memory bandwidth usage
    uint64_t now_bytes = m_mem_pmc.GetMemAccesses();
    double membw = (double)(now_bytes - m_last_bytes) / (double)(now_time - m_last_time);

    // Update the maximum memory bandwidth
    if (m_max_membw < membw) {
        m_max_membw = membw;
    }

    // Calculate the reward for the current capacity
    double norm_membw = membw / m_max_membw;
    norm_membw = std::min(norm_membw, TARGET_NORM_MEMBW) / TARGET_NORM_MEMBW;
    double norm_cap = (double)m_cap / (double)m_max_cap;
    double reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cap;

#ifdef M_SEM_DEBUG
    printf("[%ld] Current Capacity=%d, Memory Bandwidth (MBps)=%lf,"
           " Max Memory Bandwidth (MBps)=%lf, Reward=%lf\n",
           now_time, m_cap, membw, m_max_membw, reward);
#endif

    // Update the mean for the current capacity
    m_ewma_rewards[m_cap] = (1 - REWARD_EWMA_WEIGHT) * m_ewma_rewards[m_cap] \
        + REWARD_EWMA_WEIGHT * reward;

    // Find the core count with the maximum reward
    uint32_t best_cap = 1;
    for (uint32_t i = 1; i <= m_max_cap; ++i) {
        if (m_ewma_rewards[best_cap] < m_ewma_rewards[i]) {
            best_cap = i;
        }
    }

    double explr_prob = ((double)(rand() % 1001)) / 1000.0;
    if (explr_prob < EXPLR_PROB) {
        // Explore for core counts with better rewards.
        // We limit the exploration to at most 1 core away
        // from the core count with the best reward.
        int explr_dir = (rand() % 2) ? 1 : -1;
        m_cap = (int)best_cap + explr_dir;

        // Make sure the capacity value is valid.
        m_cap = std::max(m_cap, 1u);
        m_cap = std::min(m_cap, m_max_cap);

#ifdef M_SEM_DEBUG
    printf("[%ld] Explored. New Capacity=%d\n",
           now_time, m_cap);
#endif

    } else {
        // Exploit the learnt reward information. Pick the
        // core count with the best known reward.
        m_cap = best_cap;

#ifdef M_SEM_DEBUG
    printf("[%ld] Exploited. New Capacity=%d\n",
           now_time, m_cap);
#endif
    }

#ifdef M_SEM_DEBUG
    // Update the average capacity
    ++m_avg_count;
    m_avg_cap += (m_cap - m_avg_cap) / (double)m_avg_count;
    printf("[%ld] Average Capacity=%lf\n", now_time, m_avg_cap);

    printf("[%ld] {", now_time);
    int count = 0;
    for (uint32_t i = 1; i <= m_max_cap; ++i) {
        printf("[cap=%d]:[ewma_reward=%lf],",
               i, m_ewma_rewards[i]);
    }
    printf("}\n");
#endif

    // Update any remaining state
    m_last_bytes = now_bytes;
    m_last_time = now_time;
}


bool MemSemaphoreMabEgImpl::TryWait() {

    bool acquired = false;
    m_spin.Lock();

    // Dynamically update the semaphore's capacity
    UpdateCapacity();

    // Basic semaphore operation
    if (m_count < m_cap) {
        ++m_count;
        acquired = true;
    }

    m_spin.Unlock();
    return acquired;
}


void MemSemaphoreMabEgImpl::Wait() {

    WaiterThread *waketh;

    m_spin.Lock();

    // Dynamically update the semaphore's capacity
    UpdateCapacity();

    // While there is available semaphore capacity
    // and there are threads waiting before the current
    // thread to acquire the semaphore, we will give
    // semaphore to those older threads.
    while ((m_count < m_cap) && !list_empty(&m_waiters)) {
        // Pop the thread at the head of the queue
        waketh = list_pop(&m_waiters, WaiterThread, link);
        uint64_t now = rdtsc();
        incr_acc_qdel_other(waketh->th, now - waketh->enque_tsc);
        thread_ready(waketh->th);

        // Update semaphore state
        ++m_count;
        --m_num_waiters;

        // Update the oldest threads enqueue time
        if (list_empty(&m_waiters)) {
            m_oldest_tsc = UINT64_MAX;
        } else {
            WaiterThread *oldestth = list_top(&m_waiters, WaiterThread, link);
            m_oldest_tsc = oldestth->enque_tsc;
        }
    }

    // If capacity is stil available to serve the current thread
    if (m_count < m_cap) {
        // Acquire and exit
        ++m_count;
        m_spin.Unlock();
        return;
    }

    // Put the current thread at the tail of the queue
    WaiterThread myth;
    myth.th = thread_self();
    myth.enque_tsc = rdtsc();
    bool is_first = list_empty(&m_waiters);
    list_add_tail(&m_waiters, &myth.link);
    ++m_num_waiters;
    if (is_first) {
        m_oldest_tsc = myth.enque_tsc;
    }
    m_spin.UnlockAndPark();
}


uint64_t MemSemaphoreMabEgImpl::QueueDelayTsc() {

    uint64_t cur_tsc = rdtsc();
    if (cur_tsc < m_oldest_tsc) {
        return 0;
    }

    return (cur_tsc - m_oldest_tsc);
}


uint64_t MemSemaphoreMabEgImpl::QueueLength() {

    return m_num_waiters;
}


void MemSemaphoreMabEgImpl::Post() {
    WaiterThread *waketh;

    m_spin.Lock();

    // Dynamically update the semaphore's capacity
    UpdateCapacity();

    // If there is no available capacity to wakeup
    // anyone, or there is no other thread to wakeup
    if ((m_count > m_cap) || list_empty(&m_waiters)) {
        --m_count;
    } else {
        waketh = list_pop(&m_waiters, WaiterThread, link);
        uint64_t now = rdtsc();
        incr_acc_qdel_other(waketh->th, now - waketh->enque_tsc);
        thread_ready(waketh->th);

        --m_num_waiters;

        // Update the oldest threads enqueue time
        if (list_empty(&m_waiters)) {
            m_oldest_tsc = UINT64_MAX;
        } else {
            WaiterThread *oldestth = list_top(&m_waiters, WaiterThread, link);
            m_oldest_tsc = oldestth->enque_tsc;
        }
    }

    m_spin.Unlock();
}
