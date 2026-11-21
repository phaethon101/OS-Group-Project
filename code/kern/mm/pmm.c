#include <default_pmm.h>
#include <defs.h>
#include <error.h>
#include <kmalloc.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <sync.h>
#include <vmm.h>
#include <riscv.h>
#include <dtb.h>

// 物理页描述符数组的虚拟地址
struct Page *pages;
// 物理内存总量（以页为单位）
size_t npage = 0;
// 内核镜像按 VA=KERNBASE、PA=info.base 建立映射
uint_t va_pa_offset;
// RISC-V 上物理内存从 0x80000000 开始
const size_t nbase = DRAM_BASE / PGSIZE;

// 启动期页目录的虚拟地址
pde_t *boot_pgdir_va = NULL;
// 启动期页目录的物理地址
uintptr_t boot_pgdir_pa;

// 物理内存管理器
const struct pmm_manager *pmm_manager;

static void check_alloc_page(void);
static void check_pgdir(void);
static void check_boot_pgdir(void);

// init_pmm_manager - 初始化一个 pmm_manager 实例
static void init_pmm_manager(void)
{
    pmm_manager = &default_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - 调 pmm->init_memmap，把空闲页建成 Page 结构
static void init_memmap(struct Page *base, size_t n)
{
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - 调 pmm->alloc_pages，分配连续 n 个页（n*PAGESIZE 字节）
struct Page *alloc_pages(size_t n)
{
    struct Page *page = NULL;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        page = pmm_manager->alloc_pages(n);
    }
    local_intr_restore(intr_flag);
    return page;
}

// free_pages - 调 pmm->free_pages，释放连续 n 个页
void free_pages(struct Page *base, size_t n)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        pmm_manager->free_pages(base, n);
    }
    local_intr_restore(intr_flag);
}

// nr_free_pages - 调 pmm->nr_free_pages，返回当前空闲页数
size_t nr_free_pages(void)
{
    size_t ret;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        ret = pmm_manager->nr_free_pages();
    }
    local_intr_restore(intr_flag);
    return ret;
}

//lab2 请补充你在lab2中的代码

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

static void enable_paging(void)
{
    write_csr(satp, 0x8000000000000000 | (boot_pgdir_pa >> RISCV_PGSHIFT));
}

// boot_map_segment - 建立并启用分页机制
// 参数
//  la:   需要映射的线性地址（x86 上还要经过段映射，RISC-V 上就是虚拟地址）
//  size: 映射长度
//  pa:   这段内存的物理地址
//  perm: 权限位
static void boot_map_segment(pde_t *pgdir, uintptr_t la, size_t size,
                             uintptr_t pa, uint32_t perm)
{
    assert(PGOFF(la) == PGOFF(pa));
    size_t n = ROUNDUP(size + PGOFF(la), PGSIZE) / PGSIZE;
    la = ROUNDDOWN(la, PGSIZE);
    pa = ROUNDDOWN(pa, PGSIZE);
    for (; n > 0; n--, la += PGSIZE, pa += PGSIZE)
    {
        pte_t *ptep = get_pte(pgdir, la, 1);
        assert(ptep != NULL);
        *ptep = pte_create(pa >> PGSHIFT, PTE_V | perm);
    }
}

// boot_alloc_page - 用 pmm->alloc_pages(1) 分配一页
// 返回值：这一页的内核虚拟地址
// 说明：该函数用于为 PDT（页目录表）和 PT（页表）取内存
static void *boot_alloc_page(void)
{
    struct Page *p = alloc_page();
    if (p == NULL)
    {
        panic("boot_alloc_page failed.\n");
    }
    return page2kva(p);
}

// pmm_init - 建立物理内存管理器，构建 PDT/PT 以启用分页
//           - 并检查 pmm 与分页机制的正确性
void pmm_init(void)
{
    // 内核需要按 4KB（或更大）的粒度分配、释放物理内存，
    // 所以 pmm.h 里定义了一套物理内存管理器的框架（struct pmm_manager）。
    // 先按这套框架初始化一个内存管理器，它才能分配/释放物理内存；
    // 目前可选的有 first_fit / best_fit / worst_fit / buddy_system。
    init_pmm_manager();

    // 探测物理内存范围、保留已被占用的部分，
    // 再用 pmm->init_memmap 建立空闲页链表
    page_init();

    // 用 pmm->check 检查内存管理器的分配/释放是否正确
    check_alloc_page();
    // 建立 boot_pgdir：启动期的页目录表（PDT）
    extern char boot_page_table_sv39[];
    boot_pgdir_va = (pte_t *)boot_page_table_sv39;
    boot_pgdir_pa = PADDR(boot_pgdir_va);

    check_pgdir();

    static_assert(KERNBASE % PTSIZE == 0 && KERNTOP % PTSIZE == 0);

    // 此时基本的虚拟内存映射（见 memlayout.h）已经建立，
    // 检查这套基本映射是否正确
    check_boot_pgdir();

    kmalloc_init();
}

