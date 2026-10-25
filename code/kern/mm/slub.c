#include <slub.h>
#include <pmm.h>
#include <stdio.h>

/* lab2 模块二·Challenge（SLUB）—— 辛梓晗 2410621 */

/* ============================================================================
 * lab2 · 模块四（Challenge）：SLUB 两层分配器            组员：丙
 *
 * 一、两层架构
 * ------------
 * 第一层：全局 alloc_pages / free_pages。本模块只经这两个全局函数取页/还页，
 *         不引用任何具体 pmm_manager 实例，因此换页级管理器不影响本模块。
 * 第二层：8、16、…、4096 字节共 10 个 2 的幂档位。每个 slab 恰好 1 页，
 *         页内切成 PGSIZE / 档位大小 个等长对象，空闲对象用页内偏移串成单链。
 *         请求字节数向上取整到档位，一页内浪费不超过一个档位粒度。
 *
 * 二、大对象（用户硬裁定：必须真支持）
 * ------------------------------------
 * size > PGSIZE 时按 npages = ceil(size / PGSIZE) 向第一层整块申请连续页，
 * 返回该组页的起始地址，整段 size 字节（含首页开头）全部可写：
 * 归属信息一律不放进用户可写区域。
 *
 * 三、归属判定（只凭一个指针）
 * ----------------------------
 * 所有归属信息放在模块内的静态元数据表 slub_meta[] 中，按「页描述符下标」索引：
 *     kind == NONE     该页不归本模块（或已归还）
 *     kind == SLAB     该页是某档位的 slab；cache 记档位，freelist/nfree 记空闲链
 *     kind == LARGE    跨页大对象首页；npages 记页数
 *     kind == LARGE_IN 跨页大对象内部页；npages 记首页描述符下标
 * slub_free(ptr) 先把虚拟地址换算回物理页号，再查表：
 *     - SLAB      -> 要求 ptr 正好是对象起点，走对象回收；
 *     - LARGE/_IN -> 按整块页归还（落在对象内部页也归还整块）；
 *     - NONE      -> 无法归属或已释放过，panic。
 * 元数据不占用户可写区域，所以 8192 字节对象的最后一字节（偏移 8191）也一定能写。
 *
 * 四、为什么元数据表是静态的
 * --------------------------
 * 规格要求 slub_init 不得向第一层要页，因此元数据表只能用静态数组。
 * 数组按页描述符下标索引，上限 SLUB_MAX_PAGES = 32768 页
 * （QEMU 默认 128MiB 时 npage - nbase 正好是 0x8000 = 32768）。
 * 超过上限时 slub_init 会 panic 并提示调大该宏，不会静默越界。
 * 代价：.bss 增加 32768 * 8 = 256KiB，即约 64 个物理页（README/日志已说明）。
 *
 * 五、每档位只跟踪一个“可分配 slab”
 * ---------------------------------
 * 某档位的 slab 被取空后，本模块不再把它当作分配源；当该 slab 的对象全部
 * 释放（nfree == ntotal）时整页归还第一层。已满 slab 若先释放一个对象，
 * 会在该档位没有可分配 slab 时重新成为分配源。这套策略实现简单、可证明
 * “对象全释放后页必归还”，代价是非当前 slab 的零散空闲对象可能暂不复用。
 *
 * 六、非法输入
 * ------------
 * ptr == NULL 直接返回；落入未管理物理内存、不属于任何 slab/大对象、
 * 重复释放、slab 内非对象起点，一律 panic（见各函数内注释）。
 * ========================================================================== */

/* ---------------------------------------------------------------------------
 * 宏与静态状态
 * ------------------------------------------------------------------------- */

/* 档位：2^3 = 8 字节 ～ 2^12 = 4096 字节，共 10 档 */
#define SLUB_MIN_SHIFT      3
#define SLUB_MAX_SHIFT      12
#define SLUB_NR_CACHES      (SLUB_MAX_SHIFT - SLUB_MIN_SHIFT + 1)

/* 元数据表容量（页描述符下标上限），见文件头第四节 */
#define SLUB_MAX_PAGES      32768

/* 空闲链结束标记（页内偏移，页内最大偏移 4095，故 0xFFFF 不会与真实偏移冲突） */
#define SLUB_NIL            0xFFFFu

