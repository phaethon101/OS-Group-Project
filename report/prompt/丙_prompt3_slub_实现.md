# slub_prompt1_生成模块 —— SLUB 两层分配器的实现与内核接入

## [PROMPT]

### 1.1 任务详情

**修改位置**：新增 `kern/mm/slub.h`、`kern/mm/slub.c`；`kern/mm/pmm.c` 加两处接入（`#include` + 守卫挂载点），合计三件事，见 1.2。

**功能目标**：在页级分配器之上实现两层内存分配。第一层复用全局 `alloc_pages` / `free_pages` 取得与归还连续物理页；第二层按 2 的幂档位建立对象缓存，把页切成对象供任意大小请求使用。超过一页的请求不做切分，直接向第一层申请 `ceil(请求字节 / PGSIZE)` 页并整块归还。

**验收表现**：`make qemu DEFS=-DLAB2_SLUB_ATTACH` 时，先打印 `check_alloc_page() succeeded!`（页级管理器自检），**随后**打印 slub 测试进度行；默认编译不包含任何 slub 调用路径（未被引用时 `slub.o` 会被 `ld --gc-sections` 整体丢弃）。

### 1.2 操作约束

**可改（仅此三处）**：
1. 全新建 `slub.h` / `slub.c`；
2. `pmm.c` 顶部加 `#include <slub.h>`；
3. `pmm_init()` 的 `check_alloc_page();` 之后加默认关闭的挂载点（放在函数末尾、`satp` 两行输出之后亦可，只要在 `check_alloc_page()` 之后）：

```c
#ifdef LAB2_SLUB_ATTACH
    slub_init();
    slub_check();
#endif
```

> 必须先 `slub_init()` 再 `slub_check()`：前者建立静态元数据，后者依赖它。

**不可改（逐条，附理由）**：
- 不改 `init_pmm_manager()`：slub 不是页级管理器，替换它会破坏既有分配链。
- 不改 `default_pmm.c`、`best_fit_pmm.c`、`tools/`：页级分配器属他人分工，`tools/` 是评分脚本。
- 不得新建 `kern/test/`、不得出现 `test_lab2.h`：`tools/inject.py:140-144` 检测到即退出，评分失败。
- `pmm.c` 不得出现第二行 `#include <pmm.h>`：注入脚本要求该行恰好匹配一行。
- 不得让 slub 直接引用任何具体页级管理器实例：只能经全局函数取页，否则"两层"退化为耦合。

**环境约束**：交叉编译 `-std=gnu99 -nostdinc -fno-builtin -Wall -Werror`；禁用 stdlib 与系统头、浮点、`malloc`；打印只能用 `cprintf`。开启验证用 `make qemu DEFS=-DLAB2_SLUB_ATTACH`，不得用 `make CFLAGS+=...`（会覆盖整个编译选项变量）。

**安全约束**：释放路径遇到无法归属或重复释放的指针必须 `panic`，不得静默忽略。

### 1.3 输出验证要求

**交付物**：①设计说明；②两个文件的完整可编译代码；③`pmm.c` 的 unified diff 与回滚步骤。

**如何证明正确**：`slub_alloc(PGSIZE + 1)` 实际向第一层取了几页、为什么；同一指针被连续释放两次时靠什么被发现；每个 slab 的页数是如何定的、一个 4096 字节对象需要几页；全部对象释放后 `nr_free_pages()` 是否精确回到 `slub_init` 之后的值；请求 8、4096、4097 字节分别走哪条路径。

**交付格式**：先设计说明（档位划分、slab 页数规则、大小对象归属判定方式），再代码，最后接入补丁与回滚。

## [RELY]

```c
// [注] 本段全部为 ucore 既有源码的逐字引用（行号见每条来源）；我方文字一律以 // [注] 开头。
// [注] ============ 宏定义 ============
// [注] 来源：kern/mm/mmu.h:9-13
#define PGSIZE          4096                    // bytes mapped by a page
#define PGSHIFT         12                      // log2(PGSIZE)

// physical/virtual page number of address
#define PPN(la) (((uintptr_t)(la)) >> PGSHIFT)

// [注] ============ 数据结构 ============
// [注] 来源：kern/mm/memlayout.h:34-39
struct Page {
    int ref;                        // page frame's reference counter
    uint64_t flags;                 // array of flags that describe the status of the page frame
    unsigned int property;          // the num of free block, used in first fit pm manager
    list_entry_t page_link;         // free list link
};

// [注] ============ 可用函数 ============
// [注] 来源：kern/mm/pmm.h:36-43（第一层入口：经 pmm_manager 派发，不绑定具体管理器）
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
// [注] 内核直映射：可写虚拟地址 = 物理地址 + va_pa_offset；pmm.h:63-77 的 KADDR 被注释掉，不存在。

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
```

## [GUARANTEE]

**对外接口（唯一导出符号集合）**：

```c
// kern/mm/slub.h，含 include guard，只 #include <pmm.h>
void  slub_init(void);                    /* 初始化缓存；不得向第一层要页 */
void *slub_alloc(size_t size);            /* 失败返回 NULL；size 为 0 返回 NULL */
void  slub_free(void *ptr);               /* 归还；无法归属或重复释放则 panic */
void  slub_check(void);                   /* 自检，挂在 pmm.c 的守卫挂载点 */
```

