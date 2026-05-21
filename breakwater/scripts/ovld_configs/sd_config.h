/*
 * sd_config.h - SEDA configurations
 */

#pragma once

/*
 * Parameters used in Svalinn evaluation (on xl170)
 *
 * Netbench (cpu+mem+lock)
 * #define SEDA_TARGET			500
 * #define SEDA_TIMEOUT			1000
 * #define SEDA_ADJ_I			5.0
 * #define SEDA_ADJ_D			1.3
 * #define SEDA_QDELAY_THRESH   200
 *
 * RocksDB
 * #define SEDA_TARGET			1250
 * #define SEDA_TIMEOUT			2000
 * #define SEDA_ADJ_I			5.0
 * #define SEDA_ADJ_D			1.3
 * #define SEDA_QDELAY_THRESH   200
 *
 * Dataframe
 * #define SEDA_TARGET			3750
 * #define SEDA_TIMEOUT			7000
 * #define SEDA_ADJ_I			12.0
 * #define SEDA_ADJ_D			1.1
 * #define SEDA_QDELAY_THRESH   2200
 *
 * Memcached
 * #define SEDA_TARGET			1375
 * #define SEDA_TIMEOUT			2000
 * #define SEDA_ADJ_I			5.0
 * #define SEDA_ADJ_D			1.3
 * #define SEDA_QDELAY_THRESH   440
 *
 */


/* maximum client delay */
#define CSD_MAX_CLIENT_DELAY_US		100
/* Token bucket initial rate (reqs/sec) */
#define CSD_TB_INIT_RATE		4
/* Token bucket minimum rate (reqs/sec) */
#define CSD_TB_MIN_RATE			2
/* Token bucket maximum number of token (burstiness) */
#define CSD_TB_MAX_TOKEN		4
/* EWMA filter constant */
#define SEDA_ALPHA			0.7
/* target 90th percentile delay */
#define SEDA_TARGET			3750
/* time before controller run */
#define SEDA_TIMEOUT			7000
/* % error to trigger decrease */
#define SEDA_ERR_D			0.0
/* % error to trigger increase */
#define SEDA_ERR_I			-0.5
/* additive rate increase */
#define SEDA_ADJ_I			12.0
/* multiplicative rate decrease */
#define SEDA_ADJ_D			1.1
/* weight on additive increase */
#define SEDA_CI				-0.1
/* Maximum tolerable queueing delay before running the request handler */
#define SEDA_QDELAY_THRESH  200
