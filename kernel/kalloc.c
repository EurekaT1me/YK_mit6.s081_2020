// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
// defined by kernel.ld.

struct run {
    struct run *next;
};

struct {
    struct spinlock lock;
    struct run *freelist;
} kmem;

/* cow */
#define PHYPAGE_TOTAL (PHYSTOP-KERNBASE)/PGSIZE+1
struct spinlock pageRefLock;
int pageRefCnt[PHYPAGE_TOTAL];

int kgetPageIdx(uint64 pa) {
    return (pa - KERNBASE) / PGSIZE;
}

void
kinit() {
    initlock(&kmem.lock, "kmem");
    initlock(&pageRefLock, "pgref");
    freerange(end, (void *) PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char *) PGROUNDUP((uint64) pa_start);
    for (; p + PGSIZE <= (char *) pa_end; p += PGSIZE)
        kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 减少一个物理页的引用
void
kfree(void *pa) {
    struct run *r;

    if (((uint64) pa % PGSIZE) != 0 || (char *) pa < end || (uint64) pa >= PHYSTOP)
        panic("kfree");

    acquire(&pageRefLock);
    int idx = kgetPageIdx((uint64) pa);
    if (--pageRefCnt[idx] <= 0) {
        // Fill with junk to catch dangling refs.
        memset(pa, 1, PGSIZE);

        r = (struct run *) pa;

        acquire(&kmem.lock);
        r->next = kmem.freelist;
        kmem.freelist = r;
        release(&kmem.lock);
    }
    release(&pageRefLock);

}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void) {
    struct run *r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r)
        kmem.freelist = r->next;
    release(&kmem.lock);

    if (r) {
        memset((char *) r, 5, PGSIZE); // fill with junk
        // 初始化引用
        pageRefCnt[kgetPageIdx((uint64) r)] = 1;
    }

    return (void *) r;
}


// 对物理页增加引用
void kincreRef(void *pa) {
    acquire(&pageRefLock);
    ++pageRefCnt[kgetPageIdx((uint64) pa)];
    release(&pageRefLock);
}

// 将原来uvmcopy中的拷贝动作lazy到此处进行
void *klazyCopy(void *pa) {
    acquire(&pageRefLock);
    int idx = kgetPageIdx((uint64) pa);
    // 如果引用次数小于1，发生panic
    if (pageRefCnt[idx] < 1) {
        release(&pageRefLock);
        panic("klazyCopy: page ref lower than 1");
    }

    // 如果引用次数为1，不需要拷贝
    if (pageRefCnt[idx] == 1) {
        release(&pageRefLock);
        return pa;
    }

    // 如果引用次数大于1，拷贝
    void *new;
    if ((new = kalloc()) == 0) {
        printf("klazyCopy: out-of-memory\n");
        release(&pageRefLock);
        return 0;
    }
    memmove(new, pa, PGSIZE);
    --pageRefCnt[idx];
    release(&pageRefLock);
    return new;
}