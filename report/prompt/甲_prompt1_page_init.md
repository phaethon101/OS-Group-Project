# 任务：实现 riscv64-ucore lab2 的 page_init()

## 【Prompt｜任务说明】

### 任务详情
在 riscv64-ucore 的 lab2 工程中实现 `kern/mm/pmm.c` 里的 `static void page_init(void)`，
补齐内核的物理内存探测与 Page 描述符初始化。完成后 `pmm_init()` 的
`init_pmm_manager() -> page_init() -> check_alloc_page()` 链条必须能跑通，
`check_alloc_page()` 正常打印 succeeded。

### 操作约束
- 只允许修改 `kern/mm/pmm.c` 一个文件。改动范围仅限：新增 `page_init()`、
  可选地新增 `static` 内部辅助函数、以及补齐必要的 `#include`。
- 必须保留 `lab2` 注释标记，并按实验提交要求把 `YOUR CODE` 替换成本人学号。
- 代码风格与同文件现有代码一致（4 空格缩进、`/* * */` 注释块、变量声明靠近使用处）。
- 必须能在 `-std=gnu99 -Wall -Werror` 下零警告编译通过。
- 不要输出测试代码，不要输出 README，不要改 Makefile。

### 输出验证要求
- 给出可直接替换的完整 `page_init()` 代码块（不要给片段，不要给 diff）。
- 逐条说明你的实现如何满足下面【Specification】里的后置条件 1~13，
  尤其是涉及 `PADDR` / `PPN` / `nbase` 的那几条。
- 如果认为某两条约束无法同时满足，先明确指出冲突再给方案，不要静默取舍。

## 【Rely｜依赖说明】

> 本节按指导书要求「`[RELY]` 必须和工程代码一致——去代码里把真实的定义复制过来，不要凭记忆写差不多版本」重做：
> 以下全部为工程源码的逐字引用，我方解释一律用 `// [注]` 开头，以便与源码区分。

### 宏定义

```c
// [注] 来源：kern/mm/mmu.h:9-13
#define PGSIZE          4096                    // bytes mapped by a page
#define PGSHIFT         12                      // log2(PGSIZE)

// physical/virtual page number of address
#define PPN(la) (((uintptr_t)(la)) >> PGSHIFT)

// [注] 来源：kern/mm/memlayout.h:5-15
#define KERNBASE            0xFFFFFFFFC0200000 // = 0x80200000(物理内存里内核的起始位置, KERN_BEGIN_PADDR) + 0xFFFFFFFF40000000(偏移量, PHYSICAL_MEMORY_OFFSET)
//把原有内存映射到虚拟内存空间的最后一页
#define KMEMSIZE            0x7E00000          // the maximum amount of physical memory
// 0x7E00000 = 0x8000000 - 0x200000
// QEMU 缺省的RAM为 0x80000000到0x88000000, 128MiB, 0x80000000到0x80200000被OpenSBI占用
#define KERNTOP             (KERNBASE + KMEMSIZE) // 0x88000000对应的虚拟地址

#define PHYSICAL_MEMORY_END         0x88000000
#define PHYSICAL_MEMORY_OFFSET      0xFFFFFFFF40000000
#define KERNEL_BEGIN_PADDR          0x80200000
#define KERNEL_BEGIN_VADDR          0xFFFFFFFFC0200000

// [注] 来源：kern/mm/memlayout.h:41-50
/* Flags describing the status of a page frame */
#define PG_reserved                 0       // if this bit=1: the Page is reserved for kernel, cannot be used in alloc/free_pages; otherwise, this bit=0 
#define PG_property                 1       // if this bit=1: the Page is the head page of a free memory block(contains some continuous_addrress pages), and can be used in alloc_pages; if this bit=0: if the Page is the the head page of a free memory block, then this Page and the memory block is alloced. Or this Page isn't the head page.

#define SetPageReserved(page)       ((page)->flags |= (1UL << PG_reserved))
#define ClearPageReserved(page)     ((page)->flags &= ~(1UL << PG_reserved))
#define PageReserved(page)          (((page)->flags >> PG_reserved) & 1)
#define SetPageProperty(page)       ((page)->flags |= (1UL << PG_property))
#define ClearPageProperty(page)     ((page)->flags &= ~(1UL << PG_property))
#define PageProperty(page)          (((page)->flags >> PG_property) & 1)

// [注] 来源：libs/defs.h:53-63
 * */
#define ROUNDDOWN(a, n) ({                                          \
            size_t __a = (size_t)(a);                               \
            (typeof(a))(__a - __a % (n));                           \
        })

/* Round up to the nearest multiple of n */
#define ROUNDUP(a, n) ({                                            \
            size_t __n = (size_t)(n);                               \
            (typeof(a))(ROUNDDOWN((size_t)(a) + __n - 1, __n));     \
        })

```

