extern "C" {
#include <numa.h>
#include <sched.h>
}

#include <cmath>
#include <random>
#include "cpucounters.h"
#include "benchmark/benchmark.h"

/**
 * Helper classes
 */

/**
 * Mock state and controller implementation state
 */


// Mock state (as we are not running these benchmarks in a real runtime)
#define MAX_CORES       (45)
#define CACHE_LINE_SIZE (64)

// Generic Constants.
static const uint64_t    CTL_DELAY_US                  = 3000;
static const uint32_t    DEF_INIT_CAP                  = 1;
static const uint32_t    NUM_STD_NORM_SAMPLES          = 1 << 18;
static const int         WINDOW_SZ                     = 10;

// Reward function specific constants.
static const double      ALPHA                         = 0.8;
static const double      TARGET_NORM_MEMBW             = 1.0;
static const double      MAGNIFYER                     = 100.0;

// Thompson Sampling specific constants.
//
// NOTE: These values depend on the range of the rewards.
// Known variance of the true reward distribution.
static const double      SIGMA_SQ                      = 25.0;
// Prior variance of the unknown mean reward.
static const double      SIGMA0_SQ                     = 625.0;
// Prior mean of the unknown mean reward.
static const double      MU0                           = 80.0;

// Softmax method specific constants.
// Exploration probability.
static const double      EXPLR_PROB                    = 0.3;
// Temperature parameter.
static const double      TAU                           = 30.0;



// State common for the two implementations of the controller
uint32_t m_cap;
uint32_t m_count;
uint32_t m_max_cap;
double   m_max_membw;
double   m_samples[MAX_CORES+1][WINDOW_SZ];
int      m_samples_idx[MAX_CORES+1];
uint64_t m_counts[MAX_CORES+1];
double   m_sums[MAX_CORES+1];
double   m_means[MAX_CORES+1];
double   m_vars[MAX_CORES+1];
double   m_std_devs[MAX_CORES+1];
double   m_softmax_probs[MAX_CORES+1];
uint64_t m_last_time;

// State for the simple implementation of the controller
std::random_device                      m_rd;
std::mt19937                            m_gen;
std::normal_distribution<double>        m_std_normal;
std::unique_ptr<pcm::PCM>               m_pcm;
std::unique_ptr<pcm::ServerUncorePMUs>  m_unc;
size_t                                  m_num_mem_ch;
uint64_t                                m_last_bytes;

// State for the optimized implementation of the controller
std::array<double, NUM_STD_NORM_SAMPLES> m_std_normals;
uint64_t                                 m_std_normals_idx;


/**
 * Helper functions
 */

// Get the cacheline reads and writes on a single memory channel.
inline uint64_t PcmGetMcAccesses(uint32_t channel) {

    uint64_t reads = m_unc->getMCCounter(
        channel, pcm::ServerUncorePMUs::EventPosition::READ);
    uint64_t writes = m_unc->getMCCounter(
        channel, pcm::ServerUncorePMUs::EventPosition::WRITE);
    return reads + writes;
}


// Get the active memory channel count on the host.
//
// NOTE: This is needed as we can have unpopulated memory channels as well.
uint64_t PcmGetActiveMcCount() {

    uint32_t count = 0;
    for (uint32_t i = 0; i < m_unc->getNumMCChannels(); ++i) {
        count += PcmGetMcAccesses(i) != 0;
    }
    return count;
}


double RuntimeMemBwUsage() {
    static double membw_usage = 175000.0;
    return membw_usage;
}


/**
 * Unoptimized controller benchmarks
 */


static void BM_GetMemBw(benchmark::State& state) {

    // Initialize some dummy state
    m_last_bytes = 0;
    m_last_time = 0;
    uint64_t now_time = 1000;

    for (auto _ : state) {
        uint64_t now_bytes = PcmGetMcAccesses(0) * CACHE_LINE_SIZE * m_num_mem_ch;
        double membw = (double)(now_bytes - m_last_bytes) / (double)(now_time - m_last_time);
        if (m_max_membw < membw){
            m_max_membw = membw;
        }

    }
}
BENCHMARK(BM_GetMemBw);