/* 元数据 kind 取值 */
#define SLUB_KIND_NONE      0
#define SLUB_KIND_SLAB      1
#define SLUB_KIND_LARGE     2
#define SLUB_KIND_LARGE_IN  3

/* 每个物理页 8 字节元数据：只记录“本模块当前持有”的页 */
typedef struct {
    uint16_t freelist;  /* SLAB: 空闲对象链头（页内偏移）；LARGE_IN: 首页描述符下标 */
    uint16_t nfree;     /* SLAB: 空闲对象个数 */
    uint16_t npages;    /* LARGE: 页数；其余不用 */
    uint8_t  cache;     /* SLAB: 档位下标 0..9；大对象填 0xFF */
    uint8_t  kind;      /* SLUB_KIND_* */
} slub_meta_t;

/* 编译期确认元数据恰好 8 字节，避免表膨胀或字段被填充字节挤乱 */
typedef char slub_meta_size_is_8[sizeof(slub_meta_t) == 8 ? 1 : -1];

static slub_meta_t slub_meta[SLUB_MAX_PAGES];

/* 每个档位当前的可分配 slab：存「描述符下标 + 1」，0 表示该档位当前没有可分配 slab */
static uint16_t slub_active[SLUB_NR_CACHES];

/* 当前从第一层持有的物理页总数（slab 每页 +1，大对象 +npages），用于 init 泄漏检查 */
static size_t slub_live_pages;

/* 测试用暂存区（本文件末尾）。容量按用例上限设定，见 slub_check 说明。 */
#define SLUB_TEST_MAX_PTRS  600

/* ---------------------------------------------------------------------------
 * 基础换算
 * ------------------------------------------------------------------------- */

/* 2 的幂档位：size <= 4096 时返回最小 cache 使 8 << cache >= size */
static size_t
slub_cache_of(size_t size) {
    size_t cache = 0;
    size_t cap = (size_t)1 << SLUB_MIN_SHIFT;
    while (cap < size) {
        cap <<= 1;
        cache ++;
    }
    return cache;
}

static size_t
slub_obj_size(size_t cache) {
    return (size_t)1 << (SLUB_MIN_SHIFT + cache);
}

static size_t
slub_objs_per_slab(size_t cache) {
    return PGSIZE / slub_obj_size(cache);
}

/* 描述符下标 -> 该页起始的可写虚拟地址（内核直映射） */
static void *
slub_page_va(size_t rel) {
    return (void *)(page2pa(pages + rel) + va_pa_offset);
}

