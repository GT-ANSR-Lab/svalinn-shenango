#ifndef __M_SEMAPHORE_IMPL_HPP__
#define __M_SEMAPHORE_IMPL_HPP__

// Interface for memory semaphore implementation.
class MemSemaphoreImpl {
public:
    virtual ~MemSemaphoreImpl() = default;

    virtual bool TryWait() = 0;
    virtual void Post() = 0;
    virtual int GetCapacity() = 0;
};

#endif  // __M_SEMAPHORE_IMPL_HPP__
