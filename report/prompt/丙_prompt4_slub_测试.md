# slub_prompt2_生成测试 —— 为 SLUB 实现设计自检函数

## [PROMPT]

### 1.1 任务详情

**修改位置**：只在 `kern/mm/slub.c` 内新增测试代码，实现 `void slub_check(void)`。该函数已在 `slub.h` 声明、并已被 `pmm.c` 的守卫挂载点引用，不要新增文件、不要改声明。

**功能目标**：为两层分配器设计一套开机自动执行的自检，覆盖全部档位、跨页大对象、大小对象混排、整页归还与非法释放判定；不满足预期即 `panic`，线索能定位到用例、档位与页号。

**验收表现**：`make qemu DEFS=-DLAB2_SLUB_ATTACH` 先打印 `check_alloc_page() succeeded!`（页级管理器自检），**随后**打印 slub 测试进度行；默认编译不含任何 slub 调用路径（未被引用时 `slub.o` 会被 `ld --gc-sections` 整体丢弃）。

### 1.2 操作约束

**可改**：`slub.c` 内新增 `static` 用例函数、辅助函数、静态暂存数组、模块内宏。

**不可改（逐条，附理由）**：
- 不得新建 `kern/test/`、不得出现 `test_lab2.h`：`tools/inject.py:140-144` 检测到即退出，评分失败。
- 不改 `pmm.c`：守卫挂载点已由第一份 prompt 建立，再补调用等于在开机流程里重复执行，且会动到注入脚本要求的锚点。
- 不改 `default_pmm.c`、`best_fit_pmm.c`、`tools/`：属于他人分工与评分脚本。
- 不改 `slub.h` 的四个导出声明：对外接口已冻结。

**环境约束**：交叉编译 `-std=gnu99 -nostdinc -fno-builtin -Wall -Werror`；禁用 stdlib、系统头、浮点；打印只能用 `cprintf`。

**安全约束**：测试路径不得主动触发 `panic`——`__panic` 末尾是死循环，会把开机流程和评分超时一起拖死。非法指针类验证改为在用例表里逐条列出判定依据，不写成运行期用例。

### 1.3 输出验证要求

**交付物**：①用例表；②完整测试代码；③执行顺序与状态复原说明。

**如何证明正确**：全部对象释放后 `nr_free_pages()` 是否精确回到 `slub_init` 之后的基准；请求 4097、8192、20000 字节时各向第一层取了几页、释放后页数是否复原；档位边界 8、4096、4097 分别走哪条路径；同一档位内并发分配到的对象地址是否互不重叠。

**交付格式**：先用例表（编号、请求尺寸、覆盖行为、断言、为何有效），再代码，最后执行顺序与复原论证。

## [RELY]