/* 把指针换算成「页描述符下标 + 页内偏移」；不在可管理物理内存内返回 0 */
static int
slub_locate(void *ptr, size_t *rel, size_t *off) {
    uintptr_t pa = (uintptr_t)ptr - va_pa_offset;
    size_t pn = PPN(pa);

    if (pn < nbase || pn >= npage) {
        return 0;
    }
    *rel = pn - nbase;
    *off = (size_t)(pa & (PGSIZE - 1));
    if (*rel >= SLUB_MAX_PAGES) {
        panic("slub: page index %d beyond static metadata (raise SLUB_MAX_PAGES)",
              (int)*rel);
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * 第二层：slab 新建 / 对象分配 / 对象回收
 * ------------------------------------------------------------------------- */

/* 新建一个档位的 slab（1 页），成功返回 1 并写 *out_rel */
static int
slub_new_slab(size_t cache, size_t *out_rel) {
    struct Page *page;
    size_t rel, objsize, ntotal, i;
    char *base;

    page = alloc_pages(1);
    if (page == NULL) {
        return 0;               /* 第一层无页：失败但不改变任何缓存状态 */
    }
    rel = (size_t)(page - pages);
    if (rel >= SLUB_MAX_PAGES) {
        panic("slub: page index %d beyond static metadata (raise SLUB_MAX_PAGES)",
              (int)rel);
    }
    objsize = slub_obj_size(cache);
    ntotal = slub_objs_per_slab(cache);
    base = (char *)slub_page_va(rel);

    /* 空闲对象内嵌“下一个空闲对象的页内偏移”，串成单链 */
    for (i = 0; i < ntotal; i ++) {
        uint16_t next = (i + 1 < ntotal) ? (uint16_t)((i + 1) * objsize) : SLUB_NIL;
        *(uint16_t *)(base + i * objsize) = next;
    }

    slub_meta[rel].freelist = 0;
    slub_meta[rel].nfree = (uint16_t)ntotal;
    slub_meta[rel].npages = 0;
    slub_meta[rel].cache = (uint8_t)cache;
    slub_meta[rel].kind = SLUB_KIND_SLAB;
    slub_active[cache] = (uint16_t)(rel + 1);
    slub_live_pages += 1;
    *out_rel = rel;
    return 1;
}

/* size <= PGSIZE：走 2 的幂档位 */
static void *
slub_alloc_small(size_t size) {
    size_t cache = slub_cache_of(size);
    size_t rel;
    char *base;
    uint16_t off;

    if (slub_active[cache] == 0) {
        if (!slub_new_slab(cache, &rel)) {
            return NULL;
        }
    } else {
        rel = (size_t)(slub_active[cache] - 1);
    }
    if (slub_meta[rel].freelist == SLUB_NIL) {
        /* 防御：active 指向的 slab 按设计必有空闲对象 */
        return NULL;
    }
    base = (char *)slub_page_va(rel);
    off = slub_meta[rel].freelist;
    slub_meta[rel].freelist = *(uint16_t *)(base + off);
    slub_meta[rel].nfree --;
    if (slub_meta[rel].freelist == SLUB_NIL) {
        slub_active[cache] = 0;         /* 该 slab 已满，不再作为分配源 */
    }
    return base + off;
}

/* 回收一个 slab 对象；rel/off 来自 slub_locate */
static void
slub_free_slab(size_t rel, size_t off) {
    slub_meta_t *m = &slub_meta[rel];
    size_t cache = m->cache;
    size_t objsize, ntotal, walked;
    char *base;
    uint16_t cur;

    if (cache >= SLUB_NR_CACHES) {
        panic("slub_free: corrupt cache index %d at rel=%d", (int)cache, (int)rel);
    }
    objsize = slub_obj_size(cache);
    ntotal = slub_objs_per_slab(cache);
    if (off >= PGSIZE || (off % objsize) != 0) {
        panic("slub_free: pointer is not a slab object start (rel=%d off=%d objsize=%d)",
              (int)rel, (int)off, (int)objsize);
    }
    base = (char *)slub_page_va(rel);

    /* 走一遍空闲链：顺带查出重复释放，并校验链本身没有损坏 */
    walked = 0;
    cur = m->freelist;
    while (cur != SLUB_NIL) {
        if (cur >= PGSIZE || (cur % objsize) != 0) {
            panic("slub_free: corrupt freelist at rel=%d off=%d", (int)rel, (int)cur);
        }
        if ((size_t)cur == off) {
            panic("slub_free: double free (rel=%d off=%d)", (int)rel, (int)off);
        }
        cur = *(uint16_t *)(base + cur);
        walked ++;
        if (walked > ntotal) {
            panic("slub_free: freelist longer than slab (rel=%d)", (int)rel);
        }
    }
    if (walked != m->nfree) {
        panic("slub_free: nfree mismatch at rel=%d (list=%d counter=%d)",
              (int)rel, (int)walked, (int)m->nfree);
    }

    /* 放回链头 */
    *(uint16_t *)(base + off) = m->freelist;
    m->freelist = (uint16_t)off;
    m->nfree ++;

    if (m->nfree == (uint16_t)ntotal) {
        /* 整个 slab 空闲：清元数据后整页归还第一层 */
        m->kind = SLUB_KIND_NONE;
        m->freelist = SLUB_NIL;
        m->nfree = 0;
        m->npages = 0;
        m->cache = 0;
        if (slub_active[cache] == (uint16_t)(rel + 1)) {
            slub_active[cache] = 0;
        }
        slub_live_pages --;
        free_pages(pages + rel, 1);
    } else if (slub_active[cache] == 0) {
        /* 该档位此前没有可分配 slab：让这个重新出现空闲对象的 slab 上位 */
        slub_active[cache] = (uint16_t)(rel + 1);
    }
}

/* ---------------------------------------------------------------------------
 * 第一层直通：跨页大对象
 * ------------------------------------------------------------------------- */

/* size > PGSIZE：按 ceil(size/PGSIZE) 页整块申请，返回页组起始地址 */
static void *
slub_alloc_large(size_t size) {
    size_t npages = (size + PGSIZE - 1) / PGSIZE;
    struct Page *page;
    size_t rel, i;

    if (size > (size_t)SLUB_MAX_PAGES * PGSIZE) {
        return NULL;            /* 超过本模块能登记的页数上限 */
    }
    page = alloc_pages(npages);
    if (page == NULL) {
        return NULL;
    }
    rel = (size_t)(page - pages);
    if (rel + npages > SLUB_MAX_PAGES) {
        free_pages(page, npages);
        return NULL;
    }
    for (i = 0; i < npages; i ++) {
        slub_meta_t *m = &slub_meta[rel + i];
        m->freelist = SLUB_NIL;
        m->nfree = 0;
        m->cache = 0xFF;
        if (i == 0) {
            m->kind = SLUB_KIND_LARGE;
            m->npages = (uint16_t)npages;
        } else {
            m->kind = SLUB_KIND_LARGE_IN;
            m->npages = (uint16_t)rel;      /* 内部页记住首页下标 */
        }
    }
    slub_live_pages += npages;
    return slub_page_va(rel);
}

/* 归还跨页大对象（传首页或内部页都可以）：整块归还，页数与申请时一致 */
static void
slub_free_large(size_t rel) {
    size_t head, npages, i;

    if (slub_meta[rel].kind == SLUB_KIND_LARGE) {
        head = rel;
    } else {
        head = (size_t)slub_meta[rel].npages;
        if (head >= SLUB_MAX_PAGES || slub_meta[head].kind != SLUB_KIND_LARGE) {
            panic("slub_free: dangling large-object interior page (rel=%d)", (int)rel);
        }
    }
    npages = (size_t)slub_meta[head].npages;
    if (npages == 0 || head + npages > SLUB_MAX_PAGES) {
        panic("slub_free: corrupt large object page count at rel=%d", (int)head);
    }
    for (i = 0; i < npages; i ++) {
        slub_meta_t *m = &slub_meta[head + i];
        m->kind = SLUB_KIND_NONE;
        m->freelist = SLUB_NIL;
        m->nfree = 0;
        m->npages = 0;
        m->cache = 0;
    }
    slub_live_pages -= npages;
    free_pages(pages + head, npages);
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */

/* 初始化：清空各档位缓存。不向第一层要页，故 nr_free_pages() 不变。
 * 若仍有页被本模块持有（重复初始化会泄漏），显式 panic。 */
void
slub_init(void) {
    int i;

    if (npage - nbase > SLUB_MAX_PAGES) {
        panic("slub_init: %d managed pages exceed static metadata (%d)",
              (int)(npage - nbase), (int)SLUB_MAX_PAGES);
    }
    if (slub_live_pages != 0) {
        panic("slub_init: %d pages still held, re-init would leak",
              (int)slub_live_pages);
    }
    for (i = 0; i < SLUB_NR_CACHES; i ++) {
        slub_active[i] = 0;
    }
    for (i = 0; i < SLUB_MAX_PAGES; i ++) {
        if (slub_meta[i].kind != SLUB_KIND_NONE) {
            panic("slub_init: stale metadata at rel=%d", i);
        }
    }
}

void *
slub_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    if (size > PGSIZE) {
        return slub_alloc_large(size);
    }
    return slub_alloc_small(size);
}

void
slub_free(void *ptr) {
    size_t rel, off;
    uint8_t kind;

    if (ptr == NULL) {
        return;                 /* 规格：NULL 直接返回，不 panic */
    }
    if (!slub_locate(ptr, &rel, &off)) {
        panic("slub_free: pointer outside managed physical memory");
    }
    kind = slub_meta[rel].kind;
    if (kind == SLUB_KIND_SLAB) {
        slub_free_slab(rel, off);
        return;
    }
    if (kind == SLUB_KIND_LARGE || kind == SLUB_KIND_LARGE_IN) {
        slub_free_large(rel);
        return;
    }
    panic("slub_free: pointer not owned by slub or already freed (rel=%d)", (int)rel);
}

/* ---------------------------------------------------------------------------
 * 自测（对应 slub_prompt2）
 *
 * 用例表（编号 / 请求尺寸 / 覆盖行为 / 关键断言）
 *  1  0 与 NULL           size==0 返回 NULL；slub_free(NULL) 不 panic；页数不变
 *  2  8,16,…,4096         每个档位单个对象：8 字节对齐、新建 slab 恰好取 1 页、
 *                         按请求尺寸写满再校验、释放后页数精确复原
 *  3  8 / 64 / 4096       同档位多对象：地址互不重叠、可完整写入、全释放后复原
 *  4  64                  填满一个 slab（64 对象/页）→ 页数 +1；再分配一个 → +2 且
 *                         落在新页；同 slab 对象在同一页；全释放后复原
 *  5  4097 / 8192 / 20000 跨页大对象：页数增量满足 ceil(size/PGSIZE) ≤ Δ ≤ 2^n；
 *                         8192 在偏移 8191 处可写（抓“把归属信息塞进页首”的实现）；
 *                         释放后页数精确复原
 *  6  混合 8…20000        大小对象同时在场，按“小先/大先/交错”三种顺序释放，
 *                         每种顺序后页数都精确复原
 *  7  ——                  空闲态重复 slub_init()：幂等、页数不变
 *  8/9 ——                 末尾复核：无 slab 残留、无未归还页、总页数与入口一致
 *
 * 不写成运行期用例的非法输入（会主动 panic，规格明令禁止在测试里触发）：
 *  - 重复释放：空闲链里能查出该偏移，panic "double free"；
 *  - slab 内非对象起点：off % objsize != 0，panic "not a slab object start"；
 *  - 不属于本模块的指针：元数据 kind == NONE，panic "not owned by slub"；
 *  - 重复 slub_init 且有页未归还：slub_live_pages != 0，panic。
 * ------------------------------------------------------------------------- */

static void *slub_test_ptrs[SLUB_TEST_MAX_PTRS];

static void
slub_test_fail(int case_id, const char *msg, size_t detail) {
    cprintf("\n[slub_check] CASE %d FAILED: %s (detail=%d)\n",
            case_id, msg, (int)detail);
    panic("[slub_check] case %d failed: %s", case_id, msg);
}

#define SLUB_CK(id, cond, msg)                                              \
    do {                                                                    \
        if (!(cond)) {                                                      \
            slub_test_fail((id), (msg), 0);                                 \
        }                                                                   \
    } while (0)

#define SLUB_CK_N(id, cond, msg, n)                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            slub_test_fail((id), (msg), (size_t)(n));                       \
        }                                                                   \
    } while (0)

