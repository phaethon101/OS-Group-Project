#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <buddy_pmm.h>
#include <slub.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
    pmm_manager = &best_fit_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 2411204
/* *
 * page_init - 物理内存探测 + Page 描述符初始化
 *
 * 函数返回时物理内存的布局（均为物理地址）：
 *   [0x80000000, 0x80200000)                       OpenSBI
 *   [0x80200000, PADDR(end))                       内核镜像（.text/.rodata/.data/.bss）
 *   [ROUNDUP(PADDR(end), PGSIZE),
 *    + (npage - nbase) * sizeof(struct Page))      Page 描述符数组（虚拟地址为 pages）
 *   [ROUNDUP(描述符数组末尾, PGSIZE),
 *    ROUNDDOWN(mem_end, PGSIZE))                   交给 pmm_manager 的空闲页
 *
 * 注意区分四种量：虚拟地址 / 物理地址 / 物理页号 / Page 描述符地址，
 * 链接符号 end 是虚拟地址，描述符数组的末尾必须先用 PADDR 转成物理地址，
 * 才能和物理内存末端比较。
 * */
static void page_init(void) {
    // (1) 虚实映射偏移量：VA - PA。kern_entry 里建立的启动页表正是按这个偏移
    //     把内核映射到高半区的，PADDR/KADDR 都依赖该全局变量。
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    // (2) 从设备树取物理内存范围（由 dtb_init() 解析后保存）
    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();

    // 设备树信息无效则终止初始化
    if (mem_base == 0 || mem_size == 0) {
        panic("page_init: invalid physical memory range from DTB!");
    }

    // mem_end 为物理内存末端（不含）
    uint64_t mem_end = mem_base + mem_size;

    cprintf("physical memory map:\n");
    cprintf("  memory: 0x%016lx, [0x%016lx, 0x%016lx].\n",
            mem_size, mem_base, mem_end - 1);

    // (3) npage 为内核可管理的物理页号上界，nbase 为 DRAM 起始页号，
    //     描述符数组需要覆盖页号区间 [nbase, npage) 中的每一个物理页。
    npage = PPN(mem_end);

    // (4) 描述符数组紧接在内核镜像尾部的 end 之后，并按页对齐。
    extern char end[];
    pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);

    size_t npage_desc = npage - nbase;            // 描述符个数
    size_t desc_size = npage_desc * sizeof(struct Page);

    // (5) 先把所有描述符清零并置为 reserved，防止尚未完成布局的页面被提前分配
    memset(pages, 0, desc_size);
    for (size_t i = 0; i < npage_desc; i++) {
        SetPageReserved(pages + i);
    }

    // (6) 描述符数组本身占用 [PADDR(pages), PADDR(pages) + desc_size)，
    //     向上按页对齐后才是空闲区的起点；物理内存末端向下按页对齐，
    //     尾部不足一整页的部分不能交给分配器。
    uintptr_t pages_end_pa = PADDR(pages) + desc_size;
    uintptr_t free_begin_pa = ROUNDUP(pages_end_pa, PGSIZE);
    uintptr_t free_end_pa = ROUNDDOWN(mem_end, PGSIZE);

    // (7) 只有非空的完整页面区间才注册给当前 pmm_manager。
    //     此时这些页仍处于 reserved 状态，由 init_memmap() 负责清除标记并
    //     挂入空闲页链表（default_init_memmap 会先 assert(PageReserved(p))）。
    if (free_begin_pa < free_end_pa) {
        size_t n = (free_end_pa - free_begin_pa) / PGSIZE;
        init_memmap(pa2page(free_begin_pa), n);
    }
}


/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();
#ifdef LAB2_BUDDY_ATTACH
    pmm_manager->check();
#endif

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);

#ifdef LAB2_SLUB_ATTACH
    // lab2 模块二·Challenge（SLUB）守卫挂载点：默认关闭，仅在 DEFS=-DLAB2_SLUB_ATTACH 时接线
    slub_init();
    slub_check();
#endif
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
