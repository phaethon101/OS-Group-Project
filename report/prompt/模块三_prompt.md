<!-- 草稿 v2：四段齐备。按用户裁定：① 四个函数一份 prompt 全做；② [RELY] 14 块全留；③ schedule 的三条选择规则保留细写；④ 验收口径改为"只在 marker 之下新增"；⑤ cpu_idle 不加屏障。另：switch.S 允许在 marker 之上补一行 #include <riscv.h> -->

[PROMPT]

**任务**：实现 lab4 功能模块三「进程调度」的四个函数：`kern/process/proc.c` 的 `cpu_idle()` 与 `proc_run()`（写在文件末尾 `//LAB4 function3 YOUR CODE` 之下）、`kern/schedule/sched.c` 的 `schedule()`（写在同一行 marker 之下）、`kern/process/switch.S` 的 `switch_to()`（该文件目前只有一行 marker，函数体与 `.globl` 都要自己写）。四者串成一条链：`cpu_idle` 发现 `need_resched` 置位就调 `schedule`；`schedule` 在 `proc_list` 里挑一个 `PROC_RUNNABLE` 的线程并调 `proc_run`；`proc_run` 更新 `current`、把 `satp` 切到该线程的页目录，再用 `switch_to` 保存旧上下文、恢复新上下文。新线程第一次被切进来时 `context.ra` 是 `forkret`，于是经 `forkret → forkrets → __trapret` 恢复它的 trapframe 并 `sret` 到 `kernel_thread_entry`，从线程入口函数开始执行。

**操作要求**：必须在项目中修改这三个真实文件，而不是只输出代码片段；只在上述三处 marker 之下新增代码，不得改动 `wakeup_proc`、`forkret`、`init_main`、`proc_init` 等既有函数，不得改动 marker 与 `YOUR CODE` 字样；`switch.S` 只有一行 marker，允许在它之上补一行 `#include <riscv.h>`（为了用 `STORE`/`LOAD`/`REGBYTES`），除此之外 marker 之上零改动；`schedule` 按 FIFO 轮转实现——当前线程不是 `idleproc` 时从它的下一个开始找 `PROC_RUNNABLE`，是 `idleproc` 时从表头开始找，整表找不到就退回 `idleproc`，进入时先把 `current->need_resched` 清零；`proc_run` 必须在 `local_intr_save` / `local_intr_restore` 临界区内完成 `current` 更新、`satp` 切换和 `switch_to` 调用，切 `satp` 用 `proc->pgdir`，口径与 `kern/mm/pmm.c` 的 `enable_paging()` 一致，`libs/riscv.h` 的 `lsatp()` 是 Sv32 遗留、严禁使用；`cpu_idle` 是无返回值死循环，`need_resched` 为真时调 `schedule()`，不要加屏障；`switch_to` 按 `struct context` 的字段顺序保存 `from`、恢复 `to`，两个形参分别经 `a0`、`a1` 传入；不得改动这三个文件以外的任何文件；不得为了"能编译"做其它改动——本阶段 `page_init`、`default_pmm`、`RESTORE_ALL`、`page_insert` 都还没补齐，整机构建必然失败，这是已知状态，所以不要运行 `make`、不要留下 `obj/`、`bin/`、不要加临时桩、不要顺手补别的模块。

**输出要求**：直接进行文件操作；按下面 [RELY] 给出的结构体、宏与既有代码作答；改完报告四个函数各自的实现要点——`switch_to` 的保存/恢复顺序如何对应 `struct context`、`schedule` 的三条选择规则、`proc_run` 的 `satp` 口径与临界区范围、`cpu_idle` 的循环条件；再报告改动所在的三个文件与各自行号，最后附一句"除三个文件的三处 marker 区外未改动任何内容"。

[RELY]

// [注] 以下均为工程源码逐字原文；凡以 `// [注]` 开头的行是我方说明，不是源码。
// [注] 来源：kern/process/proc.h:18-34（`switch_to` 的保存/恢复顺序必须严格按这个字段序，共 14 个）
```c
struct context
{
    uintptr_t ra;
    uintptr_t sp;
    uintptr_t s0;
    uintptr_t s1;
    uintptr_t s2;
    uintptr_t s3;
    uintptr_t s4;
    uintptr_t s5;
    uintptr_t s6;
    uintptr_t s7;
    uintptr_t s8;
    uintptr_t s9;
    uintptr_t s10;
    uintptr_t s11;
};
```

// [注] 来源：kern/process/proc.h:42-58（`schedule` 与 `proc_run` 要读的 PCB 字段：state、runs、need_resched、context、pgdir、list_link）
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

