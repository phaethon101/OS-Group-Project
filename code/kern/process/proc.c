#include <proc.h>
#include <kmalloc.h>
#include <string.h>
#include <sync.h>
#include <pmm.h>
#include <error.h>
#include <sched.h>
#include <elf.h>
#include <vmm.h>
#include <trap.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

/* ------------- 进程/线程机制的设计与实现 -------------
（Linux 进程/线程机制的简化版）
简介：
  ucore 实现了一套简单的进程/线程机制。一个进程包含独立的地址空间、至少一个
执行线程、内核管理数据、处理器现场（用于上下文切换）以及文件（lab6）等。ucore
需要高效地管理这些信息。在 ucore 里，线程只是进程的一种特例（共享进程的内存）。
------------------------------
进程状态            :     含义                    -- 谁负责置位
    PROC_UNINIT     :   未初始化                -- alloc_proc
    PROC_SLEEPING   :   睡眠                    -- try_free_pages, do_wait, do_sleep
    PROC_RUNNABLE   :   就绪（也可能正在运行）  -- proc_init, wakeup_proc
    PROC_ZOMBIE     :   僵尸（即将消亡）        -- do_exit

-----------------------------
进程状态转换：

  alloc_proc                                 RUNNING
      +                                   +--<----<--+
      +                                   + proc_run +
      V                                   +-->---->--+
PROC_UNINIT -- proc_init/wakeup_proc --> PROC_RUNNABLE -- try_free_pages/do_wait/do_sleep --> PROC_SLEEPING --
                                           A      +                                                           +
                                           |      +--- do_exit --> PROC_ZOMBIE                                +
                                           +                                                                  +
                                           -----------------------wakeup_proc----------------------------------
-----------------------------
进程关系
parent:           proc->parent  （proc 是子进程）
children:         proc->cptr    （proc 是父进程）
older sibling:    proc->optr    （proc 是弟弟）
younger sibling:  proc->yptr    （proc 是哥哥）
-----------------------------
与进程相关的系统调用：
SYS_exit        : 进程退出                                -->do_exit
SYS_fork        : 创建子进程（复制 mm）                   -->do_fork-->wakeup_proc
SYS_wait        : 等待子进程                              -->do_wait
SYS_exec        : fork 之后装载并执行一个程序             -->装载程序并刷新 mm
SYS_clone       : 创建子线程                              -->do_fork-->wakeup_proc
SYS_yield       : 进程自己请求重新调度                    -- proc->need_sched=1，随后调度器会重新调度它
SYS_sleep       : 进程睡眠                                -->do_sleep
SYS_kill        : 杀死进程                                -->do_kill-->proc->flags |= PF_EXITING
                                                          -->wakeup_proc-->do_wait-->do_exit
SYS_getpid      : 取进程 pid

*/

// 系统中所有进程的链表
list_entry_t proc_list;

#define HASH_SHIFT 10
#define HASH_LIST_SIZE (1 << HASH_SHIFT)
#define pid_hashfn(x) (hash32(x, HASH_SHIFT))

// 按 pid 组织的进程哈希表
static list_entry_t hash_list[HASH_LIST_SIZE];

// idle 进程
struct proc_struct *idleproc = NULL;
// init 进程
struct proc_struct *initproc = NULL;
// 当前正在运行的进程
struct proc_struct *current = NULL;

static int nr_process = 0;

void kernel_thread_entry(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);

// set_proc_name - 设置进程名
char *
set_proc_name(struct proc_struct *proc, const char *name)
{
    memset(proc->name, 0, sizeof(proc->name));
    return memcpy(proc->name, name, PROC_NAME_LEN);
}

// get_proc_name - 取进程名
char *
get_proc_name(struct proc_struct *proc)
{
    static char name[PROC_NAME_LEN + 1];
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->name, PROC_NAME_LEN);
}

