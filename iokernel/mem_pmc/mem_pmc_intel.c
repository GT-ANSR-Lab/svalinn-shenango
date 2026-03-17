#include <assert.h>
#include <stdlib.h>
#include <numa.h>
#include <sched.h>

#include "../pcm.h"
#include "asm/cpu.h"
#include "mem_pmc_intel.h"


static MemPmcIntelState *state = NULL;


void MemPmc_Intel_Init() {

    // Get the calling cpu's numa node
    uint32_t cpu = sched_getcpu();
    uint32_t node = numa_node_of_cpu(cpu);

    /* Init the pcm module */
    pcm_iok_init(node);

    /* Allocate the state */
    state = (MemPmcIntelState *)malloc(sizeof(MemPmcIntelState));
    assert(state);

    /* Initialize the state */
    state->m_num_mem_ch = pcm_iok_get_active_channel_count();
    state->m_max_num_mem_ch = pcm_iok_get_max_channel_count();
}

uint64_t MemPmc_Intel_GetMaxMemChan() {
    assert(state);
    return state->m_max_num_mem_ch;
}

uint64_t MemPmc_Intel_GetActiveMemChan() {
    assert(state);
    return state->m_num_mem_ch;
}

uint64_t MemPmc_Intel_GetMemChanAccesses(int chan) {
    assert(state);
    return pcm_iok_get_cas_count(chan) * CACHE_LINE_SIZE;
}

uint64_t MemPmc_Intel_GetMemAccesses() {
    assert(state);

    uint64_t total = 0;

    for (int i = 0; i < state->m_max_num_mem_ch; ++i) {
        total += pcm_iok_get_cas_count(i);
    }

    return total * CACHE_LINE_SIZE;
}

void MemPmc_Intel_DeInit() {

    pcm_iok_deinit();

    free(state);
    state = NULL;
}


MemPmcOps mem_pmc_intel_ops = {
    .Init = MemPmc_Intel_Init,
    .GetMaxMemChan = MemPmc_Intel_GetMaxMemChan,
    .GetActiveMemChan = MemPmc_Intel_GetActiveMemChan,
    .GetMemChanAccesses = MemPmc_Intel_GetMemChanAccesses,
    .GetMemAccesses = MemPmc_Intel_GetMemAccesses,
    .DeInit = MemPmc_Intel_DeInit
};
