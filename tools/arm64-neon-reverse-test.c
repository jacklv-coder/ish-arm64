#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asbestos/asbestos.h"
#include "emu/cpu.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"

struct fake_mmu {
    struct mmu mmu;
    addr_t base;
    unsigned char bytes[PAGE_SIZE];
};

static void *fake_translate(struct mmu *mmu, addr_t addr, int type) {
    (void) type;
    struct fake_mmu *fake = (struct fake_mmu *) mmu;
    if (addr < fake->base || addr >= fake->base + sizeof(fake->bytes))
        return NULL;
    return fake->bytes + (addr - fake->base);
}

static void *fake_translate_write_nofault(struct mmu *mmu, addr_t addr) {
    return fake_translate(mmu, addr, MEM_WRITE);
}

static void test_rev16_vectors(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x280000),
    };
    const uint32_t code[] = {
        UINT32_C(0x0e201841), // REV16 V1.8B, V2.8B
        UINT32_C(0x4e201883), // REV16 V3.16B, V4.16B
        UINT32_C(0xd4200000), // BRK #0
    };
    memcpy(fake.bytes, code, sizeof(code));

    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    struct cpu_state cpu = {
        .mmu = &fake.mmu,
        .pc = fake.base,
    };
    memset(cpu.fp[1].b, 0xff, sizeof(cpu.fp[1].b));
    memset(cpu.fp[3].b, 0xff, sizeof(cpu.fp[3].b));
    for (size_t i = 0; i < sizeof(cpu.fp[2].b); i++) {
        cpu.fp[2].b[i] = (uint8_t) i;
        cpu.fp[4].b[i] = (uint8_t) (0x80 + i);
    }

    assert(cpu_run_to_interrupt(&cpu, tlb) == INT_BREAKPOINT);
    for (size_t i = 0; i < 8; i++)
        assert(cpu.fp[1].b[i] == (uint8_t) (i ^ 1));
    for (size_t i = 8; i < sizeof(cpu.fp[1].b); i++)
        assert(cpu.fp[1].b[i] == 0);
    for (size_t i = 0; i < sizeof(cpu.fp[3].b); i++)
        assert(cpu.fp[3].b[i] == (uint8_t) (0x80 + (i ^ 1)));

    tlb_free(tlb);
    asbestos_free(asbestos);
}

int main(void) {
    test_rev16_vectors();
    return 0;
}
