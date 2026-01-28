#ifndef __M_SEMAPHORE_OPT_IMPL_HPP__
#define __M_SEMAPHORE_OPT_IMPL_HPP__

extern "C" {
#include "base/limits.h"
}

#include <cstdint>
#include <random>
#include <memory>
#include <array>

#include "cc/sync.h"
#include "m_semaphore_impl.hpp"


class MemSemaphoreOptImpl : public MemSemaphoreImpl {
private:
    // Generic Constants.
    static constexpr uint64_t    CTL_DELAY_US                  = 500;
    static constexpr uint32_t    DEF_INIT_CAP                  = 1;
    static constexpr uint32_t    NUM_STD_NORM_SAMPLES          = 1 << 18;
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

    // Lock to protect the shared semaphore state.
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

    // Array of pre-generated standard normal samples. Note that we only
    // create a single static instance of the object, as this is a singleton
    // class. Because of this the object will be allocated in data/bss section.
    // Therefore it is safe to use large statically allocated array.
    std::array<double, NUM_STD_NORM_SAMPLES> m_std_normals;
    uint64_t                                 m_std_normals_idx;

    // Last time the controller ran.
    uint64_t m_last_time;

#ifdef M_SEM_DEBUG
    // Average capacity.
    double      m_avg_cap;
    uint64_t    m_avg_count;
#endif  // M_SEM_DEBUG

    // Helper function which performs the core capacity control logic.
    void UpdateCapacity();

public:
    MemSemaphoreOptImpl(uint32_t init_cap = DEF_INIT_CAP);
    ~MemSemaphoreOptImpl();

    bool TryWait() override;
    void Post() override;
    int GetCapacity() override {
        return m_cap;
    }
};


#endif  // __M_SEMAPHORE_OPT_IMPL_HPP__
