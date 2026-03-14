#ifndef __MEM_PMC_HPP__
#define __MEM_PMC_HPP__

#include <cstdint>
#include <memory>


// Base class to be implemented for every CPU architecture.
class MemPmcImpl {
public:
    virtual ~MemPmcImpl() = default;
    virtual uint64_t GetActiveMemChan() = 0;
    virtual uint64_t GetMemChanAccesses(int chan) = 0;
    virtual uint64_t GetMemAccesses() = 0;
};

class MemPmc {
private:
    std::unique_ptr<MemPmcImpl> m_impl;

public:
    MemPmc();
    ~MemPmc();
    void     Init();
    uint64_t GetActiveMemChan();
    uint64_t GetMemChanAccesses(int chan);
    uint64_t GetMemAccesses();
};

#endif  // __MEM_PMC_HPP__
