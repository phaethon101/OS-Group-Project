# buddy_prompt2_生成测试 —— 为 Buddy 实现设计自检函数

## [PROMPT]

### 1.1 任务详情

**修改位置**：只在 `kern/mm/buddy_pmm.c` 内部新增测试代码，并把自检函数挂到 `struct pmm_manager` 的 `check` 成员上。不新增文件，不改动其它文件。

**功能目标**：为上一份 prompt 生成的伙伴系统分配器设计一套开机自动执行的测试，覆盖分配、释放、分裂、合并、耗尽与物理内存两端边界；不满足预期时立即 `panic`，线索要能定位到用例与页号。

**验收表现**：`make qemu` 先看到若干条测试进度行，最后打印 `check_alloc_page() succeeded!`；开启挂载点宏时自检被再次调用，但不重复跑用例、不改变系统状态。

### 1.2 操作约束

**可改**：`buddy_pmm.c` 内新增 `static` 测试函数与辅助函数、模块内静态常量与宏。

**不可改（逐条，附理由）**：
- 不得新建 `kern/test/`、不得出现 `test_lab2.h`：`tools/inject.py:140-144` 检测到即退出，评分直接失败。
- 不改 `pmm.c`：显式挂载点已由第一份 prompt 建立，再新增调用等于在开机流程里重复执行测试，且会污染注入脚本要求的锚点。
- 不改 `default_pmm.c`、`best_fit_pmm.c`、`tools/`：属于他人分工与评分脚本。
- 不改 `buddy_pmm.h` 的导出符号：对外只应有管理器实例。

**环境约束**：RISC-V 交叉编译，`-std=gnu99 -nostdinc -fno-builtin -Wall -Werror`；禁用 stdlib、系统头、浮点；打印只能用 `cprintf`。

**安全约束**：测试路径不得主动触发 `panic`——`__panic` 末尾是死循环，会把开机流程和评分超时一起拖死。非法输入类验证改为在说明里逐条列出判定依据，不写成运行期用例。

### 1.3 输出验证要求

**交付物**：①用例表；②完整测试代码；③执行顺序与状态复原说明。

**如何证明正确**：测试结束时空闲页计数与进入时相同；每个用例失败能定位到用例编号与页号；四类边界（最小 1 页、超限申请、非 2 的幂、内存两端）各有对应用例，并说明每类边界若被实现忽略会以什么现象暴露。

**交付格式**：先用例表（编号、覆盖行为、断言、为何有效），再代码，最后执行顺序与复原论证。

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
// [注] 来源：kern/mm/pmm.h:15-32（自检函数要挂到 check 成员上）
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
// [注] 来源：kern/mm/pmm.h:36-43（全局派发入口；测试只能用这些，不得直连内部结构）
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

// [注] 来源：kern/mm/pmm.h:34
extern const struct pmm_manager *pmm_manager;

// [注] 来源：kern/mm/pmm.c:149-152（check 成员就是在这里被调用；挂载点会再调用一次）
static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}

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

// [注] 来源：libs/stdio.h:7-9
/* kern/libs/stdio.c */
int cprintf(const char *fmt, ...);
int vcprintf(const char *fmt, va_list ap);

// [注] 来源：kern/debug/panic.c:18-29（__panic 末尾是死循环，测试不得主动触发）
    va_list ap;
    va_start(ap, fmt);
    cprintf("kernel panic at %s:%d:\n    ", file, line);
    vcprintf(fmt, ap);
    cprintf("\n");
    va_end(ap);