/* 按请求尺寸写入可辨识字节（不按档位尺寸写外圈，避免越界） */
static void
slub_test_fill(char *p, size_t size) {
    size_t i;
    for (i = 0; i < size; i ++) {
        p[i] = (char)((i * 7 + 3) & 0xFF);
    }
}

static void
slub_test_verify(char *p, size_t size) {
    size_t i;
    for (i = 0; i < size; i ++) {
        if ((unsigned char)p[i] != (unsigned char)((i * 7 + 3) & 0xFF)) {
            panic("[slub_check] payload corrupted at offset %d", (int)i);
        }
    }
}

/* 两个同尺寸对象是否重叠 */
static int
slub_test_overlap(const void *a, const void *b, size_t size) {
    uintptr_t x = (uintptr_t)a;
    uintptr_t y = (uintptr_t)b;
    return (x < y + size) && (y < x + size);
}

/* 第一层给出块的上界：2^n 页（buddy 按 2 的幂整块记账；first-fit 则正好是请求页数） */
static size_t
slub_test_block_pages(size_t npages) {
    size_t b = 1;
    while (b < npages) {
        b <<= 1;
    }
    return b;
}

/* ---- 用例 1：size == 0 / free(NULL) ---- */
static void
slub_test_zero(void) {
    size_t base = nr_free_pages();
    cprintf("[slub_check] case 1: size==0 and free(NULL) ... ");
    SLUB_CK(1, slub_alloc(0) == NULL, "alloc(0) should return NULL");
    slub_free(NULL);
    SLUB_CK(1, nr_free_pages() == base, "alloc(0)/free(NULL) changed page count");
    cprintf("PASS\n");
}

