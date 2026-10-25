# 任务：实现 Best-Fit 物理内存分配器（kern/mm/best_fit_pmm.c）

## [PROMPT]

**任务**:参考kern/mm/default_pmm.c文件中的First-Fit实现,修改文件 kern/mm/best_fit_pmm.c,在文件中的注释'lab2 function2-2 YOUR CODE'下方和注释'Framework tests: students must not modify basic_check or best_fit_check.'的上方的位置,实现完整的Best-Fit物理内存管理模块,使这个新写的的模块可以替换当前使用的First-Fit物理内存管理器。同时在模块完成后还需要在kern/mm/pmm.c文件的'init_pmm_manager()'函数中，将当前物理内存管理器切换为'best_fit_pmm_manager'。

**操作要求**:你必须在项目中修改实际文件,而不是仅展示代码。在kern/mm/best_fit_pmm.c文件里已经有了basic_check()和best_fit_check()两个测试函数,这两个函数以及文件里有的注释和头文件包含不允许做任何改动,且这些头文件已经有的接口和结构体也不允许做任何改动。你需要自行设计Best-Fit分配器的内部状态和相关管理函数,并定义好'best_fit_pmm_manager'完成注册。

**输出要求**：按照我给的文件位置写入完整的实现代码，保留文件中原有的代码和测试函数，使用以下的'

## [RELY]

> 依赖按指导书要求「必须和工程代码一致——去代码里把真实的定义复制过来」组织：
> 以下全部为工程源码的逐字引用，我方说明一律以 `// [注]` 开头。

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

// [注] 来源：kern/mm/memlayout.h:41-54
/* Flags describing the status of a page frame */
#define PG_reserved                 0       // if this bit=1: the Page is reserved for kernel, cannot be used in alloc/free_pages; otherwise, this bit=0 
#define PG_property                 1       // if this bit=1: the Page is the head page of a free memory block(contains some continuous_addrress pages), and can be used in alloc_pages; if this bit=0: if the Page is the the head page of a free memory block, then this Page and the memory block is alloced. Or this Page isn't the head page.

#define SetPageReserved(page)       ((page)->flags |= (1UL << PG_reserved))
#define ClearPageReserved(page)     ((page)->flags &= ~(1UL << PG_reserved))
#define PageReserved(page)          (((page)->flags >> PG_reserved) & 1)
#define SetPageProperty(page)       ((page)->flags |= (1UL << PG_property))
#define ClearPageProperty(page)     ((page)->flags &= ~(1UL << PG_property))
#define PageProperty(page)          (((page)->flags >> PG_property) & 1)

// convert list entry to page
#define le2page(le, member)                 \
    to_struct((le), struct Page, member)

// [注] 来源：kern/mm/memlayout.h:56-60
/* free_area_t - maintains a doubly linked list to record free (unused) pages */
typedef struct {
    list_entry_t free_list;         // the list header
    unsigned int nr_free;           // number of free pages in this free list
} free_area_t;

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

// [注] 来源：kern/mm/pmm.h:15-32
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

```

### 可用函数

```c
// [注] 来源：kern/mm/pmm.h:78-81
extern struct Page *pages;
extern size_t npage;
extern const size_t nbase;
extern uint64_t va_pa_offset;

// [注] 来源：kern/mm/pmm.h:38-43
struct Page *alloc_pages(size_t n);
void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void); // number of free pages

#define alloc_page() alloc_pages(1)
#define free_page(page) free_pages(page, 1)

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

// [注] 来源：kern/mm/pmm.h:91-109
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

// [注] 来源：libs/list.h:23-34
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

// [注] 来源：kern/mm/default_pmm.c:17-43
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
    if (list_empty(&free_list)) {
        list_add(&free_list, &(base->page_link));
    } else {
        list_entry_t* le = &free_list;
        while ((le = list_next(le)) != &free_list) {
            struct Page* page = le2page(le, page_link);
            if (base < page) {
                list_add_before(le, &(base->page_link));
                break;
            } else if (list_next(le) == &free_list) {
                list_add(le, &(base->page_link));
            }
        }
    }
}

// [注] 来源：kern/mm/default_pmm.c:60-70
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

'、'

## [SPECIFICATION]

'的信息，且实现必须符合'basic_check()'、'best_fit_check()'函数所依赖的状态约定，可以通过这两个测试函数并可以正常运行。

[RELY]
'struct pmm_manager'('kern/mm/pmm.h'),其成员均为函数指针,Best-Fit需按此注册
```c
struct pmm_manager{
    const char *name;//管理器名字
    void (*init)(void);//初始化内部数据结构
    void(*init_memmap)(struct Page *base,size_t n);//依据初始空闲区建立管理结构
    struct Page *(*alloc_pages)(size_t n);//分配≥n个连续页
    void (*free_pages)(struct Page *base,size_t n);//释放以base为首页的连续n页
    size_t (*nr_free_pages)(void);//返回空闲页数目
    void (*check)(void);//正确性检查
};
```

