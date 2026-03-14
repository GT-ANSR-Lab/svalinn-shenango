#include <cassert>

#include "cpu_id.hpp"
#include "mem_pmc.hpp"
#include "mem_pmc_intel.hpp"
#include "mem_pmc_amd_zen2.hpp"


MemPmc::MemPmc() {

}

MemPmc::~MemPmc() {

}

void MemPmc::Init() {
    CpuIdInfo cpu_info;

    if (cpu_info.IsIntel()) {
        m_impl = std::make_unique<MemPmcIntel>();
    } else if (cpu_info.IsAmd()) {
        if (cpu_info.IsAmdZen2()) {
            m_impl = std::make_unique<MemPmcAmdZen2>();
        } else {
            // Not supported
            assert(false);
        }
    } else {
        // Not supported
        assert(false);
    }
}

uint64_t MemPmc::GetActiveMemChan() {
    return m_impl->GetActiveMemChan();
}

uint64_t MemPmc::GetMemChanAccesses(int chan) {
    return m_impl->GetMemChanAccesses(chan);
}

uint64_t MemPmc::GetMemAccesses() {
    return m_impl->GetMemAccesses();
}
