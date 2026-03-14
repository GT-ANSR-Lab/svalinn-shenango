#include "c/m_semaphore_c.h"
#include "m_semaphore.hpp"

extern "C" {
// Opaque pointer is just the C++ object
struct MemSemaphoreHandle {
    MemSemaphore* obj;
};

MemSemaphoreHandle* MemSemaphore_GetInstance(void) {
    static MemSemaphoreHandle handle{ MemSemaphore::GetInstance() };
    return &handle;
}

bool MemSemaphore_TryWait(MemSemaphoreHandle* msem) {
    if (!msem || !msem->obj) {
        return false;
    }

    return msem->obj->TryWait();
}

void MemSemaphore_Wait(MemSemaphoreHandle* msem) {
    if (!msem || !msem->obj) {
        return;
    }

    msem->obj->Wait();
}

uint64_t MemSemaphore_QueueDelayTsc(MemSemaphoreHandle* msem) {
    if (!msem || !msem->obj) {
        return 0;
    }

    return msem->obj->QueueDelayTsc();
}

uint64_t MemSemaphore_QueueLength(MemSemaphoreHandle* msem) {
    if (!msem || !msem->obj) {
        return 0;
    }

    return msem->obj->QueueLength();
}

void MemSemaphore_Post(MemSemaphoreHandle* msem) {
    if (!msem || !msem->obj) {
        return;
    }

    msem->obj->Post();
}

} // extern "C"
