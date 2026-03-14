#ifndef __M_SEMAPHORE_HPP__
#define __M_SEMAPHORE_HPP__

#include <cstdint>
#include <memory>
#include "m_semaphore_impl.hpp"


class MemSemaphore {
private:
    // Pointer to the memory semaphore implementation.
    std::unique_ptr<MemSemaphoreImpl> m_impl;

    // We do not allow a user to create an object explicity. Hence, the
    // constructor is made private.
    MemSemaphore();
    ~MemSemaphore();

    // Disallow copy of the object.
    MemSemaphore(const MemSemaphore&) = delete;
    MemSemaphore& operator=(const MemSemaphore&) = delete;

public:
    // Get a reference to the singleton memory semaphore object.
    static MemSemaphore *GetInstance();

    // Non-blocking semaphore acquire. Returns true if the calling thread
    // successfully acquired the semaphore. Otherwise returns false.
    bool TryWait();

    // Blocking semaphore acquire.
    void Wait();

    // Queueing delay of the list of waiters.
    uint64_t QueueDelayTsc();

    // Queueing length of the list of waiters.
    uint64_t QueueLength();

    // Releases the semaphore.
    void Post();

    // Returns the current semaphore capacity.
    int GetCapacity();
};


#endif  // __M_SEMAPHORE_HPP__