// [注] 来源：kern/process/proc.h:60-63（`le2proc` 用于从链表节点反查 PCB；三个全局指针即 `schedule` 的选择范围）
```c
#define le2proc(le, member) \
    to_struct((le), struct proc_struct, member)

extern struct proc_struct *idleproc, *initproc, *current;
```

// [注] 来源：kern/process/proc.h:10-16（进程状态枚举；`schedule` 只挑 `PROC_RUNNABLE`）
```c
enum proc_state
{
    PROC_UNINIT = 0, // uninitialized
    PROC_SLEEPING,   // sleeping
    PROC_RUNNABLE,   // runnable(maybe running)
    PROC_ZOMBIE,     // almost dead, and wait parent proc to reclaim his resource
};
```

// [注] 来源：kern/process/proc.c:78-82（既有声明：`switch_to` 的签名、首次运行链的两个符号）
```c
void kernel_thread_entry(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);
```

// [注] 来源：kern/process/proc.c:145-152（新线程第一次被切进来后落脚的 `forkret`；不要改它）
```c
// forkret -- the first kernel entry point of a new thread/process
// NOTE: the addr of forkret is setted in copy_thread function
//       after switch_to, the current proc will execute here.
static void
forkret(void)
{
    forkrets(current->tf);
}
```

// [注] 来源：kern/process/proc.c:403-407（`proc.c` 的末尾：`cpu_idle` 与 `proc_run` 要插在这行 marker 之下）
```c
    initproc = find_proc(pid);
    set_proc_name(initproc, "init");
}

//LAB4 function3 YOUR CODE
```

// [注] 来源：kern/schedule/sched.c:1-13 全文（`schedule()` 要插在这行 marker 之下；`wakeup_proc` 是既有函数，不得改动）
```c
#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

//LAB4 function3 YOUR CODE
```

// [注] 来源：kern/process/switch.S 全文（整份文件只有这一行；`switch_to` 的汇编与 `.globl` 都要自己写）
```asm
//LAB4 function3 YOUR CODE
```

// [注] 来源：kern/sync/sync.h 全文（`proc_run` 的临界区靠这两个宏）
```c
#ifndef __KERN_SYNC_SYNC_H__
#define __KERN_SYNC_SYNC_H__

#include <defs.h>
#include <intr.h>
#include <riscv.h>

static inline bool __intr_save(void) {
    if (read_csr(sstatus) & SSTATUS_SIE) {
        intr_disable();
        return 1;
    }
    return 0;
}

static inline void __intr_restore(bool flag) {
    if (flag) {
        intr_enable();
    }
}

#define local_intr_save(x) \
    do {                   \
        x = __intr_save(); \
    } while (0)
#define local_intr_restore(x) __intr_restore(x);

#endif /* !__KERN_SYNC_SYNC_H__ */
```

// [注] 来源：kern/mm/pmm.c:92-95（切换 `satp` 的口径：页目录物理地址右移 12 位后填入）
```c
static void enable_paging(void)
{
    write_csr(satp, 0x8000000000000000 | (boot_pgdir_pa >> RISCV_PGSHIFT));
}
```

// [注] 来源：kern/mm/pmm.h:164-167（切完 `satp` 后的 TLB 失效手段；本项目线程共用同一页表，调用与否不影响判定）
```c
static inline void flush_tlb()
{
    asm volatile("sfence.vma");
}
```

// [注] 来源：libs/riscv.h:203-204（`satp` 的页号粒度）
```c
#define RISCV_PGSHIFT 12
#define RISCV_PGSIZE (1 << RISCV_PGSHIFT)
```

// [注] 来源：libs/riscv.h:210-214（`write_csr` 的签名；`proc_run` 里写 `satp` 用它）
```c
#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

#define write_csr(reg, val) ({ asm volatile("csrw " #reg ", %0" ::"rK"(val)); })
```

// [注] 来源：libs/riscv.h:244-248（反例：`lsatp()` 是 Sv32 遗留，模式位与参数类型都不适配 Sv39，本工程严禁使用）
```c
static inline void
lsatp(unsigned int pgdir)
{
  write_csr(satp, SATP32_MODE | (pgdir >> RISCV_PGSHIFT));
}
```

// [注] 来源：kern/trap/trapentry.S:66-76（首次运行链的后半段：`forkret` 调 `forkrets` 把 sp 指向 trapframe，再 `j __trapret` 恢复现场并 `sret`）
```asm
    .globl __trapret
__trapret:
    RESTORE_ALL
    # go back from supervisor call
    sret
    .globl forkrets
forkrets:
    # set stack to this new process's trapframe
    move sp, a0
    j __trapret
```

[GUARANTEE]

必须实现的四个函数（签名、所在文件、可见性固定，不得改名、不得改签名）：

