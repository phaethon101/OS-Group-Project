#include <buddy_pmm.h>
#include <list.h>
#include <pmm.h>
#include <stdio.h>

/* lab2 模块二·任务三（Buddy System）—— 辛梓晗 2410621 */

/* ============================================================================
 * lab2 · 模块三：Buddy System（伙伴系统）物理页分配器       组员：丙
 *
 * 设计要点
 * --------
 * 1. 可管理页按 2 的幂（阶 order）组织：order k 的块含 2^k 个连续物理页，
 *    块首页号必须按 2^k 对齐。order 取值 0..BUDDY_MAX_ORDER。
 * 2. 每个阶维护一条空闲块双向链表，链表节点借用 struct Page 的 page_link；
 *    空闲块首页 PageProperty=1 且 property=块页数，块内其余页字段全零。
 * 3. 分配：请求页数向上取整到阶 -> 从该阶或更高阶摘一个块 -> 逐层对半分裂，
 *    拆出的后半块进入低一阶的空闲链表。空闲页计数按「整块页数」记账，
 *    故 alloc_pages(5) 会让 nr_free 减少 8 而不是 5。
 * 4. 释放：按请求页数还原出块阶 -> 置块首为空闲块头 -> 与同阶空闲伙伴逐层合并。
 *    合并前先判定伙伴块是否整体落在可管理区间内，越界一侧立即停止合并，
 *    绝不解引用区间外的页描述符。
 * 5. 伙伴关系用「页描述符下标异或块页数」计算：
 *        buddy_rel = (base - pages) ^ block_pages
 *    nbase = 0x80000 = 2^19，BUDDY_MAX_ORDER = 15 < 19，
 *    故按描述符下标异或与按物理页号异或完全等价。
 * 6. 自测代码写在文件末尾（static），由 buddy_pmm_manager.check 指向 buddy_check，
 *    开机由 check_alloc_page() 调用一次；LAB2_BUDDY_ATTACH 挂载点再次调用时
 *    只做只读不变量校验，不重复执行破坏性用例。
 * ========================================================================== */

/* ---------------------------------------------------------------------------
 * 宏与模块内静态状态
 * ------------------------------------------------------------------------- */

/* order 上限取 15：2^15 = 32768 页 = 128MiB，覆盖 QEMU 默认内存。
 * property 是 unsigned int，2^15 不会溢出；size_t 也足够表示整块页数。 */
#define BUDDY_MAX_ORDER     15
#define BUDDY_NR_ORDERS     (BUDDY_MAX_ORDER + 1)
#define BUDDY_MAX_BLOCK     ((size_t)1 << BUDDY_MAX_ORDER)

/* 分阶空闲链表：buddy_free_list[k] 只挂 order k 的空闲块首页 */
static list_entry_t buddy_free_list[BUDDY_NR_ORDERS];

/* 当前空闲物理页总数，恒等于各阶空闲块页数之和 */
static size_t buddy_nr_free;

/* ---------------------------------------------------------------------------
 * 阶运算与伙伴定位
 * ------------------------------------------------------------------------- */

/* 求最小的 order 使 2^order >= n（n > 0，调用方保证） */
static size_t
buddy_order_of(size_t n) {
    size_t order = 0;
    while (((size_t)1 << order) < n) {
        order ++;
    }
    return order;
}

/* 求 base 在 order 阶（块页数 block_pages）下的伙伴块首。
 * 伙伴整体越出可管理区间、或 base 未按块大小对齐时返回 NULL。 */
static struct Page *
buddy_buddy_of(struct Page *base, size_t block_pages) {
    size_t nmanage = npage - nbase;
    size_t rel;

    if (base < pages || base >= pages + nmanage) {
        return NULL;
    }
    rel = (size_t)(base - pages);
    if ((rel & (block_pages - 1)) != 0) {
        return NULL;        /* 未对齐的块不存在伙伴 */
    }
    rel ^= block_pages;
    if (rel + block_pages > nmanage) {
        return NULL;        /* 伙伴块越出可管理区间 */
    }
    return pages + rel;
}