static void BM_CalculateReward(benchmark::State& state) {

    // Initialize some dummy state
    double membw = 175000.0;
    m_max_membw = 190000.0;
    m_cap = 12;
    m_max_cap = MAX_CORES;

    for (auto _ : state) {
        double norm_membw = membw / m_max_membw;
        norm_membw = std::min(norm_membw, TARGET_NORM_MEMBW) / TARGET_NORM_MEMBW;
        double norm_cap = (double)m_cap / (double)m_max_cap;
        double reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cap;
        reward = MAGNIFYER * reward;
    }
}
BENCHMARK(BM_CalculateReward);


static void BM_AppendToWindow(benchmark::State& state) {

    // Initialize some dummy state
    m_cap = 12;
    m_samples_idx[m_cap] = 0;
    m_sums[m_cap] = 0;
    m_counts[m_cap] = 0;
    double reward = 0.75;

    for (auto _ : state) {
        double old_reward = m_samples[m_cap][m_samples_idx[m_cap]];
        m_samples[m_cap][m_samples_idx[m_cap]] = reward;
        m_samples_idx[m_cap] = (m_samples_idx[m_cap] + 1) % WINDOW_SZ;
        ++m_counts[m_cap];
        if (m_counts[m_cap] > WINDOW_SZ) {
            m_counts[m_cap] = WINDOW_SZ;
        }
        m_sums[m_cap] = m_sums[m_cap] + reward - old_reward;

    }
}
BENCHMARK(BM_AppendToWindow);


static void BM_UpdatePosterior(benchmark::State& state) {

    // Initialize some dummy state
    m_cap = 12;
    m_counts[m_cap] = WINDOW_SZ;
    m_sums[m_cap] = 500.0;

    for (auto _ : state) {
        m_vars[m_cap] = 1.0 / ((1.0/SIGMA0_SQ) + ((double)m_counts[m_cap]/SIGMA_SQ));
        m_means[m_cap] = m_vars[m_cap] * (MU0/SIGMA0_SQ + m_sums[m_cap]/SIGMA_SQ);
        m_std_devs[m_cap] = std::sqrt(m_vars[m_cap]);
    }
}
BENCHMARK(BM_UpdatePosterior);


static void BM_SoftmaxSampling(benchmark::State& state) {

    // Initialize some dummy state
    std::uniform_real_distribution<double> dist(0.0, 80.0);
    for (int i = 1; i <= MAX_CORES; ++i) {
        m_means[i] = dist(m_gen);
    }
    m_cap = MAX_CORES;
    m_max_cap = MAX_CORES;

    for (auto _ : state) {
        double max_mean = -100000000.0;
        for (uint32_t i = 1; i <= m_cap; ++i) {
            if (max_mean < m_means[i]) {
                max_mean = m_means[i];
            }
        }

        // Calculate the softmax probabilities
        double sum = 0.0;
        for (uint32_t i = 1; i <= m_cap; ++i) {
            m_softmax_probs[i] = std::exp((m_means[i] - max_mean) / TAU);
            sum += m_softmax_probs[i];
        }
        for (uint32_t i = 1; i <= m_cap; ++i) {
            m_softmax_probs[i] = m_softmax_probs[i] / sum;
        }

        // Sample an arm from the probability distribution
        double arm_prob = ((double)(rand() % 1001)) / 1000.0;
        for (uint32_t i = 1; i <= m_cap; ++i) {
            if (arm_prob < m_softmax_probs[i]) {
                m_cap = i;
                break;
            }
            arm_prob = arm_prob - m_softmax_probs[i];
        }

        // Dummy code
        m_cap = MAX_CORES;
    }
}
BENCHMARK(BM_SoftmaxSampling);


static void BM_ThompsonSampling(benchmark::State& state) {

    // Initialize some dummy state
    std::uniform_real_distribution<double> dist(0.0, 80.0);
    for (int i = 1; i <= MAX_CORES; ++i) {
        m_means[i] = dist(m_gen);
        m_std_devs[i] = dist(m_gen);
    }
    m_max_cap = MAX_CORES;

    for (auto _ : state) {
        double max_mean = -100000000.0;
        for (uint32_t i = 1; i <= m_max_cap; ++i) {
            double z = m_std_normal(m_gen);
            double mean = m_means[i] + m_std_devs[i] * z;
            if (mean > max_mean) {
                max_mean = mean;
                m_cap = i;
            }
        }
    }
}
BENCHMARK(BM_ThompsonSampling);


