#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <numa.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>


#include "../pcm.h"
#include "asm/cpu.h"
#include "mem_pmc_amd_zen2.h"


/* AMD Zen2-specific constants */
static char *PMU_DEV_PATH = "/sys/bus/event_source/devices/amd_df";
static uint64_t CHAN_CTRL_PERF_EVENT_CONFIGS[MAX_NUM_MEM_CH] = {
    0x3807ULL,
    0x3847ULL,
    0x3887ULL,
    0x38c7ULL,
    0x100003807ULL,
    0x100003847ULL,
    0x100003887ULL,
    0x1000038c7ULL,
};

static MemPmcAmdZen2State *state = NULL;


static long ReadSysfsInt(const char *path) {

    // Open the sysctl file
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    // Read the integer value as a string
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    // Convert the string to integer
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    return strtol(buf, NULL, 0);
}

static int ReadFirstCpuFromCpumask(const char *path) {

    // Open the sysctl file
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    // Read the string CPU list. It can be in the following formats.
    //   "0"
    //   "0-7"
    //   "0,8"
    //   "0-3,8-11"
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        fprintf(stderr, "Cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }
    buf[n] = '\0';

    // We only need one valid CPU from the mask, so parse the first number.
    char *p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;

    if (*p == '\0')
        return -1;

    char *end = NULL;
    long cpu = strtol(p, &end, 10);
    if (end == p || cpu < 0) {
        fprintf(stderr, "Invalid cpumask format in %s: %s\n", path, buf);
        return -1;
    }

    return (int)cpu;
}

static int PerfEventOpen(
    struct perf_event_attr *attr,
    pid_t pid,
    int cpu,
    int group_fd,
    unsigned long flags) {

    return (int)syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}


void MemPmc_AmdZen2_Init() {

    char path[4097];

    /* Allocate the state */
    state = (MemPmcAmdZen2State *)malloc(sizeof(MemPmcAmdZen2State));
    assert(state);

    // Get the PMU type value
    strcpy(path, PMU_DEV_PATH);
    strcat(path, "/type");
    long pmu_type = ReadSysfsInt(path);
    if (pmu_type < 0) {
        fprintf(stderr, "Could not read PMU type.\n");
        free(state);
        state = NULL;
        return;
    }
    state->m_pmu_type = pmu_type;

    // Get one representative CPU from the PMU cpumask
    strcpy(path, PMU_DEV_PATH);
    strcat(path, "/cpumask");
    int pmu_cpu = ReadFirstCpuFromCpumask(path);
    if (pmu_cpu < 0) {
        fprintf(stderr, "Could not read PMU cpumask.\n");
        free(state);
        state = NULL;
        return;
    }
    state->m_pmu_cpu = pmu_cpu;

    // Open a perf handle for every memory channel
    state->m_max_num_mem_ch = MAX_NUM_MEM_CH;
    state->m_num_mem_ch = 0;
    for (uint32_t i = 0; i < state->m_max_num_mem_ch; ++i) {
        // Configure the perf event we want to count
        struct perf_event_attr pe = {};
        pe.size = sizeof(pe);
        pe.type = (uint32_t)pmu_type;
        pe.config = CHAN_CTRL_PERF_EVENT_CONFIGS[i];
        pe.disabled = 1;

        // Open the perf handle
        state->m_chan_fd[i] = PerfEventOpen(&pe, -1, pmu_cpu, -1, 0);
        if (state->m_chan_fd[i] < 0) {
            fprintf(stderr, "Skipping channel %d (perf_event_open: %s)\n",
                    i, strerror(errno));
            continue;
        }
        state->m_num_mem_ch++;
    }

    // Enable the counters
    for (uint32_t i = 0; i < state->m_max_num_mem_ch; ++i) {
        if (state->m_chan_fd[i] < 0) {
            continue;
        }
        ioctl(state->m_chan_fd[i], PERF_EVENT_IOC_RESET, 0);
        ioctl(state->m_chan_fd[i], PERF_EVENT_IOC_ENABLE, 0);
    }
}


uint64_t MemPmc_AmdZen2_GetMaxMemChan() {
    if (!state) {
        return 0;
    }
    return state->m_max_num_mem_ch;
}


uint64_t MemPmc_AmdZen2_GetActiveMemChan() {
    if (!state) {
        return 0;
    }
    return state->m_num_mem_ch;
}


uint64_t MemPmc_AmdZen2_GetMemChanAccesses(int chan) {

    if (!state) {
        return 0;
    }

    uint64_t accesses = 0;

    // Verify channel number is valid
    if (chan < 0 || chan >= state->m_max_num_mem_ch) {
        return 0;
    }

    // Verify the channel is active
    if (state->m_chan_fd[chan] < 0) {
        return 0;
    }

    // Read the counters
    ssize_t ret = read(state->m_chan_fd[chan], &accesses, sizeof(accesses));
    if (ret != (ssize_t)sizeof(accesses)) {
        return 0;
    }

    return accesses * CACHE_LINE_SIZE;
}


uint64_t MemPmc_AmdZen2_GetMemAccesses() {

    if (!state) {
        return 0;
    }

    uint64_t total = 0;
    uint64_t accesses = 0;
    ssize_t ret;

    for (uint32_t i = 0; i < state->m_max_num_mem_ch; ++i) {
        // Verify the channel is active
        if (state->m_chan_fd[i] < 0) {
            continue;
        }
        // Read the counters
        ret = read(state->m_chan_fd[i], &accesses, sizeof(accesses));
        if (ret != (ssize_t)sizeof(accesses)) {
            continue;
        }
        total += accesses;
    }

    return total * CACHE_LINE_SIZE;
}


void MemPmc_AmdZen2_DeInit() {

    if (!state) {
        return;
    }

    /* Close the perf descriptors */
    for (uint32_t i = 0; i < state->m_max_num_mem_ch; ++i) {
        if (state->m_chan_fd[i] < 0) {
            continue;
        }
        close(state->m_chan_fd[i]);
    }

    free(state);
    state = NULL;
}

MemPmcOps mem_pmc_amd_zen2_ops = {
    .Init = MemPmc_AmdZen2_Init,
    .GetMaxMemChan = MemPmc_AmdZen2_GetMaxMemChan,
    .GetActiveMemChan = MemPmc_AmdZen2_GetActiveMemChan,
    .GetMemChanAccesses = MemPmc_AmdZen2_GetMemChanAccesses,
    .GetMemAccesses = MemPmc_AmdZen2_GetMemAccesses,
    .DeInit = MemPmc_AmdZen2_DeInit
};
