extern "C" {
#include <numa.h>
#include <sched.h>
#include <asm/cpu.h>
}

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#include "mem_pmc_amd_zen2.hpp"


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
    return strtol(buf, nullptr, 0);
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

    char *end = nullptr;
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

MemPmcAmdZen2::MemPmcAmdZen2() {

    // Get the PMU type value
    std::string pmu_type_path = std::string(PMU_DEV_PATH) + "/type";
    long pmu_type = ReadSysfsInt(pmu_type_path.c_str());
    if (pmu_type < 0) {
        fprintf(stderr, "Could not read PMU type.\n");
        assert(false);
    }
    m_pmu_type = pmu_type;

    // Get one representative CPU from the PMU cpumask
    std::string pmu_cpumask_path = std::string(PMU_DEV_PATH) + "/cpumask";
    int pmu_cpu = ReadFirstCpuFromCpumask(pmu_cpumask_path.c_str());
    if (pmu_cpu < 0) {
        fprintf(stderr, "Could not read PMU cpumask.\n");
        assert(false);
    }
    m_pmu_cpu = pmu_cpu;

    // Open a perf handle for every memory channel
    m_max_num_mem_ch = MAX_NUM_MEM_CH;
    m_num_mem_ch = 0;
    for (uint32_t i = 0; i < m_max_num_mem_ch; ++i) {
        // Configure the perf event we want to count
        struct perf_event_attr pe = {};
        pe.size = sizeof(pe);
        pe.type = (uint32_t)pmu_type;
        pe.config = CHAN_CTRL_PERF_EVENT_CONFIGS[i];
        pe.disabled = 1;

        // Open the perf handle
        m_chan_fd[i] = PerfEventOpen(&pe, -1, pmu_cpu, -1, 0);
        if (m_chan_fd[i] < 0) {
            fprintf(stderr, "Skipping channel %d (perf_event_open: %s)\n",
                    i, strerror(errno));
            continue;
        }
        m_num_mem_ch++;
    }

    // Enable the counters
    for (uint32_t i = 0; i < m_max_num_mem_ch; ++i) {
        if (m_chan_fd[i] < 0) {
            continue;
        }
        ioctl(m_chan_fd[i], PERF_EVENT_IOC_RESET, 0);
        ioctl(m_chan_fd[i], PERF_EVENT_IOC_ENABLE, 0);
    }
}

MemPmcAmdZen2::~MemPmcAmdZen2() {

    for (uint32_t i = 0; i < m_max_num_mem_ch; ++i) {
        if (m_chan_fd[i] < 0) {
            continue;
        }
        close(m_chan_fd[i]);
    }
}

uint64_t MemPmcAmdZen2::GetActiveMemChan() {
    return m_num_mem_ch;
}

uint64_t MemPmcAmdZen2::GetMemChanAccesses(int chan) {

    uint64_t accesses = 0;

    // Verify channel number is valid
    if (chan < 0 || chan >= m_max_num_mem_ch) {
        return 0;
    }

    // Verify the channel is active
    if (m_chan_fd[chan] < 0) {
        return 0;
    }

    // Read the counters
    ssize_t ret = read(m_chan_fd[chan], &accesses, sizeof(accesses));
    if (ret != (ssize_t)sizeof(accesses)) {
        return 0;
    }

    return accesses * CACHE_LINE_SIZE;
}

uint64_t MemPmcAmdZen2::GetMemAccesses() {

    uint64_t total = 0;

    for (uint32_t i = 0; i < m_max_num_mem_ch; ++i) {
        total += GetMemChanAccesses(i);
    }

    return total;
}