### 数据结构

```c
// [注] 来源：kern/mm/memlayout.h:34-39
struct Page {
    int ref;                        // page frame's reference counter
    uint64_t flags;                 // array of flags that describe the status of the page frame
    unsigned int property;          // the num of free block, used in first fit pm manager
    list_entry_t page_link;         // free list link
};

```

### 可用函数

```c
// [注] 来源：kern/mm/pmm.h:78-81
extern struct Page *pages;
extern size_t npage;
extern const size_t nbase;
extern uint64_t va_pa_offset;

// [注] 来源：kern/mm/pmm.h:54-61
#define PADDR(kva)                                                 \
    ({                                                             \
        uintptr_t __m_kva = (uintptr_t)(kva);                      \
        if (__m_kva < KERNBASE) {                                  \
            panic("PADDR called with invalid kva %08lx", __m_kva); \
        }                                                          \
        __m_kva - va_pa_offset;                                    \
    })

// [注] 来源：kern/mm/pmm.h:83-83
static inline ppn_t page2ppn(struct Page *page) { return page - pages + nbase; }

// [注] 来源：kern/mm/pmm.h:85-87
static inline uintptr_t page2pa(struct Page *page) {
    return page2ppn(page) << PGSHIFT;
}

// [注] 来源：kern/mm/pmm.h:104-109
static inline struct Page *pa2page(uintptr_t pa) {
    if (PPN(pa) >= npage) {
        panic("pa2page called with invalid pa");
    }
    return &pages[PPN(pa) - nbase];
}

// [注] 来源：kern/driver/dtb.h:11-12
uint64_t get_memory_base(void);
uint64_t get_memory_size(void);

// [注] 来源：kern/mm/pmm.c:44-47
// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// [注] 来源：kern/mm/default_pmm.c:18-23
default_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;

```

## 【Guarantee｜接口清单】

必须交付：
- `static void page_init(void)` —— 在 `kern/mm/pmm.c` 中**有且仅有一处**定义，
  位于 `pmm_init()` 之前。
- 必须通过已有的 `static void init_memmap(struct Page *base, size_t n)` 注册空闲区，
  不得绕过它直接操作 `pmm_manager->init_memmap`。
- 允许自行添加 `static` 内部辅助函数，但必须定义在 `page_init()` 之前；不得新增文件。

绝对不可修改（改了即违约）：
- `struct Page` 的字段、字段顺序、大小；
- `PG_reserved` / `PG_property` 及 `SetPageReserved` / `ClearPageReserved` /
  `PageReserved` / `SetPageProperty` / `ClearPageProperty` / `PageProperty` 六个宏；
- `struct pmm_manager` 的成员签名与语义，以及 `pmm.h` 中 `PADDR` / `page2pa` /
  `pa2page` / `page2ppn` 的定义；
- `kern/mm/default_pmm.c` 的任何内容，**特别是 `default_init_memmap()` 里的
  `assert(PageReserved(p))`**；
- `tools/test_lab2.c` 的两个测试函数、`tools/inject.py`、`tools/grade.sh`、
  `tools/kernel.ld`、`Makefile`；
- `nbase` 的语义（DRAM 起始页号）和 `npage` 的语义（可管理页号上界）。

## 【Specification｜行为规格】

### 前置状态（这些已经成立，你不需要也不应该去重建它们）
- `dtb_init()` 已执行，`get_memory_base()` / `get_memory_size()` 返回 DTB `/memory`
  节点的范围（默认 `0x80000000` / `0x8000000`）。
- `init_pmm_manager()` 已执行：`pmm_manager` 非空，其 `init()` 已跑过
  （`free_list` 已初始化但为空，`nr_free == 0`）。
- `npage == 0`，`pages == NULL`。

### 执行步骤（按序，不得跳过）
S1. 置 `va_pa_offset = PHYSICAL_MEMORY_OFFSET`。