static void BM_UpdateCapacity(benchmark::State& state) {

    // Initialize some dummy state
    m_last_bytes = 0;
    m_last_time = 0;
    m_cap = 12;
    m_max_cap = MAX_CORES;
    std::uniform_real_distribution<double> dist(0.0, 80.0);
    for (int i = 1; i <= MAX_CORES; ++i) {
        m_counts[i] = 0;
        m_sums[i] = 0;
        m_means[i] = dist(m_gen);
        m_vars[i] = dist(m_gen);
        m_std_devs[i] = std::sqrt(m_vars[i]);
    }
    uint64_t now_time = 1000;

    for (auto _ : state) {
        uint64_t now_bytes = PcmGetMcAccesses(0) * CACHE_LINE_SIZE * m_num_mem_ch;
        double membw = (double)(now_bytes - m_last_bytes) / (double)(now_time - m_last_time);

        if (m_max_membw < membw) {
            m_max_membw = membw;
        }

        double norm_membw = membw / m_max_membw;
        norm_membw = std::min(norm_membw, TARGET_NORM_MEMBW) / TARGET_NORM_MEMBW;
        double norm_cap = (double)m_cap / (double)m_max_cap;
        double reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cap;
        reward = MAGNIFYER * reward;

        double old_reward = m_samples[m_cap][m_samples_idx[m_cap]];
        m_samples[m_cap][m_samples_idx[m_cap]] = reward;
        m_samples_idx[m_cap] = (m_samples_idx[m_cap] + 1) % WINDOW_SZ;

        ++m_counts[m_cap];
        if (m_counts[m_cap] > WINDOW_SZ) {
            m_counts[m_cap] = WINDOW_SZ;
        }
        m_sums[m_cap] = m_sums[m_cap] + reward - old_reward;

        double explr_prob = ((double)(rand() % 1001)) / 1000.0;
        if (explr_prob < EXPLR_PROB) {
            double max_mean = -100000000.0;
            for (uint32_t i = 1; i <= m_cap; ++i) {
                if (max_mean < m_means[i]) {
                    max_mean = m_means[i];
                }
            }
            double sum = 0.0;
            for (uint32_t i = 1; i <= m_cap; ++i) {
                m_softmax_probs[i] = std::exp((m_means[i] - max_mean) / TAU);
                sum += m_softmax_probs[i];
            }
            for (uint32_t i = 1; i <= m_cap; ++i) {
                m_softmax_probs[i] = m_softmax_probs[i] / sum;
            }
            double arm_prob = ((double)(rand() % 1001)) / 1000.0;
            for (uint32_t i = 1; i <= m_cap; ++i) {
                if (arm_prob < m_softmax_probs[i]) {
                    m_cap = i;
                    break;
                }
                arm_prob = arm_prob - m_softmax_probs[i];
            }
        } else {
            double max_mean = -100000000.0;
            for (uint32_t i = 1; i <= m_max_cap; ++i) {
                double z = m_std_normal(m_gen);
                double mean = m_means[i] + m_std_devs[i] * z;
                if (mean > max_mean) {
                    max_mean = mean;
                    m_cap = i;
                }
            }
        }

        m_last_bytes = now_bytes;
        m_last_time = now_time;
        now_time += 1000;       // dummy
        m_cap = MAX_CORES;      // dummy
    }
}
BENCHMARK(BM_UpdateCapacity);


/**
 * Optimized controller benchmarks
 */

static void BM_GetMemBwOpt(benchmark::State& state) {

    for (auto _ : state) {
        double membw = RuntimeMemBwUsage();
    }
}
BENCHMARK(BM_GetMemBwOpt);


static void BM_ThompsonSamplingOpt(benchmark::State& state) {

    // Initialize some dummy state
    std::uniform_real_distribution<double> dist(0.0, 80.0);
    for (int i = 1; i <= MAX_CORES; ++i) {
        m_means[i] = dist(m_gen);
        m_std_devs[i] = dist(m_gen);
    }
    m_std_normals_idx = 0;
    m_max_cap = MAX_CORES;

    for (auto _ : state) {
        double max_mean_reward = -100000000.0;
        for (uint32_t i = 1; i <= m_max_cap; ++i) {
            double z = m_std_normals[m_std_normals_idx++];
            m_std_normals_idx = m_std_normals_idx & (NUM_STD_NORM_SAMPLES - 1);
            double mean_reward = m_means[i] + m_std_devs[i] * z;
            if (mean_reward > max_mean_reward) {
                max_mean_reward = mean_reward;
                m_cap = i;
            }
        }
    }
}
BENCHMARK(BM_ThompsonSamplingOpt);