// get_pid - 为进程分配一个唯一的 pid
static int
get_pid(void)
{
    static_assert(MAX_PID > MAX_PROCESS);
    struct proc_struct *proc;
    list_entry_t *list = &proc_list, *le;
    static int next_safe = MAX_PID, last_pid = MAX_PID;
    if (++last_pid >= MAX_PID)
    {
        last_pid = 1;
        goto inside;
    }
    if (last_pid >= next_safe)
    {
    inside:
        next_safe = MAX_PID;
    repeat:
        le = list;
        while ((le = list_next(le)) != list)
        {
            proc = le2proc(le, list_link);
            if (proc->pid == last_pid)
            {
                if (++last_pid >= next_safe)
                {
                    if (last_pid >= MAX_PID)
                    {
                        last_pid = 1;
                    }
                    next_safe = MAX_PID;
                    goto repeat;
                }
            }
            else if (proc->pid > last_pid && next_safe > proc->pid)
            {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}


// forkret -- 新线程/新进程第一次运行时的内核入口
// 注意：forkret 的地址是在 copy_thread 里写进 context.ra 的；
//       switch_to 切过去之后，当前线程就会从这里开始执行。
static void
forkret(void)
{
    forkrets(current->tf);
}

// hash_proc - 把 proc 挂进 pid 哈希表
static void
hash_proc(struct proc_struct *proc)
{
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link));
}

// find_proc - 按 pid 从哈希表里找出 proc
struct proc_struct *
find_proc(int pid)
{
    if (0 < pid && pid < MAX_PID)
    {
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list;
        while ((le = list_next(le)) != list)
        {
            struct proc_struct *proc = le2proc(le, hash_link);
            if (proc->pid == pid)
            {
                return proc;
            }
        }
    }
    return NULL;
}


// setup_kstack - 分配 KSTACKPAGE 页作为进程的内核栈
static int
setup_kstack(struct proc_struct *proc)
{
    struct Page *page = alloc_pages(KSTACKPAGE);
    if (page != NULL)
    {
        proc->kstack = (uintptr_t)page2kva(page);
        return 0;
    }
    return -E_NO_MEM;
}

// put_kstack - 释放进程内核栈占用的内存
static void
put_kstack(struct proc_struct *proc)
{
    free_pages(kva2page((void *)(proc->kstack)), KSTACKPAGE);
}


// do_exit - 由 sys_exit 调用
//   1. 调 exit_mmap、put_pgdir、mm_destroy 释放进程几乎全部内存空间
//   2. 把进程状态置为 PROC_ZOMBIE，再 wakeup_proc(parent)，让父进程来回收它
//   3. 调用调度器切换到别的进程
int do_exit(int error_code)
{
    panic("process exit!!.\n");
}

// init_main - 第二个内核线程，后续实验里用来创建用户进程
static int
init_main(void *arg)
{
    cprintf("this initproc, pid = %d, name = \"%s\"\n", current->pid, get_proc_name(current));
    cprintf("To U: \"%s\".\n", (const char *)arg);
    cprintf("To U: \"en.., Bye, Bye. :)\"\n");
    return 0;
}

//LAB4 function2 2410621

// alloc_proc - 分配一个 proc_struct 并初始化它的全部字段
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL)
    {
        // 先整体清零，保证每个字段都从确定的初始值开始
        memset(proc, 0, sizeof(struct proc_struct));
        proc->state = PROC_UNINIT;
        proc->pid = -1;
        proc->runs = 0;
        proc->kstack = 0;
        proc->need_resched = 0;
        proc->parent = NULL;
        proc->mm = NULL;
        proc->tf = NULL;
        proc->pgdir = boot_pgdir_pa;
        proc->flags = 0;
        list_init(&(proc->list_link));
        list_init(&(proc->hash_link));
    }
    return proc;
}

// setup_kstack/put_kstack 上面已经给出；
// 下面的函数在它们之上搭出一个可运行的线程/进程。

// copy_mm - 为 proc 复制 current 的地址空间
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc)
{
    // 内核线程共享内核地址空间，此时 current->mm 为 NULL；
    // 本实验阶段还不实现用户 vma/pgdir 的复制。
    (void)clone_flags;
    assert(current->mm == NULL);
    proc->mm = NULL;
    return 0;
}

// copy_thread - 把 trapframe 与初始上下文布置到 proc 自己的内核栈上
//               esp: proc 内核栈的栈顶
//               tf : 父进程的中断帧（内核线程用的是伪造出来的那个）
static void
copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf)
{
    // trapframe 放在新内核栈的顶部
    proc->tf = (struct trapframe *)(esp - sizeof(struct trapframe));
    *(proc->tf) = *tf;

    // 子进程从 fork 返回时返回值应为 0
    proc->tf->gpr.a0 = 0;

    // 内核线程没有用户栈（这里 tf->gpr.sp 为 0），让它直接用自己的内核栈；
    // 若将来是用户进程 fork，则保留复制过来的用户 sp。
    if (proc->tf->gpr.sp == 0)
    {
        proc->tf->gpr.sp = esp;
    }

    // 被 switch_to 切进来之后，新线程从 forkret 开始执行
    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)(proc->tf);
}

