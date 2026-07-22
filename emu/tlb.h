#ifndef TLB_H
#define TLB_H

#include <string.h>
#include <stdint.h>
#include "emu/mmu.h"
#include "debug.h"

struct tlb_entry {
    page_t page;
    page_t page_if_writable;
    uintptr_t data_minus_addr;
#ifdef GUEST_ARM64
    uintptr_t _pad;  // pad to 32 bytes for efficient JIT indexing (lsl #5)
#endif
};
#define TLB_BITS 13  // 8192 entries
#define TLB_SIZE (1 << TLB_BITS)
#define TLB_DIRTY_BUCKET_BITS 10
#define TLB_DIRTY_BUCKET_COUNT (1 << TLB_DIRTY_BUCKET_BITS)
#define TLB_DIRTY_BUCKET_WORDS (TLB_DIRTY_BUCKET_COUNT / 64)
#if defined(GUEST_X86)
#define TLB_DIRTY_TRACE_PAGE_WORDS (MEM_PAGES / 64)
#define TLB_DIRTY_TRACE_SUMMARY_WORDS (TLB_DIRTY_TRACE_PAGE_WORDS / 64)

// Exact dirty-page diagnostics are deliberately separate from the hashed JIT
// invalidation set. ptraceomatic and unicornomatic opt in to this allocation;
// normal execution keeps tlb->dirty_trace NULL and pays no allocation cost.
struct tlb_dirty_trace {
    uint64_t summary_bits[TLB_DIRTY_TRACE_SUMMARY_WORDS];
    uint64_t page_bits[TLB_DIRTY_TRACE_PAGE_WORDS];
};
#endif
struct fiber_block;
struct fiber_frame;

struct tlb {
    struct mmu *mmu;
    // Runtime JIT invalidation state. dirty_page retains the current exact page
    // while dirty_page_buckets preserves pages transitioned away from.
    page_t dirty_page;
    unsigned mem_changes;
    // this is basically one of the return values of tlb_handle_miss, tlb_{read,write}, and __tlb_{read,write}_cross_page
    // yes, this sucks
    addr_t segfault_addr;

    // One bit per asbestos page-hash bucket. On a page transition the prior
    // exact page is ORed here; dirty_page keeps the current page and the
    // dispatcher converts it before consuming the whole set. This defers
    // bitmap work for common single-page store runs. Hash collisions remain
    // conservative at asbestos_invalidate_page() granularity.
    uint64_t dirty_page_buckets[TLB_DIRTY_BUCKET_WORDS];

#if defined(GUEST_X86)
    // Optional exact diagnostic state. Unlike dirty_page_buckets this uses the
    // full 20-bit x86 guest page number, so hash collisions cannot hide a page.
    // It survives runtime invalidation drains until the tracer consumes it,
    // but is cleared when the MMU pointer or mapping generation changes.
    struct tlb_dirty_trace *dirty_trace;
#endif

    struct tlb_entry entries[TLB_SIZE];

    // Persistent block cache across syscalls (avoids re-lookup after every interrupt)
    // Size defined by FIBER_CACHE_SIZE in asbestos.h
    struct fiber_block *block_cache[1 << 12];  // must match FIBER_CACHE_SIZE
    unsigned block_cache_gen; // tracks asbestos->invalidate_gen for invalidation

    // Persistent fiber_frame (avoids malloc/free + ret_cache zeroing per syscall)
    struct fiber_frame *frame;

    // Decoder-only guard. While a compiler holds dirty_coherence_lock it may
    // use already resolved TLB entries but must never enter an MMU slow path
    // that could upgrade mem locks.
    bool compile_nofault_reads;
};

#define TLB_INDEX(addr) ((((addr >> PAGE_BITS) ^ (addr >> (PAGE_BITS + TLB_BITS))) & (TLB_SIZE - 1)))
#ifdef GUEST_ARM64
#define TLB_PAGE(addr) ((addr) & 0xfffffffffffff000ULL)
#else
#define TLB_PAGE(addr) ((addr) & 0xfffff000)
#endif
#define TLB_PAGE_EMPTY 1