static void BM_UpdateCapacityOpt(benchmark::State& state) {

    // Initialize some dummy state
    m_last_bytes = 0;
    m_last_time = 0;
    m_cap = 12;
    m_max_cap = MAX_CORES;
    std::uniform_real_distribution<double> dist(0.0, 80.0);
    for (int i = 1; i <= MAX_CORES; ++i) {
        m_counts[i] = 0;
        m_sums[i] = 0;
        m_means[i] = dist(m_gen);
        m_vars[i] = dist(m_gen);
        m_std_devs[i] = std::sqrt(m_vars[i]);
    }
    m_std_normals_idx = 0;
    uint64_t now_time = 1000;

    for (auto _ : state) {
        double membw = RuntimeMemBwUsage();

        if (m_max_membw < membw) {
            m_max_membw = membw;
        }

        double norm_membw = membw / m_max_membw;
        norm_membw = std::min(norm_membw, TARGET_NORM_MEMBW) / TARGET_NORM_MEMBW;
        double norm_cap = (double)m_cap / (double)m_max_cap;
        double reward = ALPHA * norm_membw - (1 - ALPHA) * norm_cap;
        reward = MAGNIFYER * reward;

        double old_reward = m_samples[m_cap][m_samples_idx[m_cap]];
        m_samples[m_cap][m_samples_idx[m_cap]] = reward;
        m_samples_idx[m_cap] = (m_samples_idx[m_cap] + 1) % WINDOW_SZ;

        ++m_counts[m_cap];
        if (m_counts[m_cap] > WINDOW_SZ) {
            m_counts[m_cap] = WINDOW_SZ;
        }
        m_sums[m_cap] = m_sums[m_cap] + reward - old_reward;

        double explr_prob = ((double)(rand() % 1001)) / 1000.0;
        if (explr_prob < EXPLR_PROB) {
            double max_mean = -100000000.0;
            for (uint32_t i = 1; i <= m_cap; ++i) {
                if (max_mean < m_means[i]) {
                    max_mean = m_means[i];
                }
            }
            double sum = 0.0;
            for (uint32_t i = 1; i <= m_cap; ++i) {
                m_softmax_probs[i] = std::exp((m_means[i] - max_mean) / TAU);
                sum += m_softmax_probs[i];
            }
            for (uint32_t i = 1; i <= m_cap; ++i) {
                m_softmax_probs[i] = m_softmax_probs[i] / sum;
            }
            double arm_prob = ((double)(rand() % 1001)) / 1000.0;
            for (uint32_t i = 1; i <= m_cap; ++i) {
                if (arm_prob < m_softmax_probs[i]) {
                    m_cap = i;
                    break;
                }
                arm_prob = arm_prob - m_softmax_probs[i];
            }
        } else {
            double max_mean_reward = -100000000.0;
            for (uint32_t i = 1; i <= m_max_cap; ++i) {
                double z = m_std_normals[m_std_normals_idx++];
                m_std_normals_idx = m_std_normals_idx & (NUM_STD_NORM_SAMPLES - 1);
                double mean_reward = m_means[i] + m_std_devs[i] * z;
                if (mean_reward > max_mean_reward) {
                    max_mean_reward = mean_reward;
                    m_cap = i;
                }
            }
        }

        m_last_time = now_time;
        now_time += 1000;       // dummy
        m_cap = MAX_CORES;      // dummy
    }
}
BENCHMARK(BM_UpdateCapacityOpt);




int main(int argc, char** argv) {

    // Perform common, one-time, and heavy initialization here

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
    m_num_mem_ch = PcmGetActiveMcCount();
    m_last_bytes = 0;

    // Re-nable the PCM logs
    std::cerr.rdbuf(oldbuf);

    // Initialize the gaussian random number generator
    m_gen = std::mt19937(m_rd());
    m_std_normal = std::normal_distribution<double>(0.0, 1.0);

    // Initialize the pre-generated gaussian samples
    for (uint64_t i = 0; i < NUM_STD_NORM_SAMPLES; ++i) {
        m_std_normals[i] = m_std_normal(m_gen);
    }
    m_std_normals_idx = 0;

    // Run the benchmark
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
}