```c
// [注] 本段全部为 ucore 既有源码的逐字引用（行号见每条来源）；我方文字一律以 // [注] 开头。
// [注] ============ 宏定义 ============
// [注] 来源：kern/mm/mmu.h:9-10
#define PGSIZE          4096                    // bytes mapped by a page
#define PGSHIFT         12                      // log2(PGSIZE)

// [注] ============ 数据结构 ============
// [注] 本模块新增文件（非 ucore 既有源码），来源：kern/mm/slub.h:14-17
void  slub_init(void);          /* 初始化缓存；不向第一层要页 */
void *slub_alloc(size_t size);  /* 失败或 size==0 返回 NULL */
void  slub_free(void *ptr);     /* NULL 直接返回；无法归属/重复释放 panic */
void  slub_check(void);         /* 自检，挂在 pmm.c 的守卫挂载点 */

// [注] ============ 可用函数 ============
// [注] 来源：kern/mm/pmm.h:40
size_t nr_free_pages(void); // number of free pages

// [注] 来源：kern/mm/pmm.c:118-132（pmm_init 的初始化顺序：建管理器 → page_init → check_alloc_page）
/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();
// [注] 挂载点：在 check_alloc_page(); 之后加 #ifdef LAB2_SLUB_ATTACH / slub_init(); / slub_check(); / #endif（三行，默认关闭）。

// [注] 来源：kern/mm/pmm.c:149-152
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

**对外接口**：不新增任何导出符号。`slub_check` 保持 `void slub_check(void)` 签名不变，测试辅助全部 `static`。

**核心函数（内部必须存在，名称可自定）**：
1. 入口自检函数 `slub_check`：记录进入时的 `nr_free_pages()` 作为基准，编排各组用例，末尾校验复原并打印结论。
2. 档位覆盖组：对 8 至 4096 的每个 2 的幂档位做分配与释放往返。
3. 跨页大对象组：覆盖 4097、8192、20000 字节，核对占用页数与释放后页数复原。
4. 混排与归还组：大小对象交错分配后按不同顺序释放，核对最终页数复原、且无 slab 残留。

**显式授权**：可自行添加 `static` 辅助函数（尺寸转档位、判 2 的幂、页数差断言、进度打印）、模块内宏、模块内静态数组（暂存已分配指针，容量需自行设上限）。

**显式禁止**：不得导出任何全局符号；不得修改 `slub_init` / `slub_alloc` / `slub_free` 的正常路径行为；不得为测试放宽实现里的合法性检查。

## [SPECIFICATION]

### 1. 入口自检函数 `slub_check`

- **前置状态**：由守卫挂载点在初始化流程末尾调用；此时无业务分配，系统处于全空闲初始态。
- **后置效果**：全部用例通过后，各档位无 slab 残留，`nr_free_pages()` 与进入时完全一致；打印最终结论行。
- **分支场景**：首次进入 → 记录基准、依次执行各用例组、打印进度；任一断言失败 → `panic`，信息中含用例编号与档位；用例中途返回 → 必须先释放已借出的对象。
- **对应要求**：不得依赖调用方的额外状态；不得假设内存总量；整体运行时间必须短（开机流程内完成）。

### 2. 档位覆盖组

- **前置状态**：入口已记录基准页数。
- **后置效果**：每个档位往返结束后页数与基准相同。
- **分支场景**：单次分配再释放 → 页数复原；同档位连续分配多个对象 → 返回地址互不重叠且均可完整写入；某一档位分配至其 slab 满 → 自动产生新 slab，页数增加；该档位对象全部释放 → slab 整页归还，页数复原。
- **对应要求**：必须覆盖档位边界 8 与 4096；写入验证不得越界（按请求尺寸写，不按档位尺寸写外圈）。

### 3. 跨页大对象组

- **前置状态**：入口已记录基准页数。
- **后置效果**：每个尺寸分配后页数减少量与其占用页数一致，释放后精确复原。
- **分支场景**：请求 4097 字节 → 占用 2 页；请求 8192 字节 → 占用 2 页，且在偏移 8191 处写入后释放仍正确（这一条能抓出把归属信息塞进页首、吃掉用户可用字节的实现）；请求 20000 字节 → 占用 5 页；释放后页数回到基准。
- **对应要求**：占用页数以 `nr_free_pages()` 的差值为准核对，不得假设实现细节；大对象与小对象必须能同时存在。

### 4. 混排与归还组

- **前置状态**：入口已记录基准页数；暂存数组容量已按用例上限设定。
- **后置效果**：全部释放后页数精确回到基准，且不存在残留 slab。
- **分支场景**：先分配多个小对象再分配大对象，然后按"先小后大"释放 → 页数复原；按"先大后小"释放 → 页数复原；交错释放 → 页数复原；释放顺序打乱且包含重复档位 → 页数复原。
- **对应要求**：必须包含至少一次"大小对象同时在场"的释放序列，否则测不出大对象与小对象的归属判定是否互相干扰；不得在用例中重新调用 `slub_init` 来"清理现场"。

## [符号一致性自查]

- `[SPECIFICATION]` 中出现的符号均有出处：`PGSIZE`、`nr_free_pages`、`panic`、`assert`、`cprintf` 见 `[RELY]`；`slub_init`、`slub_alloc`、`slub_free`、`slub_check` 见 `[RELY]` 的接口与被测对象；各用例组函数、档位换算辅助、静态暂存数组属 `[GUARANTEE]` 显式授权自造。
