#include <pmm.h>
#include <list.h>
#include <best_fit_pmm.h>
#include <stdio.h>

//lab2 function2-2 2412243

// Best-Fit 管理器的内部状态。
// 与 First-Fit 使用完全相同的空闲区约定：只有空闲块的首页挂在 free_list 上，
// 首页的 property 记录块长度且 PageProperty 为 1；链表按物理页号升序排列。
// 每个 pmm_manager 各自维护一份空闲区，因此用 static 隔离。
static free_area_t free_area;

#define free_list (free_area.free_list)
#define nr_free (free_area.nr_free)

/* *
 * insert_free_block - 把一个空闲块（首页为 base）按物理页号升序插入空闲链表
 *
 * 保持链表有序有两个作用：
 * 1. best-fit 扫描时若遇到 property == n 的块可立即结束（已是最贴近的块）；
 * 2. free_pages 中 base 的前驱/后继就是物理相邻的块，可以 O(1) 判断并合并。
 * */
static void
insert_free_block(struct Page *base) {
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *page = le2page(le, page_link);
        if (base < page) {
            list_add_before(le, &(base->page_link));
            return;
        }
    }
    // base 的地址比链表中所有块都大，插入到表尾
    list_add_before(&free_list, &(base->page_link));
}

/* best_init - 初始化 Best-Fit 的内部数据结构 */
static void
best_init(void) {
    list_init(&free_list);
    nr_free = 0;
}

/* best_init_memmap - 把 [base, base + n) 这 n 个连续物理页注册成一个空闲块 */
static void
best_init_memmap(struct Page *base, size_t n) {
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
    insert_free_block(base);
}

/* *
 * best_alloc_pages - Best-Fit 分配 n 个连续物理页
 *
 * 在所有 property >= n 的空闲块中挑出 property 最小的那个（最贴近请求大小），
 * 从而尽量避免把大块切碎；若某块的 property 恰好等于 n，扫描可立即结束。
 * */
static struct Page *
best_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > nr_free) {
        return NULL;
    }
    struct Page *page = NULL;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= n &&
            (page == NULL || p->property < page->property)) {
            page = p;
            if (p->property == n) {
                break;  // 完美匹配，不可能有更小的块
            }
        }
    }
    if (page != NULL) {
        // 被选中的块从空闲链表摘下
        list_entry_t *prev = list_prev(&(page->page_link));
        list_del(&(page->page_link));
        if (page->property > n) {
            // 剩余部分构成新的空闲块，插回原位置以保持地址有序
            struct Page *p = page + n;
            p->property = page->property - n;
            SetPageProperty(p);
            list_add(prev, &(p->page_link));
        }
        page->property = n;
        ClearPageProperty(page);  // 被分配的块不再是空闲块首页
        nr_free -= n;
    }
    return page;
}

/* best_free_pages - 释放 [base, base + n) 这 n 个连续物理页并与相邻空闲块合并 */
static void
best_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p ++) {
        // 被 reserved 的页不能释放；PageProperty 为 1 说明重复释放或释放了空闲块首页
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }
    base->property = n;
    SetPageProperty(base);
    nr_free += n;
    insert_free_block(base);

    // 与物理相邻的前一个空闲块合并
    list_entry_t *le = list_prev(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (p + p->property == base) {
            p->property += base->property;
            ClearPageProperty(base);
            list_del(&(base->page_link));
            base = p;
        }
    }

    // 与物理相邻的后一个空闲块合并
    le = list_next(&(base->page_link));
    if (le != &free_list) {
        p = le2page(le, page_link);
        if (base + base->property == p) {
            base->property += p->property;
            ClearPageProperty(p);
            list_del(&(p->page_link));
        }
    }
}

/* best_nr_free_pages - 返回当前空闲物理页数 */
static size_t
best_nr_free_pages(void) {
    return nr_free;
}

// Framework tests: students must not modify basic_check or best_fit_check.
static void
basic_check(void) {
    struct Page *p0, *p1, *p2;
    p0 = p1 = p2 = NULL;
    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(p0 != p1 && p0 != p2 && p1 != p2);
    assert(page_ref(p0) == 0 && page_ref(p1) == 0 && page_ref(p2) == 0);

    assert(page2pa(p0) < npage * PGSIZE);
    assert(page2pa(p1) < npage * PGSIZE);
    assert(page2pa(p2) < npage * PGSIZE);

    list_entry_t free_list_store = free_list;
    list_init(&free_list);
    assert(list_empty(&free_list));

    unsigned int nr_free_store = nr_free;
    nr_free = 0;

    assert(alloc_page() == NULL);

    free_page(p0);
    free_page(p1);
    free_page(p2);
    assert(nr_free == 3);

    assert((p0 = alloc_page()) != NULL);
    assert((p1 = alloc_page()) != NULL);
    assert((p2 = alloc_page()) != NULL);

    assert(alloc_page() == NULL);

    free_page(p0);
    assert(!list_empty(&free_list));

    struct Page *p;
    assert((p = alloc_page()) == p0);
    assert(alloc_page() == NULL);

    assert(nr_free == 0);
    free_list = free_list_store;
    nr_free = nr_free_store;

    free_page(p);
    free_page(p1);
    free_page(p2);
}

static void
best_fit_check(void) {
    int score = 0, sumscore = 6;
    int count = 0, total = 0;
    list_entry_t *le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        assert(PageProperty(p));
        count++, total += p->property;
    }
    assert(total == nr_free_pages());

    basic_check();

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    struct Page *p0 = alloc_pages(5), *p1, *p2;
    assert(p0 != NULL);
    assert(!PageProperty(p0));

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    list_entry_t free_list_store = free_list;
    list_init(&free_list);
    assert(list_empty(&free_list));
    assert(alloc_page() == NULL);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    unsigned int nr_free_store = nr_free;
    nr_free = 0;

    // * - - * -
    free_pages(p0 + 1, 2);
    free_pages(p0 + 4, 1);
    assert(alloc_pages(4) == NULL);
    assert(PageProperty(p0 + 1) && p0[1].property == 2);
    // * - - * *
    assert((p1 = alloc_pages(1)) != NULL);
    assert(alloc_pages(2) != NULL);
    assert(p0 + 4 == p1);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    p2 = p0 + 1;
    free_pages(p0, 5);
    assert((p0 = alloc_pages(5)) != NULL);
    assert(alloc_page() == NULL);

    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
    assert(nr_free == 0);
    nr_free = nr_free_store;

    free_list = free_list_store;
    free_pages(p0, 5);

    le = &free_list;
    while ((le = list_next(le)) != &free_list) {
        struct Page *p = le2page(le, page_link);
        count--, total -= p->property;
    }
    assert(count == 0);
    assert(total == 0);
    #ifdef ucore_test
    score += 1;
    cprintf("grading: %d / %d points\n", score, sumscore);
    #endif
}

/* Best-Fit 物理内存管理器的注册表，pmm_init() 通过它接管物理页管理 */
const struct pmm_manager best_fit_pmm_manager = {
    .name = "best_fit_pmm_manager",
    .init = best_init,
    .init_memmap = best_init_memmap,
    .alloc_pages = best_alloc_pages,
    .free_pages = best_free_pages,
    .nr_free_pages = best_nr_free_pages,
    .check = best_fit_check,
};
