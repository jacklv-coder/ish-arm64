#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asbestos/asbestos.h"
#include "emu/cpu.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"
#include "util/list.h"

struct fake_mmu {
    struct mmu mmu;
    addr_t base;
    unsigned char bytes[4 * PAGE_SIZE];
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

static uint32_t branch_immediate(addr_t source, addr_t target) {
    int64_t displacement = (int64_t) target - (int64_t) source;
    assert((displacement & 3) == 0);
    int64_t words = displacement / 4;
    assert(words >= -(INT64_C(1) << 25) && words < (INT64_C(1) << 25));
    return UINT32_C(0x14000000) |
            ((uint32_t) words & UINT32_C(0x03ffffff));
}

static uint32_t branch_link_immediate(addr_t source, addr_t target) {
    return branch_immediate(source, target) | UINT32_C(0x80000000);
}

static void put_insns(struct fake_mmu *fake, addr_t addr,
        const uint32_t *insns, size_t count) {
    assert(addr >= fake->base);
    size_t offset = (size_t) (addr - fake->base);
    assert(offset + count * sizeof(*insns) <= sizeof(fake->bytes));
    memcpy(fake->bytes + offset, insns, count * sizeof(*insns));
}

static struct fiber_block *find_block(struct asbestos *asbestos, addr_t addr) {
    struct list *bucket = &asbestos->hash[addr % asbestos->hash_size];
    if (list_null(bucket))
        return NULL;
    struct fiber_block *block;
    list_for_each_entry(bucket, block, chain) {
        if (block->addr == addr)
            return block;
    }
    return NULL;
}

static void assert_direct_chain(struct fiber_block *source, unsigned index,
        const struct fiber_block *target) {
    assert(source != NULL && target != NULL && index <= 1);
    assert(source->jump_ip[index] != NULL);
    assert(*source->jump_ip[index] == (unsigned long) target->code);
    assert(*source->jump_ip[index] != source->old_jump_ip[index]);
}

static uint64_t run_guest_observing_cycles(struct fake_mmu *fake,
        struct tlb *tlb, addr_t pc, uint64_t x1, uint64_t x2,
        unsigned *cycles) {
    struct cpu_state cpu = {
        .mmu = &fake->mmu,
        .pc = pc,
        .x1 = x1,
        .x2 = x2,
    };
    assert(cpu_run_to_interrupt(&cpu, tlb) == INT_BREAKPOINT);
    if (cycles != NULL)
        *cycles = cpu.cycle;
    return cpu.x0;
}

static uint64_t run_guest(struct fake_mmu *fake, struct tlb *tlb,
        addr_t pc, uint64_t x1, uint64_t x2) {
    return run_guest_observing_cycles(fake, tlb, pc, x1, x2, NULL);
}

static unsigned invalidate_generation(const struct asbestos *asbestos) {
    return atomic_load_explicit(&asbestos->invalidate_gen,
            memory_order_acquire);
}

static void test_direct_branch_dirty_boundary(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x280000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t source = fake.base;
    const addr_t writer = fake.base + 0x100;
    const addr_t target = fake.base + PAGE_SIZE;
    const uint32_t mov_x0_1 = UINT32_C(0xd2800020);
    const uint32_t mov_x0_2 = UINT32_C(0xd2800040);
    const uint32_t brk = UINT32_C(0xd4200000);
    const uint32_t source_code[] = {branch_immediate(source, target)};
    const uint32_t target_code[] = {mov_x0_1, brk};
    const uint32_t writer_code[] = {
        UINT32_C(0xb9000022), // STR W2, [X1]
        branch_immediate(writer + sizeof(uint32_t), source),
    };
    put_insns(&fake, source, source_code,
            sizeof(source_code) / sizeof(source_code[0]));
    put_insns(&fake, target, target_code,
            sizeof(target_code) / sizeof(target_code[0]));
    put_insns(&fake, writer, writer_code,
            sizeof(writer_code) / sizeof(writer_code[0]));

    assert(run_guest(&fake, tlb, writer, target, mov_x0_1) == 1);
    struct fiber_block *writer_block = find_block(asbestos, writer);
    struct fiber_block *source_block = find_block(asbestos, source);
    struct fiber_block *target_block = find_block(asbestos, target);
    assert_direct_chain(writer_block, 0, source_block);
    assert_direct_chain(source_block, 0, target_block);

    unsigned before = invalidate_generation(asbestos);
    assert(run_guest(&fake, tlb, writer, target, mov_x0_2) == 2);
    assert(invalidate_generation(asbestos) == before + 1);
    assert(!tlb_has_runtime_dirty_pages(tlb));

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void test_return_cache_dirty_boundary(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x380000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t caller = fake.base + PAGE_SIZE - sizeof(uint32_t);
    const addr_t continuation = caller + sizeof(uint32_t);
    const addr_t callee = fake.base + 0x200;
    const uint32_t mov_x0_1 = UINT32_C(0xd2800020);
    const uint32_t mov_x0_2 = UINT32_C(0xd2800040);
    const uint32_t brk = UINT32_C(0xd4200000);
    const uint32_t caller_code[] = {
        branch_link_immediate(caller, callee),
    };
    const uint32_t callee_code[] = {
        UINT32_C(0xb9000022), // STR W2, [X1]
        UINT32_C(0xd65f03c0), // RET
    };
    const uint32_t continuation_code[] = {mov_x0_1, brk};
    put_insns(&fake, caller, caller_code,
            sizeof(caller_code) / sizeof(caller_code[0]));
    put_insns(&fake, callee, callee_code,
            sizeof(callee_code) / sizeof(callee_code[0]));
    put_insns(&fake, continuation, continuation_code,
            sizeof(continuation_code) / sizeof(continuation_code[0]));

    assert(run_guest(&fake, tlb, caller, continuation, mov_x0_1) == 1);
    struct fiber_block *caller_block = find_block(asbestos, caller);
    struct fiber_block *callee_block = find_block(asbestos, callee);
    struct fiber_block *continuation_block = find_block(asbestos, continuation);
    assert_direct_chain(caller_block, 1, callee_block);
    assert_direct_chain(caller_block, 0, continuation_block);

    unsigned before = invalidate_generation(asbestos);
    assert(run_guest(&fake, tlb, caller, continuation, mov_x0_2) == 2);
    assert(invalidate_generation(asbestos) == before + 1);
    assert(!tlb_has_runtime_dirty_pages(tlb));

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void test_data_only_store_keeps_direct_chains(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT64_C(0x480000),
    };
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;

    const addr_t writer = fake.base;
    const addr_t middle = fake.base + PAGE_SIZE;
    const addr_t target = fake.base + 2 * PAGE_SIZE;
    const addr_t data = fake.base + 3 * PAGE_SIZE;
    const uint32_t writer_code[] = {
        UINT32_C(0xb9000022), // STR W2, [X1]
        branch_immediate(writer + sizeof(uint32_t), middle),
    };
    const uint32_t middle_code[] = {branch_immediate(middle, target)};
    const uint32_t target_code[] = {
        UINT32_C(0xd2800020), // MOV X0, #1
        UINT32_C(0xd4200000), // BRK
    };
    put_insns(&fake, writer, writer_code,
            sizeof(writer_code) / sizeof(writer_code[0]));
    put_insns(&fake, middle, middle_code,
            sizeof(middle_code) / sizeof(middle_code[0]));
    put_insns(&fake, target, target_code,
            sizeof(target_code) / sizeof(target_code[0]));

    assert(run_guest(&fake, tlb, writer, data, UINT64_C(0x12345678)) == 1);
    // A second warm-up lets the dispatcher publish and patch both successive
    // direct targets before measuring an entirely chained traversal.
    assert(run_guest(&fake, tlb, writer, data, UINT64_C(0x23456789)) == 1);
    struct fiber_block *writer_block = find_block(asbestos, writer);
    struct fiber_block *middle_block = find_block(asbestos, middle);
    struct fiber_block *target_block = find_block(asbestos, target);
    assert_direct_chain(writer_block, 0, middle_block);
    assert_direct_chain(middle_block, 0, target_block);

    unsigned before = invalidate_generation(asbestos);
    unsigned cycles = 0;
    assert(run_guest_observing_cycles(&fake, tlb, writer, data,
            UINT64_C(0x87654321), &cycles) == 1);
    // Both writer -> middle and middle -> target remain in the JIT. The old
    // unconditional dirty-page boundary exited before the first chain and
    // observed only one chained transition here.
    assert(cycles == 2);
    assert(invalidate_generation(asbestos) == before);
    assert(!tlb_has_runtime_dirty_pages(tlb));

    tlb_free(tlb);
    asbestos_free(asbestos);
}

int main(void) {
    test_direct_branch_dirty_boundary();
    test_return_cache_dirty_boundary();
    test_data_only_store_keeps_direct_chains();
    return 0;
}
