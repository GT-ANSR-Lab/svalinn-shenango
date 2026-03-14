extern "C" {
#include <bw2_config.h>
#include <pcc_config.h>
#include <breakwater/rpc.h>
}

#include <breakwater/msem++.h>

namespace rpc {

RpcMemSemaphore::RpcMemSemaphore(MemSemaphore *msem_) {
    msem = msem_;
}

RpcMemSemaphore::~RpcMemSemaphore() {
}


RpcMemSemaphore *RpcMemSemaphore::GetInstance() {
    static RpcMemSemaphore inst(MemSemaphore::GetInstance());
    return &inst;
}

bool RpcMemSemaphore::TryWait() {
    return msem->TryWait();
}

void RpcMemSemaphore::Wait() {
    msem->Wait();
}

void RpcMemSemaphore::Post() {
    msem->Post();
}

int RpcMemSemaphore::GetCapacity() {
    return msem->GetCapacity();
}

bool RpcMemSemaphore::IsCongested() {

    if (!get_rpc_ctx()) {
        return false;
    }

    if (srpc_ops == &sbw2_ops) {
        /* If protego */
        return ((get_acc_qdel() + msem->QueueDelayTsc()) \
                > (SBW_LATENCY_BUDGET * cycles_per_us));
        // return (msem->QueueLength() >= 4);
    } else if (srpc_ops == &spcc_ops) {
        /* If pcc */
        return ((get_acc_qdel() + msem->QueueDelayTsc()) \
                > (SPCC_QDELAY_BUDGET * cycles_per_us));
        // return (msem->QueueLength() >= 4);
    }

    /* Other algorithms do no define queueing budget */
    return false;
}

bool RpcMemSemaphore::WaitIfUncongested() {
    if (IsCongested()) {
        return false;
    }

    Wait();
    return true;
}

} // namespace rpc
