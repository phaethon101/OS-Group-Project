<!-- 草稿 v2：四段齐备。按用户裁定：① 一并修正 pid 初值（0 → -1），diff 口径改为 2 删 2 增；② 保留 grep 复核条款；③ 保留 lsatp() 反例块 -->

[PROMPT]

**任务**：修改 `kern/process/proc.c` 的 `alloc_proc()`，修正 PCB 两个字段的初值：页目录基址 `proc->pgdir` 由 `0` 改为 `boot_pgdir_pa`，进程号 `proc->pid` 由 `0` 改为 `-1`。前者必须改，是因为 `satp` 只接受物理页号，内核切地址空间的标准写法（`kern/mm/pmm.c` 的 `enable_paging()`）是把页目录物理地址右移 12 位后填入；初值写 `0` 等于声称"根页表在物理页 0"，一旦模块三的 `proc_run()` 用这个值切 `satp`，切过去的第一条取指就失效；而当前全工程还没有读 `proc->pgdir` 的代码，这个错误现在不会暴露，只会以"首次切到 initproc 后静默死机"的形式出现。后者对应指导书"pid 先设为 -1，表示身份还未分配"的口径，0 保留给 idleproc，该初值随后会被 `do_fork` 与 `proc_init` 覆盖，属语义规范化。本任务只负责这两个字段，不负责模块三的调度实现，也不负责 lab2/lab3 代码移植。

**操作要求**：必须在项目中修改真实文件 `kern/process/proc.c`，而不是只输出代码片段；只改上述两个字段的初值，改完的 diff 必须恰好是"2 行删除 + 2 行新增"，且每对行只有右值不同；不得改动其它字段的初值、`memset`、`list_init`、函数签名、缩进风格与已有注释，两行 marker（`//LAB4 function2 YOUR CODE`、`//LAB4 function3 YOUR CODE`）及 `YOUR CODE` 字样须原样保留；不得改动 `kern/process/proc.c` 以外的任何文件（`proc.h`、`pmm.c`、`pmm.h`、`sched.c`、`switch.S`、`Makefile`、`CMakeLists.txt`、`tools/` 与工作区根目录的 `proc.txt`），也不得新增 `#include`；不得为了"能编译"做其它改动——本阶段 `function3` 区为空、lab2/lab3 代码尚未移植，整机构建必然失败，这是已知状态，所以不要运行 `make`、不要留下 `obj/`、`bin/`、不要加临时桩、不要顺手补 `function3`。

**输出要求**：直接进行文件操作；按下面 [RELY] 给出的结构体、宏与既有代码作答，以工程真实代码为准；改完报告改动所在文件、函数与行号，改前与改后各一行（各自独立代码块），说明为什么这里必须是物理地址 `boot_pgdir_pa` 而不能用 `boot_pgdir_va` 或 `PADDR(boot_pgdir_va)`（结合 `satp` 的 PPN 字段与 `PADDR` 的展开），并用 grep 统计全工程读 `proc->pgdir` 的位置如实列出，若与"尚无读者"的判断不一致须指出，但不得擅自扩大改动范围；最后附一句"除上述两行外未改动任何内容"。

[RELY]

// [注] 以下均为工程源码逐字原文；凡以 `// [注]` 开头的行是我方说明，不是源码。
// [注] 来源：kern/process/proc.h:42-58（PCB 定义；第 45 行是 `pid`、第 53 行是 `pgdir`，两者都是本次要修正的字段）
```c
struct proc_struct
{
    enum proc_state state;        // Process state
    int pid;                      // Process ID
    int runs;                     // the running times of Proces
    uintptr_t kstack;             // Process kernel stack
    volatile bool need_resched;   // bool value: need to be rescheduled to release CPU?
    struct proc_struct *parent;   // the parent process
    struct mm_struct *mm;         // Process's memory management field
    struct context context;       // Switch here to run process
    struct trapframe *tf;         // Trap frame for current interrupt
    uintptr_t pgdir;              // the base addr of Page Directroy Table(PDT)
    uint32_t flags;               // Process flag
    char name[PROC_NAME_LEN + 1]; // Process name
    list_entry_t list_link;       // Process link list
    list_entry_t hash_link;       // Process hash list
};
```