已经由'kern/mm/memlayout.h'提供与First-Fit相同的空闲区维护结构,Best-Fit应使用同样的约定,否则框架的'basic_check()'和'best_fit_check()'函数无法正常编译。

```c
struct Page{
    int ref;
    uint64_t flags;//状态位
    unsigned int property;//空闲页大小
    list_entry_t page_link;//空闲链表节点
};

typedef struct{
    list_entry_t free_list;//空闲块链表头
    unsigned int nr_free;//空闲页总数
}free_area_t;

#define PG_property 1
#define SetPageProperty(page) ((page)->flags |= (1UL << PG_property))
#define ClearPageProperty(page) ((page)->flags &= ~(1UL << PG_property))
#define PageProperty(page) (((page)->flags >> PG_property) &1)
#define le2page(le,member) to_struct((le),struct Page,member)
```

链表接口('libs/list.h')有'list_init','list_add','list_add_before','list_del','list_empty','list_next','list_prev','to_struct'/'offsetof'('libs/defs.h')

页相关函数和全局变量('kern/mm/pmm.h','kern/mm/pmm.c'):
```c
struct Page *pages; //页描述符数组起始
size_t npage; //可管理物理页号上界
const size_t nbase; 
uint64_t va_pa_offset;
static inline int page_ref(struct Page*page);
static inline void set_page_ref(struct Page *page,int val);
static inline uint64_t page2pa(struct Page *page);
static inline struct Page *pa2page(uint64_t pa);
struct Page *alloc_pages(size_t n); //转发给pmm_manager->alloc_pages
void free_pages(struct Page *base,size_t n);
size_t nr_free_pages(void);
#define alloc_page() alloc_pages(1)
#define free_page(page) free_pages(page,1)
int cprintf(const char *fmt,...);
void panic(const char *fmt,...);
```

'kern/mm/default_pmm.c'文件给出了完整的First-Fit算法实现('default_init','default_init_memmap','default_alloc_pages','default_free_pages','default_nr_free_pages'和'default_pmm_manager'注册),在需要实现的Best-Fit算法中可以以此为基础改动分配策略,并遵守相同的空闲区状态约定

[GUARANTEE]
必须实现或修改的接口：
```c
static void best_init(void);
static void best_init_memmap(struct Page *base,size_t n);
static struct Page *best_alloc_pages(size_t n);
static void best_free_pages(struct Page *base,size_t n);
static size_t best_nr_free_pages(void);
const struct pmm_manager best_fit_pmm_manager={...}; //.name应为"best_fit_pmm_manager",.check指向best_fit_check()
```

未满注册时函数对'free_list','nr_free'的引用，必须要有
```c
static free_area_t free_area;
#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)
```

你可以根据需要添加：
- 辅助函数(建议设为static)
- 内部使用的宏定义
- 模块内部的静态变量

禁止改动'basic_check()'和'best_fit_check()'函数，禁止改动'struct pmm_manager'的成员签名，禁止新增跨文件的新接口。

[SPECIFICATION]
## best_init函数
**Pre-Condition**: 

**Post-Condition**: 

**Requirements**: 

## best_init_memmap函数
**Pre-Condition**: 

**Post-Condition**: 

  **Requirements**: 

## best_alloc_pages函数
**Pre-Condition**: 
- 'n>0'
- 'free_list','nr_free'已正确维护

**Post-Condition**: 

  **Case 1**:
  - 返回一个至少包含'n'个连续页的空闲块的起始页'page'
  - 若这个块大于'n'页，剩下'(page->property-n)'页构成新的空闲块
  - 当存在多个满足'property>=n'的空闲块时，选取其中'property'最小的那个

  **Case 2**:
  - 'n>nr_free'时或没有任何空闲块满足'property>=n'时返回'NULL'

  **Case 3**:
  - 'n==0'时断言失败

**Requirements**: 
- 若成功，则'page'的'PageProperty'被清除；从链表'free——list'摘除被分配的块；'nr_free-=n'
- 若失败，不改变'free_list'和'nr_free'的值
- 因为这次要实现的是Best-Fit而不是First-Fit,必须选择对请求大小最贴近的块,即所有'property>=n'中最小的那个

## best_free_pages函数
**Pre-Condition**: 

**Post-Condition**: 

  **Requirements**: 

## best_nr_free_pages函数
**Pre-Condition**: 

**Post-Condition**: 

  **Requirements**: 

## best_fit_pmm_manager注册
**Pre-Condition**: 

**Post-Condition**:

**Requirements**: 
对于Best-Fit的改进策略有以下几项：
1.换数据结构，进行大小分桶（或 Buddy），把分配/释放从 O(块数) 降到 O(log n) / O(1)，这是收益最大的一步；
2.补健壮性，每页分配状态位（或位图）+ 边界校验 + 并发保护，解决“释放中间页/部分释放”导致状态损坏的隐患；
3.加度量与压测，随机化 alloc/free 工作负载 + 不变量检查 + 碎片指标（块数、最大块、大连块请求成功率），用数据决定究竟该用 First-Fit、Best-Fit、Good-Fit 还是 Buddy——因为 Best-Fit 在长期碎片上并不具备理论优势。