// do_fork - 按 clone_flags 创建一个新进程/线程
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf)
{
    int ret = -E_NO_FREE_PROC;
    struct proc_struct *proc = NULL;
    uintptr_t esp;

    // 这里的 stack 参数用不上：新线程总是从自己的内核栈开始跑（下面的 esp），
    // 对 lab4 的内核线程来说这已经够了。
    (void)stack;

    if (nr_process >= MAX_PROCESS)
    {
        goto fork_out;
    }

    ret = -E_NO_MEM;
    int pid = get_pid();
    if ((proc = alloc_proc()) == NULL)
    {
        goto fork_out;
    }
    proc->pid = pid;
    proc->parent = current;

    if (setup_kstack(proc) != 0)
    {
        goto bad_fork_cleanup_proc;
    }

    if (copy_mm(clone_flags, proc) != 0)
    {
        goto bad_fork_cleanup_kstack;
    }

    esp = proc->kstack + KSTACKSIZE;
    copy_thread(proc, esp, tf);

    set_proc_name(proc, current->name);
    proc->state = PROC_RUNNABLE;

    bool intr_flag;
    local_intr_save(intr_flag);
    {
        list_add(&proc_list, &(proc->list_link));
        hash_proc(proc);
        nr_process++;
    }
    local_intr_restore(intr_flag);

    ret = proc->pid;

fork_out:
    return ret;

bad_fork_cleanup_kstack:
    put_kstack(proc);
bad_fork_cleanup_proc:
    kfree(proc);
    goto fork_out;
}

// kernel_thread - 创建一个入口为 fn(arg) 的内核线程
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
{
    struct trapframe tf;

    // 伪造一个 trapframe：kernel_thread_entry 会执行 "move a0, s1; jalr s0"，
    // 所以函数指针放 s0、参数放 s1。
    memset(&tf, 0, sizeof(struct trapframe));
    tf.gpr.s0 = (uintptr_t)fn;
    tf.gpr.s1 = (uintptr_t)arg;
    // 返回 S 态；中断先关着，由 sret 依据 SPIE 重新打开
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;
    tf.epc = (uintptr_t)kernel_thread_entry;

    // 内核线程没有用户栈，所以 stack 传 0
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}

// proc_init - 建立 idle 线程，并创建第一个真正的内核线程
void proc_init(void)
{
    int i;

    list_init(&proc_list);
    for (i = 0; i < HASH_LIST_SIZE; i++)
    {
        list_init(hash_list + i);
    }

    if ((idleproc = alloc_proc()) == NULL)
    {
        panic("cannot alloc idleproc.\n");
    }

    idleproc->pid = 0;
    idleproc->state = PROC_RUNNABLE;
    // 当前执行现场就跑在启动栈上，直接复用它
    idleproc->kstack = (uintptr_t)bootstack;
    idleproc->need_resched = 1;
    idleproc->mm = NULL;
    idleproc->parent = NULL;

    // idleproc 只代表"当前内核执行现场"，不挂进 proc_list / hash_list：
    // 它由 cpu_idle 自己承载，调度器在找不到就绪线程时才回退到它。
    // 若把 idleproc 也挂进 proc_list，schedule 从表头起找时第一个就会选中它，
    // 于是每次调度都"选中自己、不切换"，initproc 永远得不到 CPU。
    nr_process = 1;
    current = idleproc;

    int pid = kernel_thread(init_main, "Hello world!!", CLONE_VM);
    if (pid <= 0)
    {
        panic("create init_main failed.\n");
    }

    initproc = find_proc(pid);
    set_proc_name(initproc, "init");
}

//LAB4 function3 2410621

// cpu_idle - 空闲线程的主循环
// 说明：本函数永不返回；它检查的是 current 的 need_resched，
//       因为中途可能已经被调度器切到别的线程去了。
void
cpu_idle(void)
{
    while (1)
    {
        if (current->need_resched)
        {
            schedule();
        }
    }
}

// proc_run - 让 proc 在本 CPU 上运行
//   1. 更新 current
//   2. 把 satp 切到 proc 的页目录（Sv39：最高位是模式 8，
//      PPN = pgdir >> RISCV_PGSHIFT，写法与 enable_paging() 一致）
//   3. 通过 switch_to 保存旧现场、恢复新现场
void
proc_run(struct proc_struct *proc)
{
    if (proc != current)
    {
        bool intr_flag;
        struct proc_struct *prev = current, *next = proc;
        local_intr_save(intr_flag);
        {
            current = proc;
            write_csr(satp, 0x8000000000000000 | (proc->pgdir >> RISCV_PGSHIFT));
            switch_to(&(prev->context), &(next->context));
        }
        local_intr_restore(intr_flag);
    }
}
