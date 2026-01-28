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

void MemSemaphore_Post(MemSemaphoreHandle* msem) {
    if (!msem || !msem->obj) {
        return;
    }

    msem->obj->Post();
}

} // extern "C"
