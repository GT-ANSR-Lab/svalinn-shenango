#ifndef __MEM_PMC_AMD_ZEN2_HPP__
#define __MEM_PMC_AMD_ZEN2_HPP__

#include "mem_pmc.hpp"


class MemPmcAmdZen2 : public MemPmcImpl {
private:
    // For Zen2 CPU(s), AMD Data Fabric Counters expose the memory channel
    // counters, required to calculate the memory bandwidth.
    inline static constexpr char PMU_DEV_PATH[] = "/sys/bus/event_source/devices/amd_df";
    // Maximium number of memory channels are 8 for Zen2 CPU(s).
    inline static constexpr uint64_t MAX_NUM_MEM_CH = 8;
    // The perf event config value can be found using the perf command.
    // This are fixed given the CPU vendor, family, and model are the same.
    // Moreover, many models under the same family share these config values.
    inline static constexpr uint64_t CHAN_CTRL_PERF_EVENT_CONFIGS[MAX_NUM_MEM_CH] = {
        0x3807ULL,
        0x3847ULL,
        0x3887ULL,
        0x38c7ULL,
        0x100003807ULL,
        0x100003847ULL,
        0x100003887ULL,
        0x1000038c7ULL,
    };

    // Per-channel file descriptors to read the counters
    long             m_pmu_type;
    int              m_pmu_cpu;
    int              m_chan_fd[MAX_NUM_MEM_CH];
    size_t           m_num_mem_ch;
    size_t           m_max_num_mem_ch;

public:
    MemPmcAmdZen2();
    ~MemPmcAmdZen2();

    uint64_t GetActiveMemChan() override;
    uint64_t GetMemChanAccesses(int chan) override;
    uint64_t GetMemAccesses() override;
};


#endif  // __MEM_PMC_AMD_ZEN2_HPP__