/* ---- 用例 2：每个档位单个对象往返 ---- */
static void
slub_test_one_per_cache(void) {
    size_t cache;
    cprintf("[slub_check] case 2: single object per cache (8..4096) ... ");
    for (cache = 0; cache < SLUB_NR_CACHES; cache ++) {
        size_t size = (size_t)1 << (SLUB_MIN_SHIFT + cache);
        size_t base = nr_free_pages();
        char *p = (char *)slub_alloc(size);

        SLUB_CK_N(2, p != NULL, "alloc failed", size);
        SLUB_CK_N(2, ((uintptr_t)p & 7) == 0, "pointer not 8-byte aligned", size);
        SLUB_CK_N(2, base - nr_free_pages() == 1,
                  "a fresh 1-page slab should take exactly 1 page", size);
        slub_test_fill(p, size);
        slub_test_verify(p, size);
        slub_free(p);
        SLUB_CK_N(2, nr_free_pages() == base, "pages not restored", size);
    }
    cprintf("PASS\n");
}

/* ---- 用例 3：同档位多对象互不重叠 ---- */
static void
slub_test_multi(void) {
    static const size_t caches[] = {0, 3, 9};   /* 8B, 64B, 4096B */
    size_t k;

    cprintf("[slub_check] case 3: several objects of one cache ... ");
    for (k = 0; k < sizeof(caches) / sizeof(caches[0]); k ++) {
        size_t cache = caches[k];
        size_t size = (size_t)1 << (SLUB_MIN_SHIFT + cache);
        size_t n = (cache == 9) ? 2 : 16;
        size_t base = nr_free_pages();
        size_t i, j;

        for (i = 0; i < n; i ++) {
            char *p = (char *)slub_alloc(size);
            SLUB_CK_N(3, p != NULL, "alloc failed", size);
            SLUB_CK_N(3, ((uintptr_t)p & 7) == 0, "not 8-byte aligned", size);
            for (j = 0; j < i; j ++) {
                SLUB_CK_N(3, !slub_test_overlap(p, slub_test_ptrs[j], size),
                          "overlapping objects returned", size);
            }
            slub_test_ptrs[i] = p;
            slub_test_fill(p, size);
        }
        for (i = 0; i < n; i ++) {
            slub_test_verify((char *)slub_test_ptrs[i], size);
        }
        for (i = 0; i < n; i ++) {
            slub_free(slub_test_ptrs[i]);
        }
        SLUB_CK_N(3, nr_free_pages() == base, "pages not restored", size);
    }
    cprintf("PASS\n");
}

