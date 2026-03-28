/*
 * pcc_config.h - PCC configurations
 */

#pragma once

#include <stdint.h>
#include <breakwater/pcc.h>

/* AQM drop threshold */
#define SPCC_QDELAY_BUDGET            200

/* Round trip time in us */
#define SPCC_RTT_US                   10

/* Duration to wait before starting the monitoring of a microexperiment */
#define SPCC_PRE_MI_US              (200)

/* Monitor interval duration in microseconds. */
#define SPCC_MI_US                  (1000)

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
#define SPCC_MICRO_EXP_PERTURB_CB             (0)


static inline double spcc_calc_util_fn_tput(struct spcc_micro_exp_stats *stats) {

    double tput_rps = (double)(stats->out_resps) / (double)(stats->duration);
    tput_rps *= 1000000.0;

    return tput_rps;
}

static inline double spcc_calc_util_fn_gput(struct spcc_micro_exp_stats *stats) {

    double gput_rps = (double)(stats->good_out_resps) / (double)(stats->duration);
    gput_rps *= 1000000.0;

    return gput_rps;
}

static inline double spcc_calc_util_fn_qdelay(struct spcc_micro_exp_stats *stats) {

    double tput_rps = (double)(stats->out_resps) / (double)(stats->duration);
    tput_rps *= 1000000.0;

    if (stats->qdelay >= SPCC_QDELAY_BUDGET) {
        return -tput_rps;
    }

    return tput_rps;
}

static inline double spcc_calc_util_fn_drop(struct spcc_micro_exp_stats *stats) {

    double drop_rate = (double)(stats->drop_reqs) / (double)(stats->in_reqs);
    double dropped_rps = (double)(stats->drop_reqs) / (double)(stats->duration);
    double tput_rps = (double)(stats->out_resps) / (double)(stats->duration);
    dropped_rps *= 1000000.0;
    tput_rps *= 1000000.0;

    if (drop_rate >= 0.1) {
        return -dropped_rps;
    }

    return tput_rps - dropped_rps;
}

static inline double spcc_calc_util_fn_power(struct spcc_micro_exp_stats *stats) {

    /* XXX: This utility requires the monitor interval to be at least 1ms,
     *      as RAPL (power/energy) counters are updated by the hardware
     *      every ~1ms. Using 2ms monitor interval duration works well.
     */

    double duration_sec = (double)stats->duration / 1000000.0;
    double power = stats->energy_consumed / duration_sec;
    double tput_rps = (double)stats->out_resps / duration_sec;

    if (power >= 55.0) {
        return -tput_rps;
    }

    return tput_rps;
}

/* The utility calculation function */
static inline double spcc_calc_util_fn(struct spcc_micro_exp_stats *stats) {

    return spcc_calc_util_fn_tput(stats);
    /* return spcc_calc_util_fn_gput(stats); */
    /* return spcc_calc_util_fn_qdelay(stats); */
    /* return spcc_calc_util_fn_drop(stats); */
    /* return spcc_calc_util_fn_power(stats); */
}

/* The utility comparison function */
static inline enum spcc_dir spcc_comp_util_fn_simple(
    struct spcc_micro_exp_stats *minus_stats,
    struct spcc_micro_exp_stats *plus_stats) {

    if (plus_stats->utility > minus_stats->utility) {
        return SPCC_DIR_PLUS;
    }

    return SPCC_DIR_MINUS;
}

static inline enum spcc_dir spcc_comp_util_fn_protego(
    struct spcc_micro_exp_stats *minus_stats,
    struct spcc_micro_exp_stats *plus_stats) {

    double in_reqs_delta = (int)plus_stats->in_reqs - (int)minus_stats->in_reqs;
    double out_resps_delta = (int)plus_stats->good_out_resps - (int)minus_stats->good_out_resps;
    double slope = out_resps_delta / in_reqs_delta;

    if (slope > 0.1) {
        return SPCC_DIR_PLUS;
    }

    return SPCC_DIR_MINUS;
}

static inline enum spcc_dir spcc_comp_util_fn(
    struct spcc_micro_exp_stats *minus_stats,
    struct spcc_micro_exp_stats *plus_stats) {

    return spcc_comp_util_fn_simple(minus_stats, plus_stats);
    /* return spcc_comp_util_fn_protego(minus_stats, plus_stats); */
}


/* The maximum supported window size */
#define SPCC_MAX_WINDOW_EXP         6
#define SPCC_MAX_WINDOW             64

#define CPCC_MAX_CLIENT_DELAY_US    10
