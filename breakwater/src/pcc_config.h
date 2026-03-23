/*
 * pcc_config.h - PCC configurations
 */

#pragma once

#include <stdint.h>

/* Queueing delay budget of a request */
#define SPCC_QDELAY_BUDGET            300

/* Round trip time in us */
#define SPCC_RTT_US                 40

/* Duration to wait before starting the monitoring of a microexperiment */
#define SPCC_PRE_MI_US              (200)

/* Monitor interval duration in microseconds. */
#define SPCC_MI_US                  (400)

/* Credit pool change granularity */
#define SPCC_EPSILON                (1)

/* Flag to select lazy or strict labelling of the microexperiments.
 *
 * Strict labelling:
 * The microexperiment in which the credit pool was increased by epsilon
 * is labelled as (+) experiment. The microexperiment in which the credit
 * pool was decreased by epsilon is labelled as (-) experiment. Under
 * strict labelling, the controller expects to receive more load in the
 * (+) experiment than the (-) experiment. If that is not observed,
 * the controller declares the set of experiments as inconclusive,
 * keeping the credit pool size the same, and rerunning the experiments.
 *
 * Lazy labelling:
 * The microexperiment is labelled either (+) or (-) based on whether
 * it received more or less load as compared to the other microexperiment.
 * In this case, the direction perturbation of the credit pool (+epsilon
 * or -epsilon) before starting the microexperiment just acts as a hint and
 * not as a strict label. For example, If the microexperiment with +epsilon
 * perturbation received less load than the microexperiment with -epsilon
 * perturbation, then under lazy labelling, microexperiment with +epsilon
 * perturbation will be labelled as (-) experiment.
 */
#define SPCC_MICRO_EXP_STRICT_LABELLING       (0)

/* Flag to enable or disable perturbing the credit pool
 * by epsilon credits before starting the microexperiments.
 */
#define SPCC_MICRO_EXP_PERTURB_CB             (1)

/* The utility function */
static inline double spcc_util_fn(
    uint64_t in_cnt,
    uint64_t out_cnt,
    uint64_t drop_cnt,
    uint64_t qdelay,
    uint64_t mem_accesses,
    double energy_consumed,
    uint64_t duration) {

    return (double)(out_cnt) / (double)duration;
}

/* The maximum supported window size */
#define SPCC_MAX_WINDOW_EXP         6
#define SPCC_MAX_WINDOW             64

#define CPCC_MAX_CLIENT_DELAY_US    40