/* 把 page 标记为 order 阶空闲块头，并向上逐层与空闲伙伴合并。
 * 返回合并后的块首（地址较小者），*out_order 返回合并后的阶。
 * 调用前 page 不得已在任何空闲链表中。 */
static struct Page *
buddy_merge_free_block(struct Page *page, size_t order, size_t *out_order) {
    page->property = (size_t)1 << order;
    SetPageProperty(page);
    set_page_ref(page, 0);

    while (order < BUDDY_MAX_ORDER) {
        size_t block = (size_t)1 << order;
        struct Page *buddy = buddy_buddy_of(page, block);

        if (buddy == NULL) {
            break;
        }
        /* 伙伴必须恰好是同阶空闲块头：被占用、或属于更大空闲块都不合并 */
        if (!PageProperty(buddy) || buddy->property != block || page_ref(buddy) != 0) {
            break;
        }
        list_del(&(buddy->page_link));

        if (buddy < page) {
            /* 伙伴在下方：块头下移到伙伴，原块首降为块内页 */
            ClearPageProperty(page);
            page->property = 0;
            page = buddy;
        } else {
            /* 伙伴在上方：块头不变，吸收伙伴 */
            ClearPageProperty(buddy);
            buddy->property = 0;
            set_page_ref(buddy, 0);
        }
        order ++;
        page->property = (size_t)1 << order;
    }
    *out_order = order;
    return page;
}

/* ---------------------------------------------------------------------------
 * pmm_manager 六个成员
 * ------------------------------------------------------------------------- */

/* 初始化：清空分阶空闲链表，计数归零。不读写任何 struct Page，可重复调用。 */
static void
buddy_init(void) {
    int order;
    for (order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_init(&buddy_free_list[order]);
    }
    buddy_nr_free = 0;
}

/* 交付一段物理页：清字段 -> 贪心分解成若干按页号对齐的 2 的幂块 -> 入链表。
 * 可被多次调用累积生效；与已交付区间相邻的块会通过合并自动长成大块。 */
static void
buddy_init_memmap(struct Page *base, size_t n) {
    size_t nmanage = npage - nbase;
    size_t done = 0;
    struct Page *p;

    if (n == 0) {
        panic("buddy_init_memmap: n == 0");
    }
    if (base == NULL || base < pages || base + n > pages + nmanage) {
        panic("buddy_init_memmap: range out of managed pages");
    }
    for (p = base; p != base + n; p ++) {
        assert(PageReserved(p));
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }

    while (done < n) {
        struct Page *blk = base + done;
        size_t rel = (size_t)(blk - pages);
        size_t remain = n - done;
        size_t order = 0;
        size_t new_order;
        struct Page *head;

        /* 取「起点对齐 + 长度允许」的最大 2 的幂块，保证不丢弃任何页 */
        while (order < BUDDY_MAX_ORDER) {
            size_t next = (size_t)1 << (order + 1);
            if (next > remain) {
                break;
            }
            if ((rel & (next - 1)) != 0) {
                break;
            }
            order ++;
        }
        head = buddy_merge_free_block(blk, order, &new_order);
        list_add(&buddy_free_list[new_order], &(head->page_link));
        buddy_nr_free += (size_t)1 << order;
        done += (size_t)1 << order;
    }
}

/* 分配连续 n 页：向上取整到阶，从该阶或更高阶摘块后逐层分裂。
 * 失败路径（n 超过最大阶 / 无可用块）不改变任何状态。 */
