/*
 * pcc_config.h - PCC configurations
 */

#pragma once

/* AQM drop threshold */
#define SPCC_DROP_THRESH            300

/* Round trip time in us */
#define SPCC_RTT_US                 40

/**
 * PCC controller parameters
 */
/* Duration after which we expect a set rate to take effect */
#define SPCC_PRE_MI_US              (200)
/* Monitor interval duration in microseconds. */
#define SPCC_MI_US                  (400)
/* Minimum rate change granularity (in terms of credits). */
#define SPCC_EPSILON_MIN            (1.0)
/* Maximum rate change granularity (in terms of credits). */
#define SPCC_EPSILON_MAX            (5.0)
/* Number of times to repeat the microexperiments. */
#define SPCC_REPS                   (1)
/* Whether to use the rate adjusting state or not
 *
 * XXX: Svalinn evaluation was done with this flag set to False.
 */
#define SPCC_RATE_ADJUSTING_STATE_ENABLED (0)
/* The utility function */
static inline double spcc_util_fn(
    int in_cnt,
    int out_cnt,
    int drop_cnt,
    int qdelay,
    int duration) {

    return (double)(out_cnt) / (double)duration;
}

/* The maximum supported window size */
#define SPCC_MAX_WINDOW_EXP         6
#define SPCC_MAX_WINDOW             64

#define CPCC_MAX_CLIENT_DELAY_US    40

