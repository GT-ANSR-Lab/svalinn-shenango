#ifndef __M_SEMAPHORE_MAB_EG_IMPL_HPP__
#define __M_SEMAPHORE_MAB_EG_IMPL_HPP__

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


class MemSemaphoreMabEgImpl : public MemSemaphoreImpl {
private:
    // Generic Constants.
    static constexpr uint64_t    CTL_DELAY_US                  = 500;
    static constexpr uint32_t    DEF_INIT_CAP                  = 1;

    // Reward function specific constants.
    static constexpr double      ALPHA                         = 0.8;
    static constexpr double      TARGET_NORM_MEMBW             = 1.0;
    static constexpr double      REWARD_EWMA_WEIGHT            = 0.8;

    // Epsilon greedy specific constants.
    // Exploration probability.
    static constexpr double      EXPLR_PROB                    = 0.3;

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

    // EWMA of the rewards for every core count.
    double m_ewma_rewards[NCPU+1];

    // State to calculate the memory bandwidth usage.
    MemPmc      m_mem_pmc;
    size_t      m_num_mem_ch;
    uint64_t    m_last_bytes;

    // Last time the controller ran.
    uint64_t m_last_time;

    // List of threads waiting to acquire the semaphore.
    struct list_head m_waiters;
    uint64_t         m_num_waiters;

#ifdef M_SEM_DEBUG
    // Average capacity.
    double      m_avg_cap;
    uint64_t    m_avg_count;
#endif  // M_SEM_DEBUG

    // Helper function which performs the core capacity control logic.
    void UpdateCapacity();

public:
    MemSemaphoreMabEgImpl(uint32_t init_cap = DEF_INIT_CAP);
    ~MemSemaphoreMabEgImpl();

    bool TryWait() override;
    void Wait() override;
    uint64_t QueueDelayTsc() override;
    uint64_t QueueLength() override;
    void Post() override;
    int GetCapacity() override {
        return m_cap;
    }
};

#endif  // __M_SEMAPHORE_MAB_EG_IMPL_HPP__