static struct Page *
buddy_alloc_pages(size_t n) {
    size_t order, cur, block;
    struct Page *page, *buddy, *p;

    if (n == 0) {
        panic("buddy_alloc_pages: n == 0");
    }
    if (n > BUDDY_MAX_BLOCK) {
        return NULL;
    }
    order = buddy_order_of(n);
    cur = order;
    while (cur <= BUDDY_MAX_ORDER && list_empty(&buddy_free_list[cur])) {
        cur ++;
    }
    if (cur > BUDDY_MAX_ORDER) {
        return NULL;
    }

    page = le2page(list_next(&buddy_free_list[cur]), page_link);
    list_del(&(page->page_link));
    ClearPageProperty(page);

    /* 逐层对半分裂：保留下半块继续拆，拆出的后半块进入低一阶链表 */
    while (cur > order) {
        cur --;
        block = (size_t)1 << cur;
        buddy = page + block;
        buddy->property = block;
        SetPageProperty(buddy);
        set_page_ref(buddy, 0);
        list_add(&buddy_free_list[cur], &(buddy->page_link));
        page->property = block;
    }

    /* 返回块：块首 PageProperty=0、property=整块页数、ref=1；
     * 块内其余页 property/PageProperty 清零、ref=0。 */
    block = (size_t)1 << order;
    page->property = block;
    ClearPageProperty(page);
    set_page_ref(page, 1);
    for (p = page + 1; p != page + block; p ++) {
        p->property = 0;
        ClearPageProperty(p);
        set_page_ref(p, 0);
    }
    buddy_nr_free -= block;
    return page;
}

/* 释放一次分配得到的块：校验 -> 置块首为空闲块头 -> 逐层合并 -> 入链表。
 * 非法输入（重复释放、非块首、保留页、区间外、n == 0）一律 panic；
 * 注意 n 只需与分配时同阶即可（分配 n 页实际得到 2^k 页的整块）。 */
static void
buddy_free_pages(struct Page *base, size_t n) {
    size_t nmanage = npage - nbase;
    size_t order, block, new_order;
    struct Page *p, *head;

    if (n == 0) {
        panic("buddy_free_pages: n == 0");
    }
    if (n > BUDDY_MAX_BLOCK) {
        panic("buddy_free_pages: n exceeds max block");
    }
    order = buddy_order_of(n);
    block = (size_t)1 << order;

    if (base == NULL || base < pages || base + block > pages + nmanage) {
        panic("buddy_free_pages: base out of managed pages");
    }
    if (PageReserved(base)) {
        panic("buddy_free_pages: page is reserved");
    }
    if (PageProperty(base)) {
        panic("buddy_free_pages: page is already free (double free)");
    }
    /* 已分配块首的 property 记录整块页数：块内非首页 property 为 0，
     * 阶与分配时不一致时这里也会对不上，一并拦住。 */
    if (base->property != block) {
        panic("buddy_free_pages: n mismatched with allocation or not block head");
    }
    if (((size_t)(base - pages) & (block - 1)) != 0) {
        panic("buddy_free_pages: block not aligned");
    }

    /* 归还前把块内所有页的残留字段清干净（块首稍后重新置位） */
    for (p = base + 1; p != base + block; p ++) {
        p->property = 0;
        ClearPageProperty(p);
        set_page_ref(p, 0);
    }

    buddy_nr_free += block;
    head = buddy_merge_free_block(base, order, &new_order);
    list_add(&buddy_free_list[new_order], &(head->page_link));
}

/* 空闲页计数：按整块记账，恒等于各阶空闲块页数之和 */
static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

/* ---------------------------------------------------------------------------
 * 自测（对应 buddy_prompt2：为 Buddy 实现设计自检函数）
 *
 * 用例表（编号 / 覆盖行为 / 关键断言）
 *  1  单页分配释放幂等          每次申请后计数 -1、释放后复原；块首字段合法
 *  2  非 2 的幂请求向上取整      alloc_pages(3) 得 4 页块：4 页对齐、property=4、
 *                                计数按整块 -4、块内其余页字段全零
 *  3  分裂后剩余块可再分配       连续 8 次单页申请互不重复且都在可管理区间内
 *  4  合并的上下两种方向         ①先放上方伙伴：下方仍占用 -> property 必须仍为 1（不得合并）
 *                                ②先放下方伙伴：上方仍占用 -> property 必须仍为 1
 *                                两者释放后上方页必须变成块内页（PageProperty=0）
 *  5  超限申请零副作用           alloc_pages(2^15+1)、alloc_pages(npage) 均返回 NULL
 *  6  耗尽 -> 两端边界 -> 复原   贪心从高阶到低阶分配直到 NULL（计数 0、再申请仍 NULL）；
 *                                最高块释放时伙伴越界不得合并，且能被原样重新分配；
 *                                最低块释放时下方伙伴不在可交付区间内，不得越界合并；
 *                                全部释放后计数复原、不变量（含「已完全合并」）成立
 *  另外每个用例前后都执行只读不变量校验，校验内容见 buddy_test_invariants()。
 * ------------------------------------------------------------------------- */

