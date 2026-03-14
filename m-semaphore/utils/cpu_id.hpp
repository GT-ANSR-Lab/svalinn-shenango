#ifndef __CPU_ID_HPP__
#define __CPU_ID_HPP__

#include <string>
#include <cstdint>

// Information required to identify the vendor and model of CPU
class CpuIdInfo {
private:
    std::string m_vendor;
    uint32_t m_family;
    uint32_t m_model;

public:
    CpuIdInfo();

    bool IsIntel();
    bool IsAmd();
    bool IsAmdZen2();
    bool IsAmdZen3();
    bool IsAmdZen4();
};

#endif  // __CPU_ID_HPP__