panic_dead:
    while (1) {
        ;
    }
}
```

## [GUARANTEE]

**对外接口**：不新增任何导出符号。`buddy_pmm_manager.check` 仍指向本文件的入口自检函数，测试代码全部 `static`。**入口自检函数必须复用第一份 prompt 已确定的名称 `buddy_check`**（仅 `[SPECIFICATION]` 按此名引用；`pmm.c` 的显式挂载点经 `pmm_manager->check()` 间接调用，**不按名引用**，因此测试函数可以且必须全部保持 `static`），不得再定义第二个同名或替代入口。

**核心函数（内部必须存在，除 `buddy_check` 外名称可自定）**：
1. 入口自检函数 `buddy_check`：保存进入时的空闲页计数，编排用例顺序，末尾校验状态复原；被重复调用时只做只读校验并打印一行提示。
2. 不变量校验函数：遍历全部阶与全部空闲块，校验块头字段、块大小、对齐性、块内页字段，并核对计数与链表总和一致。
3. 分配与分裂用例组：覆盖单页幂等、非 2 的幂请求的整块对齐、大块被切小后的剩余块归属。
4. 合并用例组：覆盖向下方伙伴合并、向上方伙伴合并，以及伙伴仍被占用时不得合并。
5. 耗尽与恢复用例组：覆盖一直分配到失败、再全部释放回初始布局，以及超限申请无副作用。

**显式授权**：可自行添加 `static` 辅助函数（向上取整、判 2 的幂、页号断言、进度打印）、模块内宏、模块内静态数组（暂存分配到的页指针）。

**显式禁止**：不得导出任何全局符号；不得修改被测函数的语义；不得替换 `.check` 的指向；不得把测试写进 `.init` 或 `.init_memmap`。

## [SPECIFICATION]

### 1. 入口自检函数（`check` 指向它）

- **前置状态**：由初始化流程在末尾调用；此时尚无业务分配，系统处于全空闲初始态；可能被连续调用两次。
- **后置效果**：全部用例通过后，空闲页计数与块布局与进入时完全一致；首次调用打印完整进度，后续调用只打印一行。
- **分支场景**：首次调用 → 依次执行各用例组并打印；再次调用 → 只做只读不变量校验，不重复执行破坏性用例；任一断言失败 → `panic`，信息中含用例编号。
- **对应要求**：必须幂等（连续两次结果一致）；内部用例必须自己把借出的页还清；不得依赖调用方的额外状态。

### 2. 不变量校验函数

- **前置状态**：可在任意时刻调用，只读，不改变任何结构。
- **后置效果**：断言各阶空闲块头 `PageProperty` 为真、`property` 等于块页数、起始页号按块大小对齐、`ref` 为 0；块内其余页 `property` 与 `PageProperty` 均为零；各阶块页数之和等于 `nr_free_pages()`。
- **分支场景**：结构自洽 → 正常返回；块头字段与块大小不符、块内页残留非零字段、计数与链表不一致 → `panic`。
- **对应要求**：不得只校验链表长度而漏掉块内页；不得写入任何字段。

### 3. 分配与分裂用例组

- **前置状态**：系统全空闲；记录进入时的空闲页数作为基准。
- **后置效果**：每个用例结束时页已全部归还，计数回到基准。
- **分支场景**：单页反复申请与释放 → 计数每次复原；请求非 2 的幂页数 → 返回块按「不小于请求值的最小 2 的幂」对齐，且计数按整块页数下降；从大块中取小块 → 剩余块仍可被再次分配。任何分配返回空指针、块不对齐、块头字段不符 → `panic`。
- **对应要求**：不得假设内存里存在某种特定大小的块；用例取页数量要小，避免依赖具体内存容量。

### 4. 合并用例组

- **前置状态**：系统全空闲；基准计数已记录。
- **后置效果**：每个用例结束时页已全部归还，计数回到基准。
- **分支场景**：释放一个块且其下方伙伴空闲 → 合并成更大块；上方伙伴空闲 → 合并后块头下移；伙伴仍被占用 → 禁止合并，两块各自独立；内存两端的块 → 越界一侧不得参与合并。
- **对应要求**：必须包含「伙伴被占用时不得合并」的反例，否则测不出「不看伙伴状态就合并」这类错误；不得靠遍历内部链表做断言，只能通过对外接口与页字段观察。

### 5. 耗尽与恢复用例组

- **前置状态**：系统全空闲；基准计数已记录。
- **后置效果**：全部释放后计数回到基准，块布局与初始一致。
- **分支场景**：持续申请直到返回空指针 → 计数为 0，再次申请仍返回空指针；申请页数超过最大阶或超过可管理页总数 → 返回空指针且计数不变；把此前申请到的页逆序释放 → 计数逐级回升至基准。
- **对应要求**：申请数量必须设上限，避免在真实内存下长时间阻塞开机；耗尽状态下不得重新初始化分配器或重新交付区间。

## [符号一致性自查]

- `[SPECIFICATION]` 中出现的符号均有出处：`PageProperty`、`property`、`ref`、`nr_free_pages`、`alloc_pages`、`free_pages`、`pages`、`npage`、`nbase`、`page2ppn`、`assert`、`panic`、`cprintf` 见 `[RELY]`；用例组函数、只读校验函数、静态暂存数组属 `[GUARANTEE]` 显式授权自造。
