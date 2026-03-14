#ifndef __MEM_PMC_INTEL_HPP__
#define __MEM_PMC_INTEL_HPP__

#include <memory>

#include "mem_pmc.hpp"
#include "cpucounters.h"


class MemPmcIntel : public MemPmcImpl {
private:
    // Intel-PCM objects required to access the uncore counters
    std::unique_ptr<pcm::PCM>               m_pcm;
    std::unique_ptr<pcm::ServerUncorePMUs>  m_unc;
    size_t                                  m_num_mem_ch;
    size_t                                  m_max_num_mem_ch;

public:
    MemPmcIntel();
    ~MemPmcIntel();

    uint64_t GetActiveMemChan() override;
    uint64_t GetMemChanAccesses(int chan) override;
    uint64_t GetMemAccesses() override;
};

#endif // __MEM_PMC_INTEL_HPP__