forceinline __no_instrument void tlb_mark_dirty_page(struct tlb *tlb, addr_t addr) {
    page_t page = TLB_PAGE(addr);
    page_t previous = tlb->dirty_page;
    tlb->dirty_page = page;
    // Keep the most recent page exact and defer its bucket conversion to the
    // dispatcher. Only a transition away from an earlier pending page needs
    // to preserve that earlier bucket now. Common single-page store runs thus
    // avoid bitmap arithmetic while arbitrary multi-page writes remain lossless.
    if (previous == page)
        return;
    if (previous != TLB_PAGE_EMPTY) {
        unsigned bucket = (unsigned) (PAGE(previous) & (TLB_DIRTY_BUCKET_COUNT - 1));
        tlb->dirty_page_buckets[bucket / 64] |= UINT64_C(1) << (bucket % 64);
    }
#if defined(GUEST_X86)
    if (tlb->dirty_trace != NULL) {
        page_t page_number = PAGE(page);
        size_t word = page_number / 64;
        tlb->dirty_trace->page_bits[word] |= UINT64_C(1) << (page_number % 64);
        tlb->dirty_trace->summary_bits[word / 64] |= UINT64_C(1) << (word % 64);
    }
#endif
}

forceinline __no_instrument bool tlb_has_runtime_dirty_pages(const struct tlb *tlb) {
    return tlb->dirty_page != TLB_PAGE_EMPTY;
}

forceinline __no_instrument void tlb_clear_runtime_dirty_pages(struct tlb *tlb) {
    tlb->dirty_page = TLB_PAGE_EMPTY;
    memset(tlb->dirty_page_buckets, 0, sizeof(tlb->dirty_page_buckets));
}

#if defined(GUEST_X86)
bool tlb_dirty_trace_attach(struct tlb *tlb);
void tlb_dirty_trace_detach(struct tlb *tlb);
void tlb_dirty_trace_clear(struct tlb *tlb);
bool tlb_dirty_trace_has_pages(const struct tlb *tlb);
bool tlb_dirty_trace_next(const struct tlb *tlb, page_t *cursor, addr_t *page_addr);

forceinline void tlb_dirty_trace_finish(struct tlb *tlb,
        bool comparison_succeeded) {
    if (comparison_succeeded)
        tlb_dirty_trace_clear(tlb);
}

forceinline __no_instrument void tlb_dirty_trace_mark_page(
        struct tlb *tlb, addr_t page_addr) {
    if (tlb->dirty_trace == NULL)
        return;
    page_t page_number = PAGE(page_addr);
    size_t word = page_number / 64;
    tlb->dirty_trace->page_bits[word] |= UINT64_C(1) << (page_number % 64);
    tlb->dirty_trace->summary_bits[word / 64] |= UINT64_C(1) << (word % 64);
}
#else
forceinline __no_instrument void tlb_dirty_trace_mark_page(
        struct tlb *tlb, addr_t page_addr) {
    (void) tlb;
    (void) page_addr;
}
#endif

void tlb_refresh(struct tlb *tlb, struct mmu *mmu);
void tlb_free(struct tlb *tlb);
void tlb_flush(struct tlb *tlb);
void *tlb_handle_miss(struct tlb *tlb, addr_t addr, int type);

forceinline __no_instrument void *__tlb_read_ptr(struct tlb *tlb, addr_t addr) {
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page == TLB_PAGE(addr)) {
        void *address = (void *) (entry.data_minus_addr + addr);
        return address;
    }
    if (tlb->compile_nofault_reads) {
        tlb->segfault_addr = addr;
        return NULL;
    }
    return tlb_handle_miss(tlb, addr, MEM_READ);
}
bool __tlb_read_cross_page(struct tlb *tlb, addr_t addr, char *out, unsigned size);
forceinline __no_instrument bool tlb_read(struct tlb *tlb, addr_t addr, void *out, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_read_cross_page(tlb, addr, out, size);
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(out, ptr, size);
    return true;
}

// C-level write watchpoint: detect stores that bypass assembly write_prep
#define ENABLE_C_WRITE_WATCHPOINT 1
extern volatile addr_t g_watch_page_val;
void c_watch_write_hit(addr_t addr, const char *caller);

forceinline __no_instrument void *__tlb_write_ptr(struct tlb *tlb, addr_t addr) {
#ifdef ENABLE_C_WRITE_WATCHPOINT
    if (g_watch_page_val && (addr & ~0xfffULL) == g_watch_page_val) {
        c_watch_write_hit(addr, __func__);
    }
#endif
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page_if_writable == TLB_PAGE(addr)) {
        tlb_mark_dirty_page(tlb, addr);
        void *address = (void *) (entry.data_minus_addr + addr);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_WRITE);
}
bool __tlb_write_cross_page(struct tlb *tlb, addr_t addr, const char *value, unsigned size);
forceinline __no_instrument bool tlb_write(struct tlb *tlb, addr_t addr, const void *value, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_write_cross_page(tlb, addr, value, size);
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(ptr, value, size);
    return true;
}

#endif