/* ---- 用例 4：填满一个 slab，再产生第二个 slab ---- */
static void
slub_test_full_slab(void) {
    size_t cache = 3;                           /* 64B, 64 对象/页 */
    size_t size = (size_t)1 << (SLUB_MIN_SHIFT + cache);
    size_t ntotal = PGSIZE / size;
    size_t base, i;
    char *p;
    uintptr_t page0;

    cprintf("[slub_check] case 4: fill a slab then allocate a second one ... ");
    base = nr_free_pages();
    for (i = 0; i < ntotal; i ++) {
        p = (char *)slub_alloc(size);
        SLUB_CK_N(4, p != NULL, "alloc failed while filling slab", size);
        slub_test_ptrs[i] = p;
        slub_test_fill(p, size);
    }
    SLUB_CK_N(4, base - nr_free_pages() == 1, "first slab should take 1 page", size);

    page0 = (uintptr_t)slub_test_ptrs[0] & ~(uintptr_t)(PGSIZE - 1);
    SLUB_CK_N(4,
              page0 == ((uintptr_t)slub_test_ptrs[ntotal - 1] & ~(uintptr_t)(PGSIZE - 1)),
              "objects of one slab should live in the same page", size);

    p = (char *)slub_alloc(size);
    SLUB_CK_N(4, p != NULL, "alloc for the second slab failed", size);
    SLUB_CK_N(4, base - nr_free_pages() == 2, "second slab should add 1 page", size);
    SLUB_CK_N(4, ((uintptr_t)p & ~(uintptr_t)(PGSIZE - 1)) != page0,
              "second slab reused the full slab's page", size);
    slub_test_ptrs[ntotal] = p;
    slub_test_fill(p, size);

    for (i = 0; i <= ntotal; i ++) {
        slub_test_verify((char *)slub_test_ptrs[i], size);
    }
    for (i = 0; i <= ntotal; i ++) {
        slub_free(slub_test_ptrs[i]);
    }
    SLUB_CK_N(4, nr_free_pages() == base, "pages not restored", size);
    cprintf("PASS\n");
}