// [注] 来源：kern/process/proc.c:221-246（本次唯一允许修改的函数；第 233 行是 `proc->pid = 0;`，第 240 行是 `proc->pgdir = 0;`）
```c
//LAB4 function2 YOUR CODE

// alloc_proc - alloc a proc_struct and init all fields of proc_struct
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL)
    {
        // clear the whole PCB, so that every field starts from a known state
        memset(proc, 0, sizeof(struct proc_struct));
        proc->state = PROC_UNINIT;
        proc->pid = 0;
        proc->runs = 0;
        proc->kstack = 0;
        proc->need_resched = 0;
        proc->parent = NULL;
        proc->mm = NULL;
        proc->tf = NULL;
        proc->pgdir = 0;
        proc->flags = 0;
        list_init(&(proc->list_link));
        list_init(&(proc->hash_link));
    }
    return proc;
}
```

// [注] 来源：kern/mm/pmm.h:30-33（正确取值的来源；`boot_pgdir_pa` 已对外声明，proc.c 无需新增 include）
```c
extern const struct pmm_manager *pmm_manager;
extern pde_t *boot_pgdir_va;
extern const size_t nbase;
extern uintptr_t boot_pgdir_pa;
```

// [注] 来源：kern/mm/pmm.h:59-67（PADDR：内核虚拟地址换算成物理地址，多行反斜杠宏逐字保留）
```c
#define PADDR(kva)                                                 \
    ({                                                             \
        uintptr_t __m_kva = (uintptr_t)(kva);                      \
        if (__m_kva < KERNBASE)                                    \
        {                                                          \
            panic("PADDR called with invalid kva %08lx", __m_kva); \
        }                                                          \
        __m_kva - va_pa_offset;                                    \
    })
```

// [注] 来源：kern/mm/pmm.c:154-157（`boot_pgdir_pa` 的产生处：它是启动页目录的物理地址）
```c
    // create boot_pgdir, an initial page directory(Page Directory Table, PDT)
    extern char boot_page_table_sv39[];
    boot_pgdir_va = (pte_t *)boot_page_table_sv39;
    boot_pgdir_pa = PADDR(boot_pgdir_va);
```

// [注] 来源：kern/mm/pmm.c:92-95（`satp` 的正确写法：页目录物理地址右移 12 位后填入；本修复必须与这个口径一致）
```c
static void enable_paging(void)
{
    write_csr(satp, 0x8000000000000000 | (boot_pgdir_pa >> RISCV_PGSHIFT));
}
```

// [注] 来源：libs/riscv.h:203-204（`satp` 的页号粒度）
```c
#define RISCV_PGSHIFT 12
#define RISCV_PGSIZE (1 << RISCV_PGSHIFT)
```

// [注] 来源：libs/riscv.h:244-248（反例：`lsatp()` 是 Sv32 遗留，模式位与参数类型都不适配 Sv39，本工程严禁用它写 `satp`）
```c
static inline void
lsatp(unsigned int pgdir)
{
  write_csr(satp, SATP32_MODE | (pgdir >> RISCV_PGSHIFT));
}
```

[GUARANTEE]

唯一允许的语义改动是两处初始化语句的右值：

- `proc->pgdir`：由 `0` 改为 `boot_pgdir_pa`；
- `proc->pid`：由 `0` 改为 `-1`。

必须保持不变的接口事实（任一项被改动即视为交付失败）：

- `alloc_proc` 的函数签名、`static` 可见性、返回值语义（成功返回 PCB 指针，`kmalloc` 失败返回 `NULL`）；
- `struct proc_struct` 的字段集合与顺序（`proc.h` 一个字节都不许动）；
- 其余字段的初值：`state = PROC_UNINIT`、`runs = 0`、`kstack = 0`、`need_resched = 0`、`parent = NULL`、`mm = NULL`、`tf = NULL`、`flags = 0`，`context` 全 0（由 `memset` 保证），`list_link` 与 `hash_link` 已 `list_init`；
- 两行 marker 注释 `//LAB4 function2 YOUR CODE`、`//LAB4 function3 YOUR CODE` 原样保留，`YOUR CODE` 字样不得改写（学号替换由我方在提交前统一处理）；
- 文件编码与换行风格（纯 ASCII、LF）不变，不得把整份文件改成 CRLF 或重排缩进。

允许增加的：无。本修复不需要新增函数、宏、全局变量、`#include` 或注释。

明确禁止：

