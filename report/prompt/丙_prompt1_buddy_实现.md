# buddy_prompt1_生成模块 —— Buddy 分配器的实现与内核接入

## [PROMPT]

### 1.1 任务详情

**修改位置**：新增 `kern/mm/buddy_pmm.h`、`kern/mm/buddy_pmm.c`；改动 `kern/mm/pmm.c` 三处。

**功能目标**：在 ucore 现有页级物理内存管理框架下，从零实现伙伴系统分配器：把可管理物理页按 2 的幂块（阶）组织，支持任意页数的连续申请（向上取整成整块）、块分裂与相邻伙伴合并，完整实现框架要求的六个成员函数，由框架统一派发。

**验收表现**：临时切到该管理器后 `make qemu` 打印 `memory management: buddy_pmm_manager`，随后打印 `check_alloc_page() succeeded!`（自检不 panic）。

### 1.2 操作约束

**可改（仅此三处）**：
1. 全新建 `buddy_pmm.h` / `buddy_pmm.c`；
2. `pmm.c` 顶部 `#include <best_fit_pmm.h>` 之后加一行 `#include <buddy_pmm.h>`；
3. `init_pmm_manager()` 内把 `&default_pmm_manager` 换成 `&buddy_pmm_manager`；并在 `pmm_init()` 的 `check_alloc_page();` 之后加默认关闭的挂载点：

```c
#ifdef LAB2_BUDDY_ATTACH
    pmm_manager->check();   /* 显式挂载点：再次调用当前管理器的自检，测试函数保持 static */
#endif
```

> **挂载点必须经函数指针调用，不得按名调用 `buddy_check()`**：自测函数是 `static`，
> 按名引用会导致开启 `-DLAB2_BUDDY_ATTACH` 时链接失败（该缺陷已在第一轮实测中暴露并修正）。

**不可改（逐条，附理由）**：
- `Makefile`：`kern/mm/` 下新增 `.c` 会被自动通配编译，不需要登记，改了反而易错。
- `default_pmm.c`、`best_fit_pmm.c`、`tools/` 下任何文件：它们分别承担他人的分工与评分脚本，动一处即为越权。
- 不得新建 `kern/test/`、不得出现 `test_lab2.h`：`tools/inject.py:140-144` 检测到即退出，评分直接失败。
- `pmm.c` 不得出现第二行 `#include <pmm.h>`：注入脚本要求该行恰好匹配一行，否则注入失败。
- 最终提交态管理器必须回到 `&best_fit_pmm_manager`（`tools/test_lab2.c:106` 硬校验），挂载点须保持关闭。

**环境约束**：RISC-V 交叉编译，选项 `-mcmodel=medany -std=gnu99 -nostdinc -fno-builtin -Wall -Werror`。禁止 stdlib 与系统头、浮点、x86 内联汇编、`malloc`；打印只能用 `cprintf`。开启挂载点验证用 `make qemu DEFS=-DLAB2_BUDDY_ATTACH`（`DEFS` 在 Makefile 中从未赋值，是干净的填空）；不得用 `make CFLAGS+=...`，那会覆盖整个编译选项变量导致编译失败。

### 1.3 输出验证要求

**交付物**：①设计说明；②两个文件的完整可编译代码；③`pmm.c` 的 unified diff 与回滚步骤。

**如何证明正确**：`alloc_pages(5)` 使空闲页计数减少几页、为什么这样记账仍自洽；给出 `struct Page` 的 `flags` / `property` / `ref` 在空闲块头、空闲块内部页、已分配块头、已分配块内部页、内核保留页五种状态下的取值表；列出所有会触发 `panic` 的输入及判定依据。

**交付格式**：先设计说明（结构选型理由、阶上限取值、计数口径），再代码，最后接入补丁与回滚。

## [RELY]

