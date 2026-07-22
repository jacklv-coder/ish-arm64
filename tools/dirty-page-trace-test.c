#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asbestos/asbestos.h"
#include "emu/cpu.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"

struct fake_mmu {
    struct mmu mmu;
    addr_t base;
    addr_t first_data_page;
    addr_t second_data_page;
    unsigned char code_bytes[PAGE_SIZE];
    unsigned char first_data_bytes[PAGE_SIZE];
    unsigned char second_data_bytes[PAGE_SIZE];
};

static void *fake_translate(struct mmu *mmu, addr_t addr, int type) {
    (void) type;
    struct fake_mmu *fake = (struct fake_mmu *) mmu;
    addr_t page = TLB_PAGE(addr);
    if (page == fake->base)
        return fake->code_bytes;
    if (page == fake->first_data_page)
        return fake->first_data_bytes;
    if (page == fake->second_data_page)
        return fake->second_data_bytes;
    return NULL;
}

static void *fake_translate_write_nofault(struct mmu *mmu, addr_t addr) {
    return fake_translate(mmu, addr, MEM_WRITE);
}

static void assert_trace_pages(const struct tlb *tlb,
        const page_t *expected, size_t expected_count) {
    page_t cursor = 0;
    addr_t page_addr;
    size_t index = 0;
    while (tlb_dirty_trace_next(tlb, &cursor, &page_addr)) {
        assert(index < expected_count);
        assert(PAGE(page_addr) == expected[index]);
        index++;
    }
    assert(index == expected_count);
}

static void test_jit_fast_store_trace(void) {
    static struct mmu_ops ops = {
        .translate = fake_translate,
        .translate_write_nofault = fake_translate_write_nofault,
    };
    struct fake_mmu fake = {
        .mmu = {.ops = &ops},
        .base = UINT32_C(0x100000),
    };
    fake.first_data_page = fake.base + PAGE_SIZE;
    fake.second_data_page = fake.first_data_page +
            4 * TLB_DIRTY_BUCKET_COUNT * PAGE_SIZE;
    struct asbestos *asbestos = asbestos_new(&fake.mmu);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    fake.mmu.asbestos = asbestos;
    tlb_refresh(tlb, &fake.mmu);

    const addr_t first_addr = fake.first_data_page + 8;
    const addr_t second_addr = fake.second_data_page + 8;
    assert(PAGE(first_addr) % TLB_DIRTY_BUCKET_COUNT ==
            PAGE(second_addr) % TLB_DIRTY_BUCKET_COUNT);
    assert(PAGE(second_addr) >= 4096);
    uint32_t ignored;
    // Prime writable entries through read misses so both guest stores use the
    // assembly write_prep fast path rather than C tlb_handle_miss.
    assert(tlb_read(tlb, first_addr, &ignored, sizeof(ignored)));
    assert(tlb_read(tlb, second_addr, &ignored, sizeof(ignored)));
    assert(tlb_dirty_trace_attach(tlb));

    // MOV [moffs32], EAX; MOV [moffs32], EAX; INT3
    unsigned char *code = fake.code_bytes;
    size_t offset = 0;
    code[offset++] = 0xa3;
    memcpy(code + offset, &first_addr, sizeof(first_addr));
    offset += sizeof(first_addr);
    code[offset++] = 0xa3;
    memcpy(code + offset, &second_addr, sizeof(second_addr));
    offset += sizeof(second_addr);
    code[offset++] = 0xcc;

    const uint32_t value = UINT32_C(0x12345678);
    struct cpu_state cpu = {
        .mmu = &fake.mmu,
        .eip = fake.base,
        .eax = value,
    };
    assert(cpu_run_to_interrupt(&cpu, tlb) == INT_BREAKPOINT);
    assert(memcmp(fake.first_data_bytes + PGOFFSET(first_addr), &value,
            sizeof(value)) == 0);
    assert(memcmp(fake.second_data_bytes + PGOFFSET(second_addr), &value,
            sizeof(value)) == 0);
    assert(!tlb_has_runtime_dirty_pages(tlb));
    const page_t expected[] = {PAGE(first_addr), PAGE(second_addr)};
    assert_trace_pages(tlb, expected, sizeof(expected) / sizeof(expected[0]));

    tlb_free(tlb);
    asbestos_free(asbestos);
}

