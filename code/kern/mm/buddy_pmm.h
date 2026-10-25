#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <pmm.h>

/*
 * lab2 · 模块三：Buddy System（伙伴系统）物理页分配器
 *
 * 对外只导出管理器实例本身，六个成员函数与自检函数全部 static。
 * 自检函数固定命名为 buddy_check（pmm.c 的挂载点按此名引用）。
 */
extern const struct pmm_manager buddy_pmm_manager;

#endif /* !__KERN_MM_BUDDY_PMM_H__ */