// get_pte - 找到线性地址 la 对应的页表项，返回它的内核虚拟地址
//         - 若该页表项所在的页表页不存在，可按需为它分配一页
// 参数：
//  pgdir:  PDT 的内核虚拟基地址
//  la:     需要映射的线性地址
//  create: 布尔值，决定页表页不存在时是否分配
// 返回值：该页表项的内核虚拟地址
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create)
{
    pde_t *pdep1 = &pgdir[PDX1(la)];
    if (!(*pdep1 & PTE_V))
    {
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep1 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }
    pde_t *pdep0 = &((pte_t *)KADDR(PDE_ADDR(*pdep1)))[PDX0(la)];
    if (!(*pdep0 & PTE_V))
    {
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep0 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }
    return &((pte_t *)KADDR(PDE_ADDR(*pdep0)))[PTX(la)];
}

// get_page - 按 PDT 与线性地址 la 找出对应的 Page 结构
struct Page *get_page(pde_t *pgdir, uintptr_t la, pte_t **ptep_store)
{
    pte_t *ptep = get_pte(pgdir, la, 0);
    if (ptep_store != NULL)
    {
        *ptep_store = ptep;
    }
    if (ptep != NULL && *ptep & PTE_V)
    {
        return pte2page(*ptep);
    }
    return NULL;
}

//LAB4 function1 2410621

/* *
 * page_remove_pte - 撤销 ptep 这一条页表项所描述的映射
 *
 * 页表项内容、物理页引用计数、TLB 三者必须同步更新：
 *   1. 引用计数减一，减到 0 说明没人再用这一页了，真正还给 pmm_manager；
 *   2. 页表项清零，防止悬空映射；
 *   3. 让 TLB 里这条翻译失效，否则 CPU 还会走旧映射。
 * 调用者必须保证 ptep 非空；本函数对"本来就无效"的项安静跳过。
 * */
static void page_remove_pte(pde_t *pgdir, uintptr_t la, pte_t *ptep)
{
    if (*ptep & PTE_V)
    {
        struct Page *page = pte2page(*ptep);
        if (page_ref_dec(page) == 0)
        {
            // 最后一个引用也消失了，这一页可以回收
            free_page(page);
        }
        *ptep = 0;
        tlb_invalidate(pgdir, la);
    }
}

/* *
 * page_remove - 撤销线性地址 la 的映射（幂等操作）
 *
 * la 本来就没有映射时安静返回，不当作异常。
 * */
void page_remove(pde_t *pgdir, uintptr_t la)
{
    // create = 0：只查不建，不能为了删除一条映射而去新分配页表页
    pte_t *ptep = get_pte(pgdir, la, 0);
    if (ptep != NULL)
    {
        page_remove_pte(pgdir, la, ptep);
    }
}

/* *
 * page_insert - 把物理页 page 映射到线性地址 la，权限位为 perm
 *
 * 成功返回 0；get_pte(create=1) 可能为了建中间级页表而分配物理页，
 * 一旦分配失败就返回 -E_NO_MEM，此时页表保持原样，不会留下半完成的映射。
 * */
int page_insert(pde_t *pgdir, struct Page *page, uintptr_t la, uint32_t perm)
{
    // create = 1：中间级页表页不存在就现分配
    pte_t *ptep = get_pte(pgdir, la, 1);
    if (ptep == NULL)
    {
        return -E_NO_MEM;
    }

    // 先为这条新映射加一次引用；若发现是"同一页重复建映射"再退回去
    page_ref_inc(page);

    if (*ptep & PTE_V)
    {
        struct Page *p = pte2page(*ptep);
        if (p == page)
        {
            // 情况一：la 原本就映射到同一个物理页，只是要改权限。
            // 不能走 page_remove_pte，否则会把刚加的这次引用和原有的那次
            // 一起减掉，引用计数会凭空少一个。
            page_ref_dec(page);
        }
        else
        {
            // 情况二：la 已经映射到别的物理页，先把旧映射完整撤销
            // （旧的引用计数要减，减到 0 就回收那一页）
            page_remove_pte(pgdir, la, ptep);
        }
    }

    // pte_create() 内部已经或上 PTE_V，perm 只负责 RWXU 等权限位
    *ptep = pte_create(page2ppn(page), perm);
    tlb_invalidate(pgdir, la);
    return 0;
}

// 使 TLB 中某一条翻译失效；只有被修改的页表正是当前
// CPU 正在使用的那一份时才有必要。
void tlb_invalidate(pde_t *pgdir, uintptr_t la)
{
    // flush_tlb();
    // flush_tlb 会刷新整个 TLB，有没有更好的办法？
    asm volatile("sfence.vma %0" : : "r"(la));
}

static void check_alloc_page(void)
{
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}

