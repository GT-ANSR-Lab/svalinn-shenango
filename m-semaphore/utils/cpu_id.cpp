extern "C" {
#include "asm/ops.h"
}

#include <cstring>

#include "cpu_id.hpp"


CpuIdInfo::CpuIdInfo() {

    struct cpuid_info regs;
    char vendor[13];

    // Get the CPU vendor
    cpuid(0, &regs);
    std::memcpy(vendor + 0, &regs.ebx, 4);
    std::memcpy(vendor + 4, &regs.edx, 4);
    std::memcpy(vendor + 8, &regs.ecx, 4);
    vendor[12] = '\0';
    m_vendor = vendor;

    // Get the family and model of the CPU
    cpuid(1, &regs);
    uint32_t base_family = (regs.eax >> 8) & 0xF;
    uint32_t base_model  = (regs.eax >> 4) & 0xF;
    uint32_t ext_family  = (regs.eax >> 20) & 0xFF;
    uint32_t ext_model   = (regs.eax >> 16) & 0xF;
    m_family = base_family;
    if (base_family == 0xF) {
        m_family += ext_family;
    }
    m_model = base_model;
    if (base_family == 0x6 || base_family == 0xF) {
        m_model += (ext_model << 4);
    }
}

bool CpuIdInfo::IsIntel() {

    return m_vendor == "GenuineIntel";
}

bool CpuIdInfo::IsAmd() {

    return m_vendor == "AuthenticAMD";
}

bool CpuIdInfo::IsAmdZen2() {
    return m_vendor == "AuthenticAMD" &&
        m_family == 0x17 &&
        m_model >= 0x30 && m_model <= 0x3F;
}

bool CpuIdInfo::IsAmdZen3() {
    return m_vendor == "AuthenticAMD" &&
        m_family == 0x19 &&
        m_model >= 0x00 && m_model <= 0x0F;
}

bool CpuIdInfo::IsAmdZen4() {
    return m_vendor == "AuthenticAMD" &&
        m_family == 0x19 &&
        ((m_model >= 0x10 && m_model <= 0x1F) ||
         (m_model >= 0xA0 && m_model <= 0xAF));
}
