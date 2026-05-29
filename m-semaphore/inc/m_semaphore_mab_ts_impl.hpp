#ifndef __M_SEMAPHORE_MAB_TS_IMPL_HPP__
#define __M_SEMAPHORE_MAB_TS_IMPL_HPP__

extern "C" {
#include "base/limits.h"
}

#include <cstdint>
#include <random>
#include <memory>

#include "cc/sync.h"
#include "base/list.h"
#include "mem_pmc.hpp"
#include "m_semaphore_impl.hpp"


class MemSemaphoreMabTsImpl : public MemSemaphoreImpl {
private:
    // Generic Constants.
    static constexpr uint64_t    CTL_DELAY_US                  = 500;
    static constexpr uint32_t    DEF_INIT_CAP                  = 1;
    static constexpr int         WINDOW_SZ                     = 10;

    // Reward function specific constants.
    static constexpr double      ALPHA                         = 0.8;
    static constexpr double      TARGET_NORM_MEMBW             = 1.0;
    static constexpr double      MAGNIFYER                     = 100.0;

    // Thompson Sampling specific constants.
    //
    // NOTE: These values depend on the range of the rewards.
    // Known variance of the true reward distribution.
    static constexpr double      SIGMA_SQ                      = 25.0;
    // Prior variance of the unknown mean reward.
    static constexpr double      SIGMA0_SQ                     = 625.0;
    // Prior mean of the unknown mean reward.
    static constexpr double      MU0                           = 80.0;

    // Softmax method specific constants.
    // Exploration probability.
    static constexpr double      EXPLR_PROB                    = 0.3;
    // Temperature parameter.
    static constexpr double      TAU                           = 30.0;

    // Lock to protect the shared semaphore state
    rt::Spin m_spin;

    // Current capacity of the semaphore. This dictates how many concurrent
    // threads can acquire the semaphore.
    uint32_t m_cap;

    // Current number of threads that have acquired the semaphore. This is
    // always less than or equal m_cap.
    uint32_t m_count;

    // The maximum possible capacity. This is typically equal to the maximum
    // number of CPU cores that can be allocated to the application.
    uint32_t m_max_cap;

    // The maximum memory bandwidth seen so far.
    double m_max_membw;

    // Window of past reward samples for every core count. This is implemeneted
    // as a circular buffer.
    double m_samples[NCPU+1][WINDOW_SZ];
    int    m_samples_idx[NCPU+1];

    // State required to keep track of the posterior distribution of the unknown
    // mean reward, for every core count.
    uint64_t m_counts[NCPU+1];
    double   m_sums[NCPU+1];
    double   m_means[NCPU+1];
    double   m_vars[NCPU+1];
    double   m_std_devs[NCPU+1];

    // Array of softmax probabilities used during exploration.
    double m_softmax_probs[NCPU+1];

    // State required to generate random gaussian samples.
    std::random_device               m_rd;
    std::mt19937                     m_gen;
    std::normal_distribution<double> m_std_normal;

    // State to calculate the memory bandwidth usage.
    MemPmc      m_mem_pmc;
    size_t      m_num_mem_ch;
    uint64_t    m_last_bytes;

    // Last time the controller ran.
    uint64_t m_last_time;

    // List of threads waiting to acquire the semaphore.
    struct list_head m_waiters;
    uint64_t         m_num_waiters;
    uint64_t         m_oldest_tsc;

#ifdef M_SEM_DEBUG
    // Average capacity.
    double      m_avg_cap;
    uint64_t    m_avg_count;
#endif  // M_SEM_DEBUG

    // Helper function which performs the core capacity control logic.
    void UpdateCapacity();

public:
    MemSemaphoreMabTsImpl(uint32_t init_cap = DEF_INIT_CAP);
    ~MemSemaphoreMabTsImpl();

    bool TryWait() override;
    void Wait() override;
    uint64_t QueueDelayTsc() override;
    uint64_t QueueLength() override;
    void Post() override;
    int GetCapacity() override {
        return m_cap;
    }
};

#endif  // __M_SEMAPHORE_MAB_TS_IMPL_HPP__