S2. 取 `mem_base`、`mem_size`，算 `mem_end = mem_base + mem_size`（不含）。
    - 分支 S2a（DTB 无效）：`mem_base == 0 || mem_size == 0` → 调用 `panic(...)` 终止。
      **不得**继续后续步骤，**不得**调用 `init_memmap()`。

S3. 打印探测结果，格式必须逐字符一致（`%016lx`，行尾有句点）：
    ```
    physical memory map:
      memory: 0x<mem_size>, [0x<mem_base>, 0x<mem_end-1>].
    ```
    三个数都按 16 位、小写、左补 0 输出。

S4. 置 `npage = PPN(mem_end)`。描述符个数为 `npage - nbase`
    （注意是减 `nbase`，不是直接用 `npage`）。

S5. 描述符数组起点：`pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE)`，
    其中 `end` 用 `extern char end[];` 在函数内声明，**`end` 是虚拟地址**。

S6. 对全部 `npage - nbase` 个描述符：先 `memset` 清零，再**逐个**
    `SetPageReserved()`。这一步结束后，不允许任何描述符处于可分配状态。

S7. 计算空闲区（这一步全部在**物理地址域**）：
    - `pages_end_pa = PADDR(pages) + (npage - nbase) * sizeof(struct Page)`
    - `free_begin_pa = ROUNDUP(pages_end_pa, PGSIZE)`
    - `free_end_pa   = ROUNDDOWN(mem_end, PGSIZE)`
    `end` 与 `pages` 都是虚拟地址，**必须经 `PADDR()` 才能与 `mem_end` 比较**。

S8. 分支 S8a（有空闲区）：`free_begin_pa < free_end_pa` →
    `init_memmap(pa2page(free_begin_pa), (free_end_pa - free_begin_pa) / PGSIZE)`。
    调用时这些页**必须仍处于 reserved 状态**：`default_init_memmap()` 内部循环里的
    `assert(PageReserved(p))` 会检查这一点，并由它自己清除标记。
    **绝对不要提前 `ClearPageReserved()`。**
    - 分支 S8b（无空闲区）：`free_begin_pa >= free_end_pa` → 不调用 `init_memmap()`
      （其内部 `assert(n > 0)` 会失败），直接返回。

### 后置条件（必须同时成立，全部可验证）
1. `va_pa_offset == PHYSICAL_MEMORY_OFFSET`
2. `get_memory_base() != 0 && get_memory_size() != 0`
3. `npage == PPN(mem_base + mem_size) && npage > nbase`
4. `pages != NULL && (uintptr_t)pages % PGSIZE == 0
    && (uintptr_t)pages >= ROUNDUP((uintptr_t)end, PGSIZE)`
5. `0 < nr_free_pages() <= npage - nbase`
6. `PageReserved(pa2page(PADDR((uintptr_t)end)))` —— 内核镜像尾页必须保留
7. `PageReserved(pa2page(PADDR((uintptr_t)pages)))` —— 描述符数组首页必须保留
8. `pa2page(page2pa(pa2page(PADDR((uintptr_t)end))))
    == pa2page(PADDR((uintptr_t)end))` —— 地址换算自洽
9. `default_pmm_manager.check()`（即 `default_check()`）不触发任何 assert：
   空闲链表满足 `sum(property) == nr_free`，且连续的 alloc / free / 相邻块合并测试全过
10. 空闲区与内核镜像、描述符数组**无任何重叠**
11. 物理内存末端不足一整页的残余部分**未**交给分配器
12. 默认环境（128 MiB、20 KiB 内核）下：`npage == 0x88000`，空闲页数 `== 31930`（该值与内核镜像大小相关；内核体积变化时以实际 `end` 为准）
13. 全程在 `-std=gnu99 -Wall -Werror` 下零警告

### 验证命令
```bash
make          # 必须零警告
make qemu     # 必须看到下面两行，然后是 check_alloc_page() succeeded!
```
```text
physical memory map:
  memory: 0x0000000008000000, [0x0000000080000000, 0x0000000087ffffff].
check_alloc_page() succeeded!
```

阶段提示：本阶段不要执行 `make grade`。它注入的 `test_best_fit()` 会引用模块二才有的
`best_fit_pmm_manager` 符号，此时链接必然失败，与本函数无关。