```c
// [注] 本段全部为 ucore 既有源码的逐字引用（行号见每条来源）；我方文字一律以 // [注] 开头。
// [注] ============ 宏定义 ============
// [注] 来源：kern/mm/mmu.h:9-13
#define PGSIZE          4096                    // bytes mapped by a page
#define PGSHIFT         12                      // log2(PGSIZE)

// physical/virtual page number of address
#define PPN(la) (((uintptr_t)(la)) >> PGSHIFT)

// [注] 来源：kern/mm/pmm.c:22-24（nbase = DRAM_BASE / PGSIZE = 0x80000 = 2^19）
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// [注] 来源：kern/mm/memlayout.h:42-50
#define PG_reserved                 0       // if this bit=1: the Page is reserved for kernel, cannot be used in alloc/free_pages; otherwise, this bit=0 
#define PG_property                 1       // if this bit=1: the Page is the head page of a free memory block(contains some continuous_addrress pages), and can be used in alloc_pages; if this bit=0: if the Page is the the head page of a free memory block, then this Page and the memory block is alloced. Or this Page isn't the head page.

#define SetPageReserved(page)       ((page)->flags |= (1UL << PG_reserved))
#define ClearPageReserved(page)     ((page)->flags &= ~(1UL << PG_reserved))
#define PageReserved(page)          (((page)->flags >> PG_reserved) & 1)
#define SetPageProperty(page)       ((page)->flags |= (1UL << PG_property))
#define ClearPageProperty(page)     ((page)->flags &= ~(1UL << PG_property))
#define PageProperty(page)          (((page)->flags >> PG_property) & 1)

// [注] ============ 数据结构 ============
// [注] 来源：kern/mm/pmm.h:15-32（本次要实现的目标接口）
struct pmm_manager {
    const char *name;  // XXX_pmm_manager's name
    void (*init)(
        void);  // initialize internal description&management data structure
                // (free block list, number of free block) of XXX_pmm_manager
    void (*init_memmap)(
        struct Page *base,
        size_t n);  // setup description&management data structcure according to
                    // the initial free physical memory space
    struct Page *(*alloc_pages)(
        size_t n);  // allocate >=n pages, depend on the allocation algorithm
    void (*free_pages)(struct Page *base, size_t n);  // free >=n pages with
                                                      // "base" addr of Page
                                                      // descriptor
                                                      // structures(memlayout.h)
    size_t (*nr_free_pages)(void);  // return the number of free pages
    void (*check)(void);            // check the correctness of XXX_pmm_manager
};

// [注] 来源：kern/mm/memlayout.h:34-39
struct Page {
    int ref;                        // page frame's reference counter
    uint64_t flags;                 // array of flags that describe the status of the page frame
    unsigned int property;          // the num of free block, used in first fit pm manager
    list_entry_t page_link;         // free list link
};

// [注] ============ 可用函数 ============
// [注] 来源：kern/mm/pmm.h:36-43
void pmm_init(void);

struct Page *alloc_pages(size_t n);
void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void); // number of free pages

#define alloc_page() alloc_pages(1)
#define free_page(page) free_pages(page, 1)

// [注] 来源：kern/mm/pmm.h:78-109
extern struct Page *pages;
extern size_t npage;
extern const size_t nbase;
extern uint64_t va_pa_offset;

static inline ppn_t page2ppn(struct Page *page) { return page - pages + nbase; }

static inline uintptr_t page2pa(struct Page *page) {
    return page2ppn(page) << PGSHIFT;
}



static inline int page_ref(struct Page *page) { return page->ref; }

static inline void set_page_ref(struct Page *page, int val) { page->ref = val; }

static inline int page_ref_inc(struct Page *page) {
    page->ref += 1;
    return page->ref;
}

static inline int page_ref_dec(struct Page *page) {
    page->ref -= 1;
    return page->ref;
}
static inline struct Page *pa2page(uintptr_t pa) {
    if (PPN(pa) >= npage) {
        panic("pa2page called with invalid pa");
    }
    return &pages[PPN(pa) - nbase];
}
// [注] 注意：pmm.h 中 KADDR 宏在原文里被 /* */ 整体注释掉（pmm.h:63-77），内核里不存在该符号。

// [注] 来源：libs/list.h:23-31,33-34（双向链表声明；ucore 没有 list_add_tail）
static inline void list_init(list_entry_t *elm) __attribute__((always_inline));
static inline void list_add(list_entry_t *listelm, list_entry_t *elm) __attribute__((always_inline));
static inline void list_add_before(list_entry_t *listelm, list_entry_t *elm) __attribute__((always_inline));
static inline void list_add_after(list_entry_t *listelm, list_entry_t *elm) __attribute__((always_inline));
static inline void list_del(list_entry_t *listelm) __attribute__((always_inline));
static inline void list_del_init(list_entry_t *listelm) __attribute__((always_inline));
static inline bool list_empty(list_entry_t *list) __attribute__((always_inline));
static inline list_entry_t *list_next(list_entry_t *listelm) __attribute__((always_inline));
static inline list_entry_t *list_prev(list_entry_t *listelm) __attribute__((always_inline));

static inline void __list_add(list_entry_t *elm, list_entry_t *prev, list_entry_t *next) __attribute__((always_inline));
static inline void __list_del(list_entry_t *prev, list_entry_t *next) __attribute__((always_inline));

// [注] 来源：kern/mm/memlayout.h:52-54
// convert list entry to page
#define le2page(le, member)                 \
    to_struct((le), struct Page, member)

// [注] 来源：kern/debug/assert.h:6-24
void __warn(const char *file, int line, const char *fmt, ...);
void __noreturn __panic(const char *file, int line, const char *fmt, ...);

#define warn(...)                                       \
    __warn(__FILE__, __LINE__, __VA_ARGS__)

#define panic(...)                                      \
    __panic(__FILE__, __LINE__, __VA_ARGS__)

#define assert(x)                                       \
    do {                                                \
        if (!(x)) {                                     \
            panic("assertion failed: %s", #x);          \
        }                                               \
    } while (0)

// static_assert(x) will generate a compile-time error if 'x' is false.
#define static_assert(x)                                \
    switch (x) { case 0: case (x): ; }

// [注] 来源：libs/stdio.h:7-9（cprintf 声明；ucore 没有 printf）
/* kern/libs/stdio.c */
int cprintf(const char *fmt, ...);
int vcprintf(const char *fmt, va_list ap);

// [注] 来源：kern/mm/default_pmm.c:17-28（First-Fit 字段约定：交给 init_memmap 的页必须 reserved，然后把 flags/property 清零、ref 归零）
static void
default_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;

// [注] 来源：kern/mm/default_pmm.c:60-70（分配时按请求页数从块头切走，并 ClearPageProperty）
    if (page != NULL) {
        list_entry_t* prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));
        if (page->property > n) {
            struct Page *p = page + n;
            p->property = page->property - n;
            SetPageProperty(p);
            list_add(prev, &(p->page_link));
        }
        nr_free -= n;
        ClearPageProperty(page);
```

