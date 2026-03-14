#pragma once

extern "C" {
#include <base/stddef.h>
#include <runtime/runtime.h>
#include <runtime/thread.h>
}

#include "m_semaphore.hpp"

namespace rpc {

// Memory semaphore wrapper for RPC-based applications.
class RpcMemSemaphore {
private:
    MemSemaphore *msem;

    RpcMemSemaphore(MemSemaphore *msem_);
    ~RpcMemSemaphore();

public:
    static RpcMemSemaphore *GetInstance();
    bool TryWait();
    void Wait();
    void Post();
    int GetCapacity();
    bool IsCongested();
    bool WaitIfUncongested();
};

} // namespace rpc
