#include <iostream>
#include "m_semaphore.hpp"
#include "m_semaphore_simple_impl.hpp"
#include "m_semaphore_opt_impl.hpp"


MemSemaphore::MemSemaphore() {

    // Based on the requested implementation, create the required object.
#ifndef M_SEM_OPT
    std::cout << "Using Simple Memory Semaphore Implementation" << std::endl;
    m_impl = std::make_unique<MemSemaphoreSimpleImpl>();
#else
    std::cout << "Using Optimized Memory Semaphore Implementation" << std::endl;
    m_impl = std::make_unique<MemSemaphoreOptImpl>();
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


void MemSemaphore::Post() {
    m_impl->Post();
}

int MemSemaphore::GetCapacity() {
    return m_impl->GetCapacity();
}
