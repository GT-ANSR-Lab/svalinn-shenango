/*
 * msem.h - Memory Semaphore abstraction for overload control
 */

#include <base/stddef.h>
#include <runtime/runtime.h>
#include <breakwater/rpc.h>
#include <breakwater/msem.h>

#include "bw2_config.h"
#include "pcc_config.h"

inline bool MemSemaphore_IsCongested(MemSemaphoreHandle* msem) {

    if (!get_rpc_ctx()) {
        return false;
    }

    if (srpc_ops == &sbw2_ops) {
        /* If protego */
        return ((get_acc_qdel() + MemSemaphore_QueueDelayTsc(msem)) \
                > (SBW_LATENCY_BUDGET * cycles_per_us));
    } else if (srpc_ops == &spcc_ops) {
        /* If pcc */
        return ((get_acc_qdel() + MemSemaphore_QueueDelayTsc(msem)) \
                > (SPCC_QDELAY_BUDGET * cycles_per_us));
    }

    /* Other algorithms do no define queueing budget */
    return false;
}


inline bool MemSemaphore_WaitIfUncongested(MemSemaphoreHandle* msem) {

    if (MemSemaphore_IsCongested(msem)) {
        return false;
    }

    MemSemaphore_Wait(msem);
    return true;
}
