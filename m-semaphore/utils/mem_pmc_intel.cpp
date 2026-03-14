extern "C" {
#include <numa.h>
#include <sched.h>
#include <asm/cpu.h>
}

#include <cstdio>

#include "mem_pmc_intel.hpp"


MemPmcIntel::MemPmcIntel() {

    // Get the calling cpu's numa node
    uint32_t cpu = sched_getcpu();
    uint32_t node = numa_node_of_cpu(cpu);

    // Disable the PCM logs
    std::streambuf* oldbuf = std::cerr.rdbuf();
    pcm::null_stream nullStream;
    std::cerr.rdbuf(&nullStream);

    // Initialize the memory bandwidth monitoring related state
    m_pcm.reset(pcm::PCM::getInstance());
    m_unc = std::make_unique<pcm::ServerUncorePMUs>(node, m_pcm.get());
    m_unc->program();

    // Configure the number of active memory channels
    m_max_num_mem_ch = m_unc->getNumMCChannels();
    m_num_mem_ch = 0;
    for (uint32_t i = 0; i < m_max_num_mem_ch; ++i) {
        m_num_mem_ch += GetMemChanAccesses(i) != 0;
    }

    // Re-nable the PCM logs
    std::cerr.rdbuf(oldbuf);
}

MemPmcIntel::~MemPmcIntel() {
    m_pcm->cleanup();
}

uint64_t MemPmcIntel::GetActiveMemChan() {
    return m_num_mem_ch;
}

uint64_t MemPmcIntel::GetMemChanAccesses(int chan) {

    uint64_t reads = m_unc->getMCCounter(
        chan, pcm::ServerUncorePMUs::EventPosition::READ);
    uint64_t writes = m_unc->getMCCounter(
        chan, pcm::ServerUncorePMUs::EventPosition::WRITE);
    return (reads + writes) * CACHE_LINE_SIZE;
}

uint64_t MemPmcIntel::GetMemAccesses() {

    uint64_t total = 0;

    for (uint32_t i = 0; i < m_max_num_mem_ch; ++i) {
        total += GetMemChanAccesses(i);
    }

    return total;
}