/* ---- 用例 5：跨页大对象 ---- */
static void
slub_test_large(void) {
    static const size_t sizes[] = {4097, 8192, 20000};
    size_t k;

    cprintf("[slub_check] case 5: large objects 4097/8192/20000 ... ");
    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k ++) {
        size_t size = sizes[k];
        size_t npages = (size + PGSIZE - 1) / PGSIZE;
        size_t upper = slub_test_block_pages(npages);
        size_t base = nr_free_pages();
        size_t delta;
        char *p = (char *)slub_alloc(size);

        SLUB_CK_N(5, p != NULL, "large alloc failed", size);
        SLUB_CK_N(5, ((uintptr_t)p & 7) == 0, "large pointer not 8-byte aligned", size);
        delta = base - nr_free_pages();
        SLUB_CK_N(5, delta >= npages, "took fewer pages than ceil(size/PGSIZE)", size);
        SLUB_CK_N(5, delta <= upper, "took more pages than a 2^n block", size);

        slub_test_fill(p, size);
        slub_test_verify(p, size);
        if (size == 8192) {
            /* 最后一字节必须可写：若实现把归属信息塞进页首/页尾就会失败 */
            p[size - 1] = 0x5A;
            SLUB_CK_N(5, (unsigned char)p[size - 1] == 0x5A,
                      "last byte of 8192-byte object not writable", size);
        }
        slub_free(p);
        SLUB_CK_N(5, nr_free_pages() == base, "large object pages not restored", size);
    }
    cprintf("PASS\n");
}

/* ---- 用例 6：大小对象混排 + 三种释放顺序 ---- */
static void
slub_test_mixed(void) {
    static const size_t sizes[] = {8, 4096, 4097, 64, 20000, 512, 8192, 16};
    size_t n = sizeof(sizes) / sizeof(sizes[0]);
    int round;

    cprintf("[slub_check] case 6: mixed small/large, three release orders ... ");
    for (round = 0; round < 3; round ++) {
        size_t base = nr_free_pages();
        size_t i;

        for (i = 0; i < n; i ++) {
            char *p = (char *)slub_alloc(sizes[i]);
            SLUB_CK_N(6, p != NULL, "mixed alloc failed", sizes[i]);
            SLUB_CK_N(6, ((uintptr_t)p & 7) == 0, "mixed pointer not aligned", sizes[i]);
            slub_test_ptrs[i] = p;
            slub_test_fill(p, sizes[i]);
        }
        for (i = 0; i < n; i ++) {
            slub_test_verify((char *)slub_test_ptrs[i], sizes[i]);
        }
        if (round == 0) {
            for (i = 0; i < n; i ++) {
                slub_free(slub_test_ptrs[i]);           /* 小在先 */
            }
        } else if (round == 1) {
            for (i = n; i > 0; i --) {
                slub_free(slub_test_ptrs[i - 1]);       /* 大在先 */
            }
        } else {
            for (i = 0; i < n / 2; i ++) {              /* 交错 */
                slub_free(slub_test_ptrs[i]);
                slub_free(slub_test_ptrs[n - 1 - i]);
            }
        }
        SLUB_CK_N(6, nr_free_pages() == base, "mixed pages not restored",
                  (size_t)round);
    }
    cprintf("PASS\n");
}

/* ---- 用例 7：空闲态重复初始化 ---- */
static void
slub_test_init_idempotent(void) {
    size_t base = nr_free_pages();
    cprintf("[slub_check] case 7: repeated slub_init() while idle ... ");
    slub_init();
    slub_init();
    SLUB_CK(7, nr_free_pages() == base, "slub_init changed free page count");
    cprintf("PASS\n");
}

/* ---- 入口自检 ---- */
void
slub_check(void) {
    size_t base = nr_free_pages();
    size_t i;

    cprintf("[slub_check] begin: nr_free=%d\n", (int)base);

    slub_test_zero();
    slub_test_one_per_cache();
    slub_test_multi();
    slub_test_full_slab();
    slub_test_large();
    slub_test_mixed();
    slub_test_init_idempotent();

    /* 用例 8：无残留（所有 slab 已整页归还、元数据清空） */
    SLUB_CK(8, slub_live_pages == 0, "live pages not released");
    for (i = 0; i < SLUB_MAX_PAGES; i ++) {
        if (slub_meta[i].kind != SLUB_KIND_NONE) {
            panic("[slub_check] case 8: residual slab metadata at rel=%d", (int)i);
        }
    }
    /* 用例 9：总页数回到入口基准 */
    SLUB_CK(9, nr_free_pages() == base, "final free page count differs from entry");

    cprintf("[slub_check] all cases PASS, nr_free=%d, no slab left\n",
            (int)nr_free_pages());
}

// lab2 模块二·Challenge（SLUB）—— 辛梓晗 2410621
