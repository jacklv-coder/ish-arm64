#ifndef ASBESTOS_H
#define ASBESTOS_H
#include "misc.h"
#include "emu/mmu.h"
#include "util/list.h"
#include "util/sync.h"

#define FIBER_INITIAL_HASH_SIZE (1 << 10)
#define FIBER_CACHE_SIZE (1 << 12)  // 4096 entries
#define FIBER_PAGE_HASH_SIZE (1 << 10)

struct asbestos {
    // there is one asbestos per address space
    struct mmu *mmu;
    size_t mem_used;
    size_t num_blocks;

    struct list *hash;
    size_t hash_size;

    // list of fiber_blocks that should be freed soon (at the next RCU grace
    // period, if we had such a thing)
    struct list jetsam;

    // A way to look up blocks in a page
    struct {
        struct list blocks[2];
    } *page_hash;

    // page_hash occupancy is summarized atomically so dirty drains can avoid
    // the global mutation lock for buckets that provably contain no code.
    // Counts are protected by lock; occupancy publication uses release/acquire.
    unsigned page_hash_counts[FIBER_PAGE_HASH_SIZE];
    _Atomic uint64_t page_hash_occupied[FIBER_PAGE_HASH_SIZE / 64];

    // Incremented on every block invalidation; used to invalidate persistent
    // per-thread block caches (which may hold pointers to jetsam'd blocks)
    _Atomic unsigned invalidate_gen;

    // Number of threads currently inside cpu_run_to_interrupt.
    // When 1, we can skip jetsam_lock (no other thread to synchronize with).
    unsigned active_threads;

    // === RCU-like Optimization ===
    // Atomic counter: number of threads currently executing JIT code.
    // Jetsam cleanup waits until this reaches 0 before freeing blocks.
    // This avoids read lock overhead on every JIT enter/exit.
    _Atomic unsigned jit_active_threads;

    // Generation number for jetsam cleanup. Incremented when jetsam list
    // becomes non-empty. Threads check this to know when cleanup is needed.
    _Atomic unsigned jetsam_gen;

    // Lock order is dirty_coherence_lock -> lock. Compilation holds this for
    // write from before reading guest bytes through insertion. Dirty drains
    // hold it for read until their set is either proven code-free or consumed.
    wrlock_t dirty_coherence_lock;
    lock_t lock;
    wrlock_t jetsam_lock;
};

// this is roughly the average number of instructions in a basic block according to anonymous sources
// times 4, roughly the average number of gadgets/parameters in an instruction, according to anonymous sources
#define FIBER_BLOCK_INITIAL_CAPACITY 16

struct fiber_block {
    addr_t addr;
    addr_t end_addr;
    size_t used;

    // pointers to the ip values in the last gadget
    unsigned long *jump_ip[2];
    // original values of *jump_ip[]
    unsigned long old_jump_ip[2];
    // blocks that jump to this block
    struct list jumps_from[2];

    // hashtable bucket links
    struct list chain;
    // list of blocks in a page
    struct list page[2];
    // links for jumps_from
    struct list jumps_from_links[2];
    // links for free list
    struct list jetsam;
    bool is_jetsam;

    unsigned long code[];
};

// High-bit tracing: detect emulation bugs that leave bits 32+ dirty in guest regs
extern volatile bool g_trace_highbits;

// Create a new asbestos
struct asbestos *asbestos_new(struct mmu *mmu);
void asbestos_free(struct asbestos *asbestos);

struct tlb;

// Invalidate all fiber blocks in pages start (inclusive) to end (exclusive).
// Locks the asbestos. Should only be called by memory.c in conjunction with
// mem_changed.
void asbestos_invalidate_range(struct asbestos *asbestos, page_t start, page_t end);
void asbestos_invalidate_page(struct asbestos *asbestos, page_t page);
void asbestos_invalidate_all(struct asbestos *asbestos);
// Consume every page-hash bucket marked by guest writes since the previous
// translated-block boundary. Returns true when the dirty set was non-empty.
bool asbestos_invalidate_dirty_pages(struct asbestos *asbestos, struct tlb *tlb);

#endif
