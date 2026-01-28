#ifndef __M_SEMAPHORE_C_H__
#define __M_SEMAPHORE_C_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

// Opaque pointer type representing MemSemaphore
typedef struct MemSemaphoreHandle MemSemaphoreHandle;

// Get the singleton instance
MemSemaphoreHandle* MemSemaphore_GetInstance(void);

// Non-blocking acquire
bool MemSemaphore_TryWait(MemSemaphoreHandle* msem);

// Release
void MemSemaphore_Post(MemSemaphoreHandle* msem);

#ifdef __cplusplus
}
#endif

#endif // __M_SEMAPHORE_C_H__