```c
void cpu_idle(void);                                       // kern/process/proc.c，function3 marker 之下
void proc_run(struct proc_struct *proc);                   // kern/process/proc.c，同上
void schedule(void);                                       // kern/schedule/sched.c，marker 之下
void switch_to(struct context *from, struct context *to);  // kern/process/switch.S，全局符号，汇编实现
```

前三个已在 `proc.h` 中声明（`cpu_idle` 带 `__attribute__((noreturn))`），`switch_to` 已在 `proc.c:82` 声明；这两处声明都不得修改。

允许增加：

- `proc.c`、`sched.c` 内的 `static` 辅助函数（若确实需要）；
- `switch.S` 顶部的一行 `#include <riscv.h>`（仅此一行）。

必须保持不变的接口事实（任一项被改动即视为交付失败）：

- 四个函数的签名、可见性、返回类型，以及 `cpu_idle` 的 `noreturn` 语义；
- `proc.c`、`sched.c` 中 marker 以上的代码一行都不许动（含 `wakeup_proc`、`forkret`、`init_main`、`proc_init`、`do_fork`、`kernel_thread` 等既有函数）；
- `switch.S` 中 marker 以上只允许新增那一行 `#include <riscv.h>`；
- 三处 marker 与 `YOUR CODE` 字样原文保留（学号替换由我方在提交前统一处理）；
- `struct context`、`struct proc_struct`、`enum proc_state`、`sched.h` 的声明一个字节都不许动；
- 文件编码与换行风格（纯 ASCII、LF）不变。

明确禁止：

- 改动 `kern/process/proc.c`、`kern/schedule/sched.c`、`kern/process/switch.S` 之外的任何文件（含 `proc.h`、`sched.h`、`pmm.c`、`pmm.h`、`trapentry.S`、`Makefile`、`CMakeLists.txt`、`tools/`）；
- 修改 `wakeup_proc`、`forkret` 或任何既有函数，即使"觉得它该改"；
- 使用 `lsatp()` 写 `satp`，或对 `boot_pgdir_pa` 重新做 `PADDR`；
- 为了让 `make` 通过而补 `page_init`、`default_pmm`、`RESTORE_ALL`、`page_insert`，或添加任何临时桩；
- 新增 `switch_to` 以外的全局符号、新增跨文件导出接口；
- 运行 `make` / `make grade` / `make qemu`，或在工程内留下 `obj/`、`bin/`。

[SPECIFICATION]

## switch_to

**Pre-Condition**：

- 由 `proc_run` 在关中断状态下调用；`a0` 指向 `from`（当前线程的 `struct context`），`a1` 指向 `to`（目标线程的 `struct context`）；
- 两个 context 已按 `struct context` 的 14 个字段就绪：新线程的 `ra` 由 `copy_thread` 置为 `forkret`、`sp` 指向其内核栈上的 trapframe；老线程的字段是上次切出时保存的现场；
- 调用者不依赖任何 caller-saved 寄存器跨过本次调用（`switch_to` 只保证 callee-saved）。

**Post-Condition**：

- `from` 的 14 个字段被写成本次调用的现场：`ra` 为 `switch_to` 的返回地址，`sp` 为调用者的栈指针，`s0`–`s11` 为调用者的值；
- 全部通用寄存器（含 `sp`）取值为 `to` 中对应字段的原值；执行 `ret` 后控制流回到 `to->ra`；
- 不写内存中除 `*from` 以外的任何位置。

**Requirements**：

1. 保存与恢复的偏移必须严格对应 `struct context` 的字段序：`ra` 偏移 0、`sp` 8、`s0` 16，依次到 `s11` 112（64 位，每字段一个 `REGBYTES`）；
2. 必须先用旧 `sp` 把现场存进 `*from`，再从 `*a1` 恢复；`sp` 的恢复必须排在所有仍要使用旧 `sp` 的保存动作之后；
3. 形参只能经 `a0`、`a1` 取得；不得调用任何 C 函数，不得使用调用栈；
4. 必须导出 `.globl switch_to`，符号名不得更改；
5. 除 `ra`、`sp`、`s0`–`s11` 外不要求保存其它寄存器（caller-saved 由编译器负责）。

## schedule

**Pre-Condition**：

- `current` 指向正在运行的线程；`proc_list` 已由 `proc_init` / `do_fork` 建好，节点是 PCB 的 `list_link`；
- 调用可能发生在开中断或关中断状态下，函数必须自己保证原子性。

**Post-Condition**：

- 进入时的 `current` 的 `need_resched` 已被清零；
- 选中一个线程 `next` 且 `next->state == PROC_RUNNABLE`；
- `next->runs` 自增 1；
- 若 `next != current`，已通过 `proc_run(next)` 完成切换；若相同则不切换。