/* 耗尽用例的临时暂存区；真实内存最多 32768 页，贪心整块分配只需十余块，
 * 取 256 足够，同时避免在 .bss 里放大数组影响空闲页数量。 */
#define BUDDY_TEST_MAX_HELD   256
/* 合并用例一次申请的单页个数，用于构造相邻（伙伴）页对 */
#define BUDDY_TEST_BATCH      12

/* buddy_check 是否已执行过：挂载点会第二次调用 check，此时只做只读校验 */
static int buddy_check_calls;

/* 用例失败：先打印用例编号与页号，再 panic（panic 本身会死循环） */
static void
buddy_test_fail(int case_id, const char *msg, struct Page *p) {
    if (p != NULL) {
        cprintf("\n[buddy_check] CASE %d FAILED: %s; page ppn=0x%lx\n",
                case_id, msg, (unsigned long)page2ppn(p));
    } else {
        cprintf("\n[buddy_check] CASE %d FAILED: %s\n", case_id, msg);
    }
    panic("[buddy_check] case %d failed: %s", case_id, msg);
}

#define BUDDY_CHECK(id, cond, msg)                                          \
    do {                                                                    \
        if (!(cond)) {                                                      \
            buddy_test_fail((id), (msg), NULL);                             \
        }                                                                   \
    } while (0)

#define BUDDY_CHECK_P(id, cond, msg, p)                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            buddy_test_fail((id), (msg), (p));                              \
        }                                                                   \
    } while (0)

/* 只读不变量校验：
 *  - 每个空闲块整体落在 [pages, pages + nmanage) 内，且块首按块大小对齐；
 *  - 块首 PageProperty=1、property=块页数、ref=0；
 *  - 块内其余页 property=PageProperty=0、ref=0；
 *  - 不存在「两个同阶空闲伙伴都没合并」的情况（空闲结构已完全合并）；
 *  - 各阶空闲块页数之和 == 计数 == nr_free_pages()。
 * 不写任何字段，任何时刻都可调用。 */
static void
buddy_test_invariants(void) {
    size_t nmanage = npage - nbase;
    size_t total = 0;
    int order;

    for (order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        size_t block = (size_t)1 << order;
        list_entry_t *head = &buddy_free_list[order];
        list_entry_t *le = head;

        while ((le = list_next(le)) != head) {
            struct Page *p = le2page(le, page_link);
            struct Page *buddy;
            size_t i;

            if (p < pages || p + block > pages + nmanage) {
                panic("buddy invariant: order %d block out of range (ppn=0x%lx)",
                      order, (unsigned long)page2ppn(p));
            }
            if (!PageProperty(p) || p->property != block || page_ref(p) != 0) {
                panic("buddy invariant: order %d head fields mismatch (ppn=0x%lx)",
                      order, (unsigned long)page2ppn(p));
            }
            if (((size_t)(p - pages) & (block - 1)) != 0) {
                panic("buddy invariant: order %d block not aligned (ppn=0x%lx)",
                      order, (unsigned long)page2ppn(p));
            }
            for (i = 1; i < block; i ++) {
                struct Page *q = p + i;
                if (PageProperty(q) || q->property != 0 || page_ref(q) != 0) {
                    panic("buddy invariant: order %d interior page dirty (ppn=0x%lx)",
                          order, (unsigned long)page2ppn(q));
                }
            }
            /* 已完全合并：同阶空闲伙伴不得单独存在 */
            buddy = buddy_buddy_of(p, block);
            if (buddy != NULL && PageProperty(buddy) && buddy->property == block) {
                panic("buddy invariant: order %d block not coalesced (ppn=0x%lx)",
                      order, (unsigned long)page2ppn(p));
            }
            total += block;
        }
    }
    if (total != buddy_nr_free || buddy_nr_free != buddy_nr_free_pages()) {
        panic("buddy invariant: nr_free mismatch (list_sum=%d, counter=%d)",
              (int)total, (int)buddy_nr_free);
    }
}

