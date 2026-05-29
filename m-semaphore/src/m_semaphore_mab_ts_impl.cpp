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
#include "m_semaphore_mab_ts_impl.hpp"


// Structure for storing some state of a waiting thread.
struct WaiterThread {
    thread_t         *th;
    uint64_t          enque_tsc;
    struct list_node  link;
};


MemSemaphoreMabTsImpl::MemSemaphoreMabTsImpl(uint32_t init_cap) {

    // Initialize the basic state
    m_cap = init_cap;
    m_count = 0;
    m_max_cap = rt::RuntimeMaxCores();
    m_max_membw = 0.0;
    for (uint32_t i = 1; i <= m_max_cap; ++i) {
        // Initialize the sample window
        for (uint32_t j = 0; j < WINDOW_SZ; ++j) {
            m_samples[i][j] = 0.0;
        }
        m_samples_idx[i] = 0;

        // Initialize the posterior distribution state
        m_counts[i] = 0;
        m_sums[i] = 0.0;
        m_means[i] = MU0;
        m_vars[i] = SIGMA0_SQ;
        m_std_devs[i] = std::sqrt(m_vars[i]);
    }

    // Initialize the random number generation state
    m_gen = std::mt19937(m_rd());
    m_std_normal = std::normal_distribution<double>(0.0, 1.0);

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


MemSemaphoreMabTsImpl::~MemSemaphoreMabTsImpl() {

    assert(!m_count);
}


void MemSemaphoreMabTsImpl::UpdateCapacity() {

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
    reward = MAGNIFYER * reward;

#ifdef M_SEM_DEBUG
    printf("[%ld] Current Capacity=%d, Memory Bandwidth (MBps)=%lf,"
           " Max Memory Bandwidth (MBps)=%lf, Reward=%lf\n",
           now_time, m_cap, membw, m_max_membw, reward);
#endif

    // Update the sample array
    double old_reward = m_samples[m_cap][m_samples_idx[m_cap]];
    m_samples[m_cap][m_samples_idx[m_cap]] = reward;
    m_samples_idx[m_cap] = (m_samples_idx[m_cap] + 1) % WINDOW_SZ;

    // Update the count and sum of the samples for this arm
    ++m_counts[m_cap];
    if (m_counts[m_cap] > WINDOW_SZ) {
        m_counts[m_cap] = WINDOW_SZ;
    }
    m_sums[m_cap] = m_sums[m_cap] + reward - old_reward;

    // Update the posterior distribution of the unknown mean reward for this arm
    //
    // NOTE: The update rule can be found in the table here -
    //       https://en.wikipedia.org/wiki/Conjugate_prior
    //       We assume a known variance and unknown mean model, with Normal
    //       prior, likelihood, and posterior.
    m_vars[m_cap] = 1.0 / ((1.0/SIGMA0_SQ) + ((double)m_counts[m_cap]/SIGMA_SQ));
    m_means[m_cap] = m_vars[m_cap] * (MU0/SIGMA0_SQ + m_sums[m_cap]/SIGMA_SQ);
    m_std_devs[m_cap] = std::sqrt(m_vars[m_cap]);

    //
    // Sample the next arm
    //
    double explr_prob = ((double)(rand() % 1001)) / 1000.0;
    if (explr_prob < EXPLR_PROB) {
        // We are in the explore phase. In this phase, we randomly explore
        // a core count below the currently used core count. The idea here
        // is to discover a change in the current regime (i.e., a load shift).
        // If a regime change occurred, and the new optimal capacity required
        // to saturate the memory bandwidth reduces than the current optimal,
        // then the reward function of the current optimal will not reduce,
        // for vanilla Thompson Sampling to converge to the new value. We
        // need to probe lower core counts every now and then to detect this.
        // We sample an arm from a softmax distribution obtained using
        // the posterior mean reward values for the arms having core counts
        // lower than the currently used core count.

        // Find the maximum mean reward. Used to subtract from the
        // individual samples before exponentiation, for numerical stability.
        double max_mean = -100000000.0;
        for (uint32_t i = 1; i <= m_cap; ++i) {
            if (max_mean < m_means[i]) {
                max_mean = m_means[i];
            }
        }

        // Calculate the softmax probabilities
        double sum = 0.0;
        for (uint32_t i = 1; i <= m_cap; ++i) {
            m_softmax_probs[i] = std::exp((m_means[i] - max_mean) / TAU);
            sum += m_softmax_probs[i];
        }
        for (uint32_t i = 1; i <= m_cap; ++i) {
            m_softmax_probs[i] = m_softmax_probs[i] / sum;
        }

        // Sample an arm from the probability distribution
        double arm_prob = ((double)(rand() % 1001)) / 1000.0;
        for (uint32_t i = 1; i <= m_cap; ++i) {
            if (arm_prob < m_softmax_probs[i]) {
                m_cap = i;
                break;
            }
            arm_prob = arm_prob - m_softmax_probs[i];
        }

    } else {
        // We are in the vanilla Thompson sampling phase. In this phase, we
        // sample a mean reward from the posterior distribution of the unknown
        // mean reward we maintain for each core count. We then pick the next
        // arm to be the one that yielded the maximum random sample. Thompson
        // Sampling converges pretty quickly, which is good for controller
        // stability. Hence, even though there will be some exploration in
        // this state initially, after a few draws, this state will be
        // as good as always drawing the most optimal option.
        double max_mean = -100000000.0;
        for (uint32_t i = 1; i <= m_max_cap; ++i) {
            double z = m_std_normal(m_gen);

            double mean = m_means[i] + m_std_devs[i] * z;
            if (max_mean < mean) {
                max_mean = mean;
                m_cap = i;
            }
        }
    }

#ifdef M_SEM_DEBUG
    // Update the average capacity
    ++m_avg_count;
    m_avg_cap += (m_cap - m_avg_cap) / (double)m_avg_count;

    printf("[%ld] Updated Capacity=%d\n", now_time, m_cap);
    printf("[%ld] Average Capacity=%lf\n", now_time, m_avg_cap);
    printf("[%ld] {", now_time);
    for (uint32_t i = 1; i <= m_max_cap; ++i) {
        printf("[cap=%d]:[mean=%lf,stddev=%lf,cnt=%ld],",
               i, m_means[i], m_std_devs[i], m_counts[i]);
    }
    printf("}\n");
#endif

    // Update any remaining state
    m_last_bytes = now_bytes;
    m_last_time = now_time;
}


bool MemSemaphoreMabTsImpl::TryWait() {

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


void MemSemaphoreMabTsImpl::Wait() {

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


uint64_t MemSemaphoreMabTsImpl::QueueDelayTsc() {

    uint64_t cur_tsc = rdtsc();
    if (cur_tsc < m_oldest_tsc) {
        return 0;
    }

    return (cur_tsc - m_oldest_tsc);
}


uint64_t MemSemaphoreMabTsImpl::QueueLength() {

    return m_num_waiters;
}


void MemSemaphoreMabTsImpl::Post() {
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
