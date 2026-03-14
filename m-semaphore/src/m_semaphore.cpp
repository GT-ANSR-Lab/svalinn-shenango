#include <iostream>
#include "m_semaphore.hpp"
#include "m_semaphore_mab_eg_impl.hpp"
#include "m_semaphore_mab_ts_impl.hpp"


MemSemaphore::MemSemaphore() {

    // Based on the requested implementation, create the required object.
#if defined(M_SEM_MAB_EPSILON_GREEDY)
    std::cout << "Using MAB Epsilon Greedy Memory Semaphore Implementation" << std::endl;
    m_impl = std::make_unique<MemSemaphoreMabEgImpl>();
#elif defined(M_SEM_MAB_THOMPSON_SAMPLING)
    std::cout << "Using MAB Thompson Sampling Memory Semaphore Implementation" << std::endl;
    m_impl = std::make_unique<MemSemaphoreMabTsImpl>();
#else
    // By default use epsilon greedy implementation.
    std::cout << "Using MAB Epsilon Greedy Memory Semaphore Implementation" << std::endl;
    m_impl = std::make_unique<MemSemaphoreMabEgImpl>();
#endif
}


MemSemaphore::~MemSemaphore() {
    // Destructor for impl automatically called because of unique pointer.
}


MemSemaphore *MemSemaphore::GetInstance() {

    // The creation of this static instance is thread safe in C++11 onwards.
    // The object will destructed on program exit.
    static MemSemaphore inst;
    return &inst;
}


bool MemSemaphore::TryWait() {
    return m_impl->TryWait();
}

void MemSemaphore::Wait() {
    m_impl->Wait();
}

uint64_t MemSemaphore::QueueDelayTsc() {
    return m_impl->QueueDelayTsc();
}

uint64_t MemSemaphore::QueueLength() {
    return m_impl->QueueLength();
}

void MemSemaphore::Post() {
    m_impl->Post();
}

int MemSemaphore::GetCapacity() {
    return m_impl->GetCapacity();
}