## [GUARANTEE]

**对外接口（唯一导出符号）**：

```c
// kern/mm/buddy_pmm.h，含 include guard，只 #include <pmm.h>
extern const struct pmm_manager buddy_pmm_manager;
```

`.name` 必须是 `"buddy_pmm_manager"`，六个成员依次指向本题实现的初始化、区间交付、分配、释放、空闲页计数、自检函数。**自检函数必须命名为 `buddy_check`**（仅 `[SPECIFICATION]` 第 6 节按此名引用；`pmm.c` 的挂载点经 `pmm_manager->check()` 调用，不按名引用，故该函数可保持 `static`），且 `.check` 指向它。

**核心函数（内部必须存在，名称可自定）**：
1. 阶计算：由请求页数求向上取整的阶（最小的 k 使 2^k ≥ n）。
2. 块分裂：把高阶块逐层对半拆到目标阶，拆出的后半块进入低一阶的空闲结构。
3. 伙伴定位与合并：按页号在对应阶上定位伙伴，判其是否同阶空闲，必要时合并并更新块头。
4. 区间分解：把任意起始、任意长度的一段页拆成若干个「按页号对齐」的 2 的幂块。

**显式授权**：可自行添加内部辅助函数（一律 `static`）、模块内宏、模块内静态变量（分阶空闲结构、空闲页计数等）。

**显式禁止**：除 `buddy_pmm_manager` 外不得导出任何全局符号；六个成员函数必须为 `static`；不得定义与本题无关的全局变量或函数。

## [SPECIFICATION]

### 1. `init` —— 初始化

- **前置状态**：框架尚未交付任何页；可被重复调用。
- **后置效果**：内部空闲结构全部清空，空闲页计数为 0；不读写任何 `struct Page`。
- **分支场景**：正常初始化 → 全部结构置空；重复调用 → 结果与首次相同（幂等）。
- **对应要求**：不得依赖调用顺序，不得在此函数内调用 `alloc_pages` / `free_pages`。