static void check_pgdir(void)
{
    // assert(npage <= KMEMSIZE / PGSIZE);
    // RISC-V 上内存从 2GB 起，所以 npage 总是大于 KMEMSIZE / PGSIZE
    size_t nr_free_store;

    nr_free_store = nr_free_pages();

    assert(npage <= KERNTOP / PGSIZE);
    assert(boot_pgdir_va != NULL && (uint32_t)PGOFF(boot_pgdir_va) == 0);
    assert(get_page(boot_pgdir_va, 0x0, NULL) == NULL);

    struct Page *p1, *p2;
    p1 = alloc_page();
    assert(page_insert(boot_pgdir_va, p1, 0x0, 0) == 0);

    pte_t *ptep;
    assert((ptep = get_pte(boot_pgdir_va, 0x0, 0)) != NULL);
    assert(pte2page(*ptep) == p1);
    assert(page_ref(p1) == 1);

    ptep = (pte_t *)KADDR(PDE_ADDR(boot_pgdir_va[0]));
    ptep = (pte_t *)KADDR(PDE_ADDR(ptep[0])) + 1;
    assert(get_pte(boot_pgdir_va, PGSIZE, 0) == ptep);

    p2 = alloc_page();
    assert(page_insert(boot_pgdir_va, p2, PGSIZE, PTE_U | PTE_W) == 0);
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL);
    assert(*ptep & PTE_U);
    assert(*ptep & PTE_W);
    assert(boot_pgdir_va[0] & PTE_U);
    assert(page_ref(p2) == 1);

    assert(page_insert(boot_pgdir_va, p1, PGSIZE, 0) == 0);
    assert(page_ref(p1) == 2);
    assert(page_ref(p2) == 0);
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL);
    assert(pte2page(*ptep) == p1);
    assert((*ptep & PTE_U) == 0);

    page_remove(boot_pgdir_va, 0x0);
    assert(page_ref(p1) == 1);
    assert(page_ref(p2) == 0);

    page_remove(boot_pgdir_va, PGSIZE);
    assert(page_ref(p1) == 0);
    assert(page_ref(p2) == 0);

    assert(page_ref(pde2page(boot_pgdir_va[0])) == 1);

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0]));
    free_page(pde2page(pd0[0]));
    free_page(pde2page(pd1[0]));
    boot_pgdir_va[0] = 0;
    flush_tlb();

    assert(nr_free_store == nr_free_pages());

    cprintf("check_pgdir() succeeded!\n");
}

static void check_boot_pgdir(void)
{
    size_t nr_free_store;
    pte_t *ptep;
    int i;

    nr_free_store = nr_free_pages();

    for (i = ROUNDDOWN(KERNBASE, PGSIZE); i < npage * PGSIZE; i += PGSIZE)
    {
        assert((ptep = get_pte(boot_pgdir_va, (uintptr_t)KADDR(i), 0)) != NULL);
        assert(PTE_ADDR(*ptep) == i);
    }

    assert(boot_pgdir_va[0] == 0);

    struct Page *p;
    p = alloc_page();
    assert(page_insert(boot_pgdir_va, p, 0x100, PTE_W | PTE_R) == 0);
    assert(page_ref(p) == 1);
    assert(page_insert(boot_pgdir_va, p, 0x100 + PGSIZE, PTE_W | PTE_R) == 0);
    assert(page_ref(p) == 2);

    const char *str = "ucore: Hello world!!";
    strcpy((void *)0x100, str);
    assert(strcmp((void *)0x100, (void *)(0x100 + PGSIZE)) == 0);

    *(char *)(page2kva(p) + 0x100) = '\0';
    assert(strlen((const char *)0x100) == 0);

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0]));
    free_page(p);
    free_page(pde2page(pd0[0]));
    free_page(pde2page(pd1[0]));
    boot_pgdir_va[0] = 0;
    flush_tlb();

    assert(nr_free_store == nr_free_pages());

    cprintf("check_boot_pgdir() succeeded!\n");
}

// perm2str - 用 'u,r,w,-' 这样的字符串表示权限位
static const char *perm2str(int perm)
{
    static char str[4];
    str[0] = (perm & PTE_U) ? 'u' : '-';
    str[1] = 'r';
    str[2] = (perm & PTE_W) ? 'w' : '-';
    str[3] = '\0';
    return str;
}

// get_pgtable_items - 在 PDT 或 PT 的 [left, right) 范围内找一段连续的线性地址空间
//                  - 对应 (left_store*X_SIZE ~ right_store*X_SIZE) 这段区间
//                  - PDT 时 X_SIZE = PTSIZE = 4M；PT 时 X_SIZE = PGSIZE = 4K
// 参数：
//  left:        未使用
//  right:       表格范围的右边界
//  start:       表格范围的左边界
//  table:       表格起始地址
//  left_store:  下一段范围右边界的存放位置
//  right_store: 下一段范围左边界的存放位置
// 返回值：0 表示没有有效的表项区间；否则返回该区间的权限位
static int get_pgtable_items(size_t left, size_t right, size_t start,
                             uintptr_t *table, size_t *left_store,
                             size_t *right_store)
{
    if (start >= right)
    {
        return 0;
    }
    while (start < right && !(table[start] & PTE_V))
    {
        start++;
    }
    if (start < right)
    {
        if (left_store != NULL)
        {
            *left_store = start;
        }
        int perm = (table[start++] & PTE_USER);
        while (start < right && (table[start] & PTE_USER) == perm)
        {
            start++;
        }
        if (right_store != NULL)
        {
            *right_store = start;
        }
        return perm;
    }
    return 0;
}