static void test_trace_generation_boundary(void) {
    struct mmu first = {0};
    struct mmu second = {0};
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(tlb != NULL);

    tlb_refresh(tlb, &first);
    assert(tlb_dirty_trace_attach(tlb));
    tlb_mark_dirty_page(tlb, UINT32_C(0x12345000));
    assert(tlb_dirty_trace_has_pages(tlb));

    // Refreshing an unchanged generation is a no-op: a tracer may compare the
    // accumulated writes after cpu_run_to_interrupt returns.
    tlb_refresh(tlb, &first);
    assert(tlb_dirty_trace_has_pages(tlb));

    // Mapping changes invalidate every exact page identity in the snapshot.
    first.changes++;
    tlb_refresh(tlb, &first);
    assert(!tlb_dirty_trace_has_pages(tlb));

    tlb_mark_dirty_page(tlb, UINT32_C(0x23456000));
    assert(tlb_dirty_trace_has_pages(tlb));
    tlb_flush(tlb);
    assert(tlb_dirty_trace_has_pages(tlb));

    // Slow paths can observe an epoch mismatch and call tlb_flush directly;
    // that must not hide stale exact pages from the following refresh.
    first.changes++;
    tlb_flush(tlb);
    assert(!tlb_dirty_trace_has_pages(tlb));
    assert(tlb_has_runtime_dirty_pages(tlb));
    tlb_clear_runtime_dirty_pages(tlb);

    tlb_mark_dirty_page(tlb, UINT32_C(0x34567000));
    assert(tlb_dirty_trace_has_pages(tlb));
    tlb_refresh(tlb, &second);
    assert(!tlb_dirty_trace_has_pages(tlb));

    tlb_free(tlb);
}

int main(void) {
    struct asbestos *asbestos = asbestos_new(NULL);
    struct tlb *tlb = calloc(1, sizeof(*tlb));
    assert(asbestos != NULL && tlb != NULL);
    assert(tlb_dirty_trace_attach(tlb));
    tlb_clear_runtime_dirty_pages(tlb);

    const page_t first = 3;
    const page_t second = 70;
    const page_t collision = first + TLB_DIRTY_BUCKET_COUNT;
    const page_t last = MEM_PAGES - 1;
    const page_t expected[] = {first, second, collision, last};

    // The runtime set is hashed for conservative JIT invalidation, but the
    // opt-in diagnostic trace must retain both exact colliding page numbers.
    tlb_mark_dirty_page(tlb, (addr_t) first << PAGE_BITS);
    tlb_mark_dirty_page(tlb, (addr_t) second << PAGE_BITS);
    tlb_mark_dirty_page(tlb, (addr_t) collision << PAGE_BITS);
    tlb_mark_dirty_page(tlb, (addr_t) last << PAGE_BITS);
    assert(tlb_has_runtime_dirty_pages(tlb));
    assert(asbestos_invalidate_dirty_pages(asbestos, tlb));
    assert(!tlb_has_runtime_dirty_pages(tlb));
    assert(tlb_dirty_trace_has_pages(tlb));
    assert_trace_pages(tlb, expected, sizeof(expected) / sizeof(expected[0]));

    // Iteration is non-consuming and a failed comparison keeps the complete
    // set. Only a successful comparison acknowledgement removes it.
    assert_trace_pages(tlb, expected, sizeof(expected) / sizeof(expected[0]));
    tlb_dirty_trace_finish(tlb, false);
    assert_trace_pages(tlb, expected, sizeof(expected) / sizeof(expected[0]));
    tlb_dirty_trace_finish(tlb, true);
    assert(!tlb_dirty_trace_has_pages(tlb));

    tlb_free(tlb);
    asbestos_free(asbestos);

    test_jit_fast_store_trace();
    test_trace_generation_boundary();
    puts("exact dirty-page trace regressions passed");
    return 0;
}