**Requirements**：

1. 选择起点：`current != idleproc` 时从 `current->list_link` 的下一个开始；`current == idleproc` 时从 `&proc_list`（表头）开始；
2. 沿 `proc_list` 环形遍历，遇 `PROC_RUNNABLE` 即选中；整圈都找不到则退回 `idleproc`；
3. 遍历必须能处理"链表里只有 `idleproc` 自己"的情形，不得死循环；
4. 清 `need_resched`、遍历、`runs++`、调 `proc_run` 全部在 `local_intr_save` / `local_intr_restore` 区内；
5. 不得修改除 `current->need_resched`、`next->runs`、`current`（经 `proc_run`）之外的任何状态。

## proc_run

**Pre-Condition**：

- `proc` 非空，且已由 `schedule` 选中（处于 `PROC_RUNNABLE`）；
- `proc->pgdir` 是该线程页目录的物理地址（本实验全部内核线程共享 `boot_pgdir_pa`）；
- `proc->context` 已就绪（新线程的 `ra` 为 `forkret`）。

**Post-Condition**：

- `current == proc`；
- `satp` 的模式字段为 8（Sv39），PPN 字段等于 `proc->pgdir >> RISCV_PGSHIFT`；
- 已通过 `switch_to(&prev->context, &next->context)` 完成现场切换，此后 CPU 执行的是 `proc` 的上下文；
- 若进入时 `proc == current`，函数不做任何写入。

**Requirements**：

1. `proc == current` 时直接返回；
2. `current = proc`、写 `satp`、调用 `switch_to` 三步必须在同一个 `local_intr_save` / `local_intr_restore` 区内；
3. 写 `satp` 必须用 `write_csr(satp, 0x8000000000000000 | (proc->pgdir >> RISCV_PGSHIFT))` 这一口径（与 `enable_paging()` 一致）；严禁 `lsatp()`，严禁对 `boot_pgdir_pa` 重新 `PADDR`；
4. TLB 刷新非必需（全部线程共用同一页表），调用或不调用 `flush_tlb()` 都不影响判定；
5. 不得在函数内递增 `runs`、不得修改 `need_resched`（分别属于 `schedule` 与 `cpu_idle` 的职责）。

## cpu_idle

**Pre-Condition**：

- 由 `kern_init` 在 `intr_enable()` 之后作为最后一个调用执行；此时 `current == idleproc`，且 `idleproc->need_resched == 1`；
- `schedule` 与 `proc_run` 已实现。

**Post-Condition**：

- 永不返回（函数声明带 `__attribute__((noreturn))`）；
- 每一轮循环都检查当前线程的 `need_resched`，为真时调用 `schedule()`。

**Requirements**：

1. 必须是 `for(;;)` 或 `while(1)` 形式的无限循环，函数体内不得出现 `return` 或 `do_exit`；
2. 判断对象是 `current->need_resched`（不是局部变量、不是 `idleproc->need_resched`），因为从别的线程切回来时 `current` 可能已经变了；
3. 不得添加 `wfi`、内存屏障、延时或任何"优化"，也不得在循环里打印；
4. 本函数不直接调用 `proc_run`。

## 非目标（本任务明确不做）

- 不实现 `do_exit`（仍是 `panic` 占位）；
- 不修 `do_fork` 忽略 `stack` 参数、`copy_mm` 内的 `assert(current->mm == NULL)`；
- 不补 P0（`page_init`、`default_pmm_manager`、`RESTORE_ALL`）与 M1（`page_insert`、`page_remove`）；
- 不追求本阶段"能编译"。

## 验收口径

- **静态验收（本阶段唯一可执行）**：
  - `kern/process/proc.c` 只在 `//LAB4 function3 YOUR CODE`（第 407 行）之下新增，其上一行都未改动；
  - `kern/schedule/sched.c` 只在 `//LAB4 function3 YOUR CODE`（第 13 行）之下新增，其上一行都未改动；
  - `kern/process/switch.S` 只在 marker 之上新增一行 `#include <riscv.h>`、在 marker 之下新增实现；
  - 其它文件零改动（`proc.h`、`sched.h`、`pmm.c`、`pmm.h`、`trapentry.S`、`Makefile`、`CMakeLists.txt`、`tools/`）；
  - 三处 marker 与 `YOUR CODE` 字样仍在；`obj/`、`bin/` 不存在。
- **语义验收（待 P0 + M1 落地后）**：`make grade` 出现 `[Test Schedule] PASS`，以及 `this initproc, pid = 1, name = "init"`、`To U: "Hello world!!".`、`To U: "en.., Bye, Bye. :)"` 三行。
- **不得**以"编译通过"作为验收依据（本阶段工程不完整，构建失败属预期）。
