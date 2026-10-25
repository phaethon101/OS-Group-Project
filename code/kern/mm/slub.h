#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <pmm.h>

/*
 * lab2 · 模块四（Challenge）：SLUB 两层分配器
 *
 * 第一层：全局 alloc_pages / free_pages（经 pmm_manager 派发，不绑定具体管理器）。
 * 第二层：8..4096 字节的 2 的幂档位对象缓存；超过一页的请求直接走页级大对象。
 *
 * 对外只导出以下四个函数，测试与内部结构全部 static。
 */
void  slub_init(void);          /* 初始化缓存；不向第一层要页 */
void *slub_alloc(size_t size);  /* 失败或 size==0 返回 NULL */
void  slub_free(void *ptr);     /* NULL 直接返回；无法归属/重复释放 panic */
void  slub_check(void);         /* 自检，挂在 pmm.c 的守卫挂载点 */

#endif /* !__KERN_MM_SLUB_H__ */