- 改动 `kern/process/proc.c` 之外的任何文件（含 `proc.h`、`pmm.c`、`pmm.h`、`sched.c`、`switch.S`、`Makefile`、`CMakeLists.txt`、`tools/` 下全部文件、工作区根目录的 `proc.txt`）；
- 改动 `memset`、`list_init`、`context` 的清零方式，或其它字段的初值；
- 把 `proc->pid` 改成调用 `get_pid()` 或任何动态取值（本轮只改字面量初值）；
- 补写 `//LAB4 function3 YOUR CODE` 区、补写 lab2/lab3 代码、添加临时桩、为了让 `make` 通过而做任何"顺手修复"；
- 新增 `#include`（`<pmm.h>` 已在第 5 行）、新增跨文件导出符号、重排代码顺序；
- 运行 `make` / `make grade` / `make qemu`，或在工程内留下 `obj/`、`bin/` 等构建产物。

[SPECIFICATION]

## alloc_proc

**Pre-Condition**：

- `pmm_init()` 已执行完毕，`boot_pgdir_pa` 已由 `boot_pgdir_pa = PADDR(boot_pgdir_va)` 赋值为启动页目录（Sv39 根页表）的物理地址；
- `kmalloc()` 可用；
- 调用者尚未把该 PCB 挂入 `proc_list` 或 `hash_list`，该 PCB 也不被任何调度路径引用。

**Post-Condition**：

- 返回的 PCB 满足 `proc->pgdir == boot_pgdir_pa`，该值是物理地址：`proc->pgdir >> RISCV_PGSHIFT` 可直接作为 `satp` 的 PPN 字段使用，与 `enable_paging()` 的口径一致；
- 返回的 PCB 满足 `proc->pid == -1`，表示该 PCB 的身份尚未分配；`0` 保留给 `idleproc`，不得占用；
- 其余字段取值保持不变（见 [GUARANTEE] 的字段清单）；
- 失败路径不变：`kmalloc` 返回 `NULL` 时函数返回 `NULL`，不做部分初始化。

**Requirements**：

1. 只修改 `proc->pgdir` 与 `proc->pid` 两条初始化语句的右值，不得增加、删除或重排任何语句；
2. `pgdir` 的右值必须引用 `kern/mm/pmm.h` 中已 `extern` 的 `boot_pgdir_pa`；不得使用 `boot_pgdir_va`、`PADDR(boot_pgdir_va)`、`PADDR(boot_pgdir_va) >> RISCV_PGSHIFT`、任何字面量地址或新引入的辅助符号；
3. `pid` 的右值必须是十进制字面量 `-1`，不得写成 `0xFFFFFFFF`、`~0`、`get_pid()` 或任何等价变形；
4. 不得改变 `alloc_proc` 的调用时机与调用者（`proc_init`、`do_fork` 中的调用语句一行都不许动）；
5. 修改后文件的 diff 必须恰好是"2 行删除 + 2 行新增"，且每对行只有右值不同。

## 非目标（本任务明确不做）

- 不实现功能模块三（`cpu_idle`、`proc_run`、`schedule`、`switch_to`）；
- 不移植 lab2 / lab3 代码，不补齐 `page_init`、`default_pmm_manager`、`RESTORE_ALL`；
- 不修 `do_fork` 忽略 `stack` 参数、`copy_mm` 内的 `assert(current->mm == NULL)` 这两处技术债（留待 lab5）；
- 不追求本阶段"能编译"。

## 验收口径

- **静态验收（本阶段唯一可执行）**：`kern/process/proc.c` 与改动前相比 diff 恰为 2 行替换；`proc.h`、`pmm.c`、`pmm.h`、`sched.c`、`switch.S`、`Makefile`、`tools/` 全部零改动；两行 marker 与 `YOUR CODE` 字样仍在；`grep -n "proc->pgdir = 0"` 与 `grep -n "proc->pid = 0"` 均无命中（`proc_init` 里的 `idleproc->pid = 0` 属正常赋值，不在检查范围）。
- **语义验收（待模块三落地后）**：`proc_run()` 用 `proc->pgdir` 生成的 `satp` 页号等于 `boot_pgdir_pa >> RISCV_PGSHIFT`，从 `idleproc` 首次切到 `initproc` 不再死机，`make grade` 的 `[Test Schedule] PASS` 可打印。
- **不得**以"编译通过"作为验收依据（本阶段工程不完整，构建失败属预期）。