### 2. `init_memmap` —— 交付一段物理页

- **前置状态**：`base` 指向 `[pages, pages + (npage - nbase))` 内某页；`n > 0`；区间内每页均为保留页（`PageReserved` 为真）。
- **后置效果**：区间内所有页的 `flags`、`property` 清零、`ref` 为 0；区间被拆成若干对齐的 2 的幂块并进入空闲结构；空闲页计数增加 `n`。
- **分支场景**：`base` 按大块对齐 → 直接成为一个高阶块；`base` 不对齐或 `n` 非 2 的幂 → 贪心拆成多个块，不得丢弃任何页；与已交付区间相邻 → 自动合并成更大的块；`n == 0` → `panic`。
- **对应要求**：可被多次调用且累积生效；合并过程不得改变空闲页计数；不得假设调用方一次性交付全部内存。

### 3. `alloc_pages` —— 分配连续页

- **前置状态**：`n > 0`。
- **后置效果**：成功时返回块头，块大小为「不小于 `n` 的最小 2 的幂」页，且块起始页号按块大小对齐；块头 `property` 等于块页数、`PageProperty` 为假、`ref` 为 1；块内其余页 `property` 为 0、`ref` 为 0；空闲页计数按**整块页数**减少。
- **分支场景**：存在刚好同阶的空闲块 → 直接摘出；只有更高阶块 → 逐层分裂到目标阶，拆出的后半块回到低阶；请求超过最大阶或内存不足 → 返回 `NULL` 且状态零变化；`n == 0` → `panic`。
- **对应要求**：失败路径不得有任何副作用；分裂过程不得改变空闲页计数；返回块的地址必须按块大小对齐，否则后续无法正确合并。

### 4. `free_pages` —— 释放

- **前置状态**：`base` 是某次 `alloc_pages` 的返回值；`n` 与分配时的请求值相同。
- **后置效果**：块变为空闲块头（`PageProperty` 为真、`property` 为块页数、`ref` 为 0）；若伙伴同阶且空闲则逐层合并，合并后的块头取地址较小者；空闲页计数按整块页数增加。
- **分支场景**：伙伴被占用或不同阶 → 不合并，仅入空闲结构；伙伴在下方 → 头下移；伙伴在上方 → 头上移；伙伴越出可管理区间（内存两端）→ 停止合并；重复释放、释放块内非首页、释放保留页、`n` 与分配时不符、`base` 非法、`n == 0` → `panic`。
- **对应要求**：判定伙伴可用前必须先做区间边界判断，严禁解引用区间外的页描述符；一次释放只能入空闲结构一次。

### 5. `nr_free_pages` —— 空闲页计数

- **前置状态**：无。
- **后置效果**：返回当前空闲结构中的物理页总数，恒等于各阶空闲块页数之和。
- **分支场景**：全部分配完 → 返回 0；全部释放回初始布局 → 返回初始值。
- **对应要求**：任何分配、释放、分裂、合并之后该恒等式都必须成立；计数按整块记账，与调用方请求的页数可能是两个数。

### 6. `check` —— 自检

- **前置状态**：由框架在初始化流程末尾调用一次；此时尚无业务分配。
- **后置效果**：全部断言通过；退出时空闲页计数与块布局与进入时完全一致；任何会 `panic` 的输入不得在此函数内被主动触发。
- **分支场景**：结构自洽 → 正常返回；发现块头字段与块大小不符、块内页字段非零、计数与链表不一致 → `panic` 并指出位置。
- **对应要求**：必须可重复调用且结果一致；不得修改被测语义；运行时间必须短（开机流程内完成）。

## [GUARANTEE 之外的自查]

- `[SPECIFICATION]` 中出现的符号均可在 `[RELY]` 找到出处（`PGSIZE`、`PageReserved`、`property`、`ref`、`pages`、`npage`、`nbase`、`page2ppn`、`panic`、`assert`、`cprintf`），或已在 `[GUARANTEE]` 获准自造（内部空闲结构、空闲页计数、阶计算/分裂/合并/区间分解）。
