/*
 * pcc_config.h - PCC configurations
 */

#pragma once

/* AQM drop threshold */
#define SPCC_DROP_THRESH            5120

/* Round trip time in us */
#define SPCC_RTT_US                 40

/**
 * PCC controller parameters
 */
/* Duration after which we expect a set rate to take effect */
#define SPCC_PRE_MI_US              (5 * SPCC_RTT_US)
/* Monitor interval duration in microseconds. */
#define SPCC_MI_US                  (4000)
/* Minimum rate change granularity. */
#define SPCC_EPSILON_MIN            (0.05)
/* Maximum rate change granularity. */
#define SPCC_EPSILON_MAX            (0.10)
/* Number of times to repeat the microexperiments. */
#define SPCC_REPS                   (2)
/* The utility function */
static inline double spcc_util_fn(
    int in_cnt,
    int out_cnt,
    int drop_cnt,
    int qdelay,
    int duration) {

    return (double)(out_cnt - drop_cnt) / (double)duration;
}

/* The maximum supported window size */
#define SPCC_MAX_WINDOW_EXP         6
#define SPCC_MAX_WINDOW             64

#define CPCC_MAX_CLIENT_DELAY_US    40
