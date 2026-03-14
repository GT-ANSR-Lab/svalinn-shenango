/*
 * msem.h - Memory Semaphore abstraction for overload control
 */

#pragma once

#include <c/m_semaphore_c.h>

bool MemSemaphore_IsCongested(MemSemaphoreHandle* msem);
bool MemSemaphore_WaitIfUncongested(MemSemaphoreHandle* msem);
