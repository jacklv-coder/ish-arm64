#ifndef ASBESTOS_GUEST_ARM64_SYSREGS_H
#define ASBESTOS_GUEST_ARM64_SYSREGS_H

// Cache Type Register exposed to the guest. DIC (bit 29) and IDC (bit 28)
// intentionally remain clear, so architecturally conforming self-modifying
// code must execute both data- and instruction-cache maintenance.
#define ARM64_CTR_EL0_VALUE 0x84448004
#define ARM64_CTR_EL0_DIC (1U << 29)
#define ARM64_CTR_EL0_IDC (1U << 28)

#endif