/* 入口自检函数：编排用例、保证状态复原；重复调用只做只读校验 */
static void
buddy_check(void) {
    size_t base_count;
    struct Page *p;
    int i;

    buddy_check_calls ++;
    if (buddy_check_calls > 1) {
        cprintf("[buddy_check] call #%d: already executed, read-only verify ... ",
                buddy_check_calls);
        buddy_test_invariants();
        cprintf("PASS\n");
        return;
    }

    cprintf("[buddy_check] begin: nr_free=%d\n", (int)nr_free_pages());
    buddy_test_invariants();

    /* ---- 用例 1：单页分配 / 释放幂等 ---- */
    cprintf("[buddy_check] case 1: single page alloc/free x4 ... ");
    base_count = nr_free_pages();
    for (i = 0; i < 4; i ++) {
        p = alloc_pages(1);
        BUDDY_CHECK_P(1, p != NULL, "alloc_page returned NULL", NULL);
        BUDDY_CHECK_P(1, !PageProperty(p), "allocated head still PageProperty", p);
        BUDDY_CHECK_P(1, page_ref(p) == 1, "allocated head ref != 1", p);
        BUDDY_CHECK_P(1, p->property == 1, "allocated head property != 1", p);
        BUDDY_CHECK(1, nr_free_pages() == base_count - 1, "nr_free not decreased by 1");
        free_pages(p, 1);
        BUDDY_CHECK(1, nr_free_pages() == base_count, "nr_free not restored");
        buddy_test_invariants();
    }
    cprintf("PASS\n");

    /* ---- 用例 2：非 2 的幂请求向上取整到整块 ---- */
    cprintf("[buddy_check] case 2: request 3 pages -> 4-page aligned block ... ");
    base_count = nr_free_pages();
    p = alloc_pages(3);
    BUDDY_CHECK_P(2, p != NULL, "alloc_pages(3) returned NULL", NULL);
    BUDDY_CHECK_P(2, p->property == 4, "block size != 4", p);
    BUDDY_CHECK_P(2, !PageProperty(p), "head is PageProperty", p);
    BUDDY_CHECK_P(2, page_ref(p) == 1, "head ref != 1", p);
    BUDDY_CHECK_P(2, ((size_t)(p - pages) & 3) == 0, "head not 4-page aligned", p);
    BUDDY_CHECK(2, nr_free_pages() + 4 == base_count, "nr_free did not drop by whole block");
    for (i = 1; i < 4; i ++) {
        BUDDY_CHECK_P(2, !PageProperty(p + i) && (p + i)->property == 0 &&
                             page_ref(p + i) == 0, "interior page of block dirty", p + i);
    }
    free_pages(p, 3);
    BUDDY_CHECK(2, nr_free_pages() == base_count, "nr_free not restored");
    buddy_test_invariants();
    cprintf("PASS\n");

    /* ---- 用例 3：分裂出的剩余块仍可分配，多页申请互不重叠 ---- */
    cprintf("[buddy_check] case 3: split remainder reusable, pages distinct ... ");
    base_count = nr_free_pages();
    {
        struct Page *batch[8];
        int j;

        for (i = 0; i < 8; i ++) {
            batch[i] = alloc_pages(1);
            BUDDY_CHECK_P(3, batch[i] != NULL, "alloc_page returned NULL", NULL);
        }
        for (i = 0; i < 8; i ++) {
            BUDDY_CHECK_P(3, batch[i] >= pages && batch[i] < pages + (npage - nbase),
                          "allocated page out of range", batch[i]);
            for (j = i + 1; j < 8; j ++) {
                BUDDY_CHECK_P(3, batch[i] != batch[j], "duplicate page returned", batch[i]);
            }
        }
        BUDDY_CHECK(3, nr_free_pages() + 8 == base_count, "nr_free not decreased by 8");
        for (i = 0; i < 8; i ++) {
            free_pages(batch[i], 1);
        }
        BUDDY_CHECK(3, nr_free_pages() == base_count, "nr_free not restored");
    }
    buddy_test_invariants();
    cprintf("PASS\n");

    /* ---- 用例 4：合并的两个方向 + 伙伴被占用时不得合并 ---- */
    cprintf("[buddy_check] case 4: coalesce above/below, no merge when busy ... ");
    base_count = nr_free_pages();
    {
        struct Page *batch[BUDDY_TEST_BATCH];
        int freed[BUDDY_TEST_BATCH];
        int lo1 = -1, hi1 = -1, lo2 = -1, hi2 = -1;
        int a, b;

        for (i = 0; i < BUDDY_TEST_BATCH; i ++) {
            batch[i] = alloc_pages(1);
            freed[i] = 0;
            BUDDY_CHECK_P(4, batch[i] != NULL, "alloc_page returned NULL", NULL);
        }
        /* 在单页批次里找两对「相邻且低位页按 2 页对齐」的伙伴 */
        for (a = 0; a < BUDDY_TEST_BATCH; a ++) {
            for (b = 0; b < BUDDY_TEST_BATCH; b ++) {
                if (batch[b] != batch[a] + 1) {
                    continue;
                }
                if (((size_t)(batch[a] - pages) & 1) != 0) {
                    continue;
                }
                if (lo1 < 0) {
                    lo1 = a;
                    hi1 = b;
                } else if ((lo2 < 0) && a != lo1 && a != hi1 && b != lo1 && b != hi1) {
                    lo2 = a;
                    hi2 = b;
                }
            }
        }
        BUDDY_CHECK(4, lo1 >= 0, "no adjacent buddy pair found in 12 single-page allocations");
        BUDDY_CHECK(4, lo2 >= 0, "only one adjacent buddy pair found");

        /* 变体 A：先释放上方伙伴，下方仍被占用 -> 不得合并 */
        free_pages(batch[hi1], 1);
        BUDDY_CHECK_P(4, PageProperty(batch[hi1]) && batch[hi1]->property == 1,
                      "merged while lower buddy still allocated", batch[hi1]);
        BUDDY_CHECK(4, nr_free_pages() == base_count - BUDDY_TEST_BATCH + 1,
                    "nr_free after freeing upper buddy (variant A)");
        free_pages(batch[lo1], 1);
        BUDDY_CHECK_P(4, !PageProperty(batch[hi1]),
                      "upper block not absorbed after both freed (variant A)", batch[hi1]);
        freed[lo1] = freed[hi1] = 1;

        /* 变体 B：先释放下方伙伴，上方仍被占用 -> 不得合并；再放上方 -> 头取较小者 */
        free_pages(batch[lo2], 1);
        BUDDY_CHECK_P(4, PageProperty(batch[lo2]) && batch[lo2]->property == 1,
                      "merged while upper buddy still allocated", batch[lo2]);
        free_pages(batch[hi2], 1);
        BUDDY_CHECK_P(4, !PageProperty(batch[hi2]),
                      "upper block not absorbed after both freed (variant B)", batch[hi2]);
        freed[lo2] = freed[hi2] = 1;

        for (i = 0; i < BUDDY_TEST_BATCH; i ++) {
            if (!freed[i]) {
                free_pages(batch[i], 1);
            }
        }
        BUDDY_CHECK(4, nr_free_pages() == base_count, "nr_free not restored");
    }
    buddy_test_invariants();
    cprintf("PASS\n");

    /* ---- 用例 5：超限申请返回 NULL 且零副作用 ---- */
    cprintf("[buddy_check] case 5: oversize request has no side effect ... ");
    base_count = nr_free_pages();
    p = alloc_pages(BUDDY_MAX_BLOCK + 1);
    BUDDY_CHECK(5, p == NULL, "alloc_pages(max_block + 1) should return NULL");
    BUDDY_CHECK(5, nr_free_pages() == base_count, "oversize request changed nr_free");
    p = alloc_pages(npage);
    BUDDY_CHECK(5, p == NULL, "alloc_pages(npage) should return NULL");
    BUDDY_CHECK(5, nr_free_pages() == base_count, "alloc_pages(npage) changed nr_free");
    cprintf("PASS\n");

    /* ---- 用例 6：耗尽 -> 两端边界 -> 全部释放复原 ---- */
    cprintf("[buddy_check] case 6: exhaust, both ends, then restore ... ");
    base_count = nr_free_pages();
    {
        struct Page *held[BUDDY_TEST_MAX_HELD];
        size_t held_order[BUDDY_TEST_MAX_HELD];
        size_t nheld = 0;
        int top = -1, low = -1;
        int k;

        /* 从高阶到低阶整块贪心分配，直到彻底耗尽（不产生分裂，块数极少） */
        for (k = BUDDY_MAX_ORDER; k >= 0; k --) {
            size_t req = (size_t)1 << k;
            while (nheld < BUDDY_TEST_MAX_HELD) {
                p = alloc_pages(req);
                if (p == NULL) {
                    break;
                }
                held[nheld] = p;
                held_order[nheld] = (size_t)k;
                nheld ++;
            }
        }
        BUDDY_CHECK(6, nheld < BUDDY_TEST_MAX_HELD, "test storage overflow while exhausting");
        BUDDY_CHECK(6, nheld > 0, "no block allocated while exhausting");
        BUDDY_CHECK(6, nr_free_pages() == 0, "memory not exhausted");
        BUDDY_CHECK(6, alloc_page() == NULL, "alloc_page succeeded after exhaustion");
        BUDDY_CHECK(6, nr_free_pages() == 0, "failed allocation changed nr_free");

        /* 上端：最高块的上方伙伴越出可管理区间，不得合并，且可原样重新分配 */
        for (i = 0; i < (int)nheld; i ++) {
            if (top < 0 || page2ppn(held[i]) > page2ppn(held[top])) {
                top = i;
            }
        }
        p = held[top];
        {
            size_t block = (size_t)1 << held_order[top];
            free_pages(p, block);
            BUDDY_CHECK_P(6, PageProperty(p) && p->property == block,
                          "top block merged beyond managed range", p);
            BUDDY_CHECK(6, nr_free_pages() == block, "nr_free after freeing top block");
            BUDDY_CHECK_P(6, alloc_pages(block) == p, "top block not reusable", p);
            held[top] = p;
        }

        /* 下端：最低块的伙伴若在下方，则落在已交付区间之外（保留页），不得越界合并；
         * 伙伴在上方时属于已交付区间，允许合法向上合并。这里不假设首块的阶。 */
        for (i = 0; i < (int)nheld; i ++) {
            if (low < 0 || page2ppn(held[i]) < page2ppn(held[low])) {
                low = i;
            }
        }
        p = held[low];
        {
            size_t low_block = (size_t)1 << held_order[low];
            size_t low_rel = (size_t)(p - pages);

            free_pages(p, low_block);
            if ((low_rel ^ low_block) < low_rel) {
                BUDDY_CHECK_P(6, PageProperty(p) && p->property == low_block,
                              "lowest block merged below delivered range", p);
            } else {
                BUDDY_CHECK_P(6, PageProperty(p) && p->property >= 2 * low_block,
                              "lowest block lost after free", p);
            }
            BUDDY_CHECK(6, nr_free_pages() == low_block,
                        "nr_free after freeing lowest block");
        }
        held[low] = NULL;

        /* 逆序全部归还 */
        for (i = (int)nheld - 1; i >= 0; i --) {
            if (held[i] != NULL) {
                free_pages(held[i], (size_t)1 << held_order[i]);
            }
        }
        BUDDY_CHECK(6, nr_free_pages() == base_count, "nr_free not restored after exhaustion test");
        buddy_test_invariants();
    }
    cprintf("PASS\n");

    BUDDY_CHECK(7, nr_free_pages() == base_count, "final nr_free changed");
    buddy_test_invariants();
    cprintf("[buddy_check] all cases PASS, nr_free=%d, layout restored\n",
            (int)nr_free_pages());
}

/* ---------------------------------------------------------------------------
 * 对外唯一导出符号
 * ------------------------------------------------------------------------- */

const struct pmm_manager buddy_pmm_manager = {
    .name          = "buddy_pmm_manager",
    .init          = buddy_init,
    .init_memmap   = buddy_init_memmap,
    .alloc_pages   = buddy_alloc_pages,
    .free_pages    = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check         = buddy_check,
};

// lab2 模块二·任务三（Buddy System）—— 辛梓晗 2410621