**核心函数（内部必须存在，名称可自定）**：
1. 档位映射：把请求字节数映射到 2 的幂档位（下限 8 字节，上限一页），并判定是否属于跨页大对象。
2. 新 slab 获取：为一个档位从第一层取页、切成等长对象、串成空闲链。
3. 对象分配与回收：从空闲链取一个对象；回收时把对象放回所属 slab 的空闲链。
4. 整页归还：slab 内对象全部空闲时，把该 slab 占用的页整块还给第一层。
5. 跨页大对象：按页数向第一层整块申请、整块归还，并能由返回指针反查其归属与页数。
6. 归属判定：仅凭一个指针区分「slab 内对象」与「跨页大对象」。

**显式授权**：可自行添加 `static` 辅助函数、模块内宏、模块内静态变量（档位描述符数组、基准页数等）；可自行定义内部结构体与内部链表链接方式，不必依赖内核 `list.h`。

**显式禁止**：除上述四个函数外不得导出任何全局符号；不得定义与本题无关的全局变量；不得修改 `pmm_manager`、`pages`、`npage`、`nbase`、`va_pa_offset`。

## [SPECIFICATION]

### 1. `slub_init`

- **前置状态**：页级分配器已就绪（`page_init` 完成）；可被重复调用。
- **后置效果**：各档位缓存结构清空、无任何 slab；**不向第一层申请任何页**，`nr_free_pages()` 与调用前相同。
- **分支场景**：首次调用 → 建立各档位结构，不取页；重复调用 → 与首次结果相同，且不得泄漏已取页（若实现不允许重复调用，必须显式 `panic` 并在说明里写清）。
- **对应要求**：初始化后 `nr_free_pages()` 必须等于调用前，这是后续"无泄漏"断言的基准；不得在此函数内调用 `alloc_pages`。

### 2. `slub_alloc`

- **前置状态**：`size` 为字节数；`slub_init` 已调用。
- **后置效果**：成功时返回一段**至少可写 `size` 字节**的连续区域，且该区域必须整体可供调用方使用（归属信息不得存放在会被调用方覆盖的位置）；跨页请求占用 `ceil(size / PGSIZE)` 个连续页，返回指针指向该组页的起始地址，整段 `size` 字节（含首页开头）均可写；返回指针按 8 字节对齐。失败返回 `NULL` 且不改变任何缓存状态。
- **分支场景**：`size == 0` → 返回 `NULL`；`size ≤ PGSIZE` → 向上取整到 2 的幂档位，从对应 slab 分配，slab 无空闲对象时向第一层取页新建 slab，第一层无页则返回 `NULL`；`size > PGSIZE` → 按页数向上取整向第一层整块申请，失败返回 `NULL`；第一层返回页失败 → 返回 `NULL`，不得留下半个 slab。
- **对应要求**：单次请求占用的字节数不得超过档位粒度造成的一页以内浪费（跨页请求除外）；不得返回重叠区域；不得因失败而破坏已有空闲链。

### 3. `slub_free`

- **前置状态**：`ptr` 必须是某次 `slub_alloc` 的返回值，且未被释放过。
- **后置效果**：对象回到所属 slab 的空闲链；若该 slab 的对象全部空闲，则其占用的页被整块归还第一层；跨页大对象按申请时的页数整块归还。归还后 `nr_free_pages()` 相应增加。
- **分支场景**：`ptr == NULL` → 直接返回，不 `panic`；指针属于 slab 且是对象起点 → 正常回收；指针属于 slab 但不是对象起点 → `panic`；指针落在跨页大对象占用的页上 → 按整块归还；指针既不属于任何 slab 也不属于任何大对象、或已被释放过 → `panic` 并给出可定位信息。
- **对应要求**：归属判定只能依据 `ptr` 本身，不得要求调用方额外传参；归还页数必须与申请时完全一致；同一指针二次释放必须被发现，不得静默返回。

### 4. `slub_check`

- **前置状态**：由 `pmm.c` 的守卫挂载点在初始化流程末尾调用一次；此时无业务分配，系统处于全空闲初始态。
- **后置效果**：全部断言通过；退出时各档位缓存回到初始状态（无 slab 残留），`nr_free_pages()` 与进入时一致。
- **分支场景**：各档位往返分配释放 → 计数复原；跨页大对象往返 → 计数复原；大小对象混排后逐个释放 → 计数最终复原；发现对象重叠、计数不符、整页未归还 → `panic` 并指出档位与页号。
- **对应要求**：不得主动触发 `panic`（`__panic` 末尾是死循环，会拖死开机与评分超时）；不得假设内存总量；内部用例必须自己借自己还。

## [符号一致性自查]

- `[SPECIFICATION]` 中出现的符号均有出处：`PGSIZE`、`PPN`、`alloc_pages`、`free_pages`、`nr_free_pages`、`pages`、`npage`、`nbase`、`va_pa_offset`、`page2ppn`、`page2pa`、`pa2page`、`panic`、`assert`、`cprintf`、`struct Page` 见 `[RELY]`；`slub_init` / `slub_alloc` / `slub_free` / `slub_check` 见 `[GUARANTEE]` 对外接口；档位描述符、slab 结构、空闲链、归属判定辅助函数属 `[GUARANTEE]` 显式授权自造。
