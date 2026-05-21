/*
 * bw_config.h - Breakwater configurations
 */

#pragma once

/*
 * Parameters used in Svalinn evaluation (on xl170)
 *
 * Netbench (cpu+mem+lock)
 * #define SBW_LATENCY_BUDGET			200
 * #define SRPC_CM_SLOPE_THRESH		    0.1
 * #define SRPC_CM_SLOPE_INV		    10
 * #define SRPC_CM_UPDATE_INTERVAL		400
 * #define SRPC_CM_P99_RTT			    200
 *
 * RocksDB
 * #define SBW_LATENCY_BUDGET			200
 * #define SRPC_CM_SLOPE_THRESH		    0.1
 * #define SRPC_CM_SLOPE_INV		    10
 * #define SRPC_CM_UPDATE_INTERVAL		200
 * #define SRPC_CM_P99_RTT			    100
 *
 * Dataframe
 * #define SBW_LATENCY_BUDGET			2200
 * #define SRPC_CM_SLOPE_THRESH		    0.1
 * #define SRPC_CM_SLOPE_INV		    10
 * #define SRPC_CM_UPDATE_INTERVAL		1000
 * #define SRPC_CM_P99_RTT			    500
 *
 * Memcached
 * #define SBW_LATENCY_BUDGET			440
 * #define SRPC_CM_SLOPE_THRESH		    0.1
 * #define SRPC_CM_SLOPE_INV		    10
 * #define SRPC_CM_UPDATE_INTERVAL		200
 * #define SRPC_CM_P99_RTT			    100
 *
 */


/* delay threshold for AQM */
#define SBW_LATENCY_BUDGET			2200

#define SRPC_CM_SLOPE_THRESH		0.1
#define SRPC_CM_SLOPE_INV		10

#define SRPC_CM_UPDATE_INTERVAL		200
#define SRPC_CM_P99_RTT			100

/* round trip time in us */
#define SBW_RTT_US			10
#define SBW_AI				0.001

/* the maximum supported window size */
#define SBW_MAX_WINDOW_EXP		6
#define SBW_MAX_WINDOW			64

#define SBW_MIN_WINDOW			0

#define CBW_MAX_CLIENT_DELAY_US		10
