[PROMPT]

**任务**：修改 kern/trap/trapentry.S，实现 lab3 功能模块一「Trap 上下文恢复与 Trap 入口」。本任务交付两件事：其一，补全汇编宏 RESTORE_ALL，把 SAVE_ALL 先前保存在内核栈 trapframe 中的处理器现场恢复回寄存器，使 Trap 返回后原执行流能够继续；其二，在 __alltraps 中 SAVE_ALL 之后补上缺失的调用，把当前 trapframe 按 RISC-V 调用约定交给 kern/trap/trap.c 的 trap()，并在其返回后进入 __trapret，由 sret 返回原执行流。本任务只负责上下文恢复与入口衔接，不负责中断与异常的分类处理（interrupt_handler、exception_handler 不在范围内），也不负责时钟中断、打印与关机等行为。

**操作要求**：必须在项目中修改真实文件 kern/trap/trapentry.S，而不是仅展示代码片段；保留文件中的现有代码，只在两处 "#lab3 function1 YOUR CODE" 标记处补全，不得删除既有逻辑；两处 `#lab3` 注释行须原样保留，`YOUR CODE` 字样不得删除或改写；SAVE_ALL 的保存方式、struct trapframe 的字段顺序、trap() 的函数签名均属不可改动的既有事实，不得修改，也不得改动 trap.c、trap.h 及其他任何文件；允许新增汇编宏、局部标签或临时寄存器用法作为辅助，但不得另起 Trap 入口、不得绕过 trap() / trap_dispatch() / __trapret 这条既有调用链，不得新增全局符号。

**输出要求**：直接进行文件操作；使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 提供的信息；以工程真实代码为准，不引入无关功能；产出物必须与现有构建方式兼容（.S 文件经 C 预处理器处理，由 riscv64-unknown-elf-gcc 汇编链接，因此 #include、STORE / LOAD / REGBYTES 可用）。

[RELY]

// [注] 以下源码均为工程源码逐字原文；凡以 `// [注]` 开头的行，均是我方说明，不是源码。
// [注] 来源：libs/riscv.h:6-19（存取宏与寄存器宽度；REGBYTES 随 xlen 变化，不得硬编码为 8）
```c
#if __riscv_xlen == 64
# define SLL32    sllw
# define STORE    sd
# define LOAD     ld
# define LWU      lwu
# define LOG_REGBYTES 3
#else
# define SLL32    sll
# define STORE    sw
# define LOAD     lw
# define LWU      lw
# define LOG_REGBYTES 2
#endif
#define REGBYTES (1 << LOG_REGBYTES)
```

// [注] 来源：libs/riscv.h:41-45（sstatus 位定义；回写 sstatus 时须据此清除 SIE 位）
```c
#define SSTATUS_UIE         0x00000001
#define SSTATUS_SIE         0x00000002
#define SSTATUS_UPIE        0x00000010
#define SSTATUS_SPIE        0x00000020
#define SSTATUS_SPP         0x00000100
```

// [注] 来源：kern/trap/trap.h 全文（trapframe 布局与 trap() 接口声明）
```c
#ifndef __KERN_TRAP_TRAP_H__
#define __KERN_TRAP_TRAP_H__

#include <defs.h>

struct pushregs {
    uintptr_t zero;  // Hard-wired zero
    uintptr_t ra;    // Return address
    uintptr_t sp;    // Stack pointer
    uintptr_t gp;    // Global pointer
    uintptr_t tp;    // Thread pointer
    uintptr_t t0;    // Temporary
    uintptr_t t1;    // Temporary
    uintptr_t t2;    // Temporary
    uintptr_t s0;    // Saved register/frame pointer
    uintptr_t s1;    // Saved register
    uintptr_t a0;    // Function argument/return value
    uintptr_t a1;    // Function argument/return value
    uintptr_t a2;    // Function argument
    uintptr_t a3;    // Function argument
    uintptr_t a4;    // Function argument
    uintptr_t a5;    // Function argument
    uintptr_t a6;    // Function argument
    uintptr_t a7;    // Function argument
    uintptr_t s2;    // Saved register
    uintptr_t s3;    // Saved register
    uintptr_t s4;    // Saved register
    uintptr_t s5;    // Saved register
    uintptr_t s6;    // Saved register
    uintptr_t s7;    // Saved register
    uintptr_t s8;    // Saved register
    uintptr_t s9;    // Saved register
    uintptr_t s10;   // Saved register
    uintptr_t s11;   // Saved register
    uintptr_t t3;    // Temporary
    uintptr_t t4;    // Temporary
    uintptr_t t5;    // Temporary
    uintptr_t t6;    // Temporary
};

struct trapframe {
    struct pushregs gpr;
    uintptr_t status;
    uintptr_t epc;
    uintptr_t badvaddr;
    uintptr_t cause;
};

void trap(struct trapframe *tf);
void idt_init(void);
void print_trapframe(struct trapframe *tf);
void print_regs(struct pushregs* gpr);
bool trap_in_kernel(struct trapframe *tf);

#endif /* !__KERN_TRAP_TRAP_H__ */
```

// [注] 来源：kern/trap/trapentry.S 全文（既有 SAVE_ALL、两处待补占位、__trapret 与 sret）
```asm
#include <riscv.h>

    .macro SAVE_ALL

    csrw sscratch, sp

    addi sp, sp, -36 * REGBYTES
    # save x registers
    STORE x0, 0*REGBYTES(sp)
    STORE x1, 1*REGBYTES(sp)
    STORE x3, 3*REGBYTES(sp)
    STORE x4, 4*REGBYTES(sp)
    STORE x5, 5*REGBYTES(sp)
    STORE x6, 6*REGBYTES(sp)
    STORE x7, 7*REGBYTES(sp)
    STORE x8, 8*REGBYTES(sp)
    STORE x9, 9*REGBYTES(sp)
    STORE x10, 10*REGBYTES(sp)
    STORE x11, 11*REGBYTES(sp)
    STORE x12, 12*REGBYTES(sp)
    STORE x13, 13*REGBYTES(sp)
    STORE x14, 14*REGBYTES(sp)
    STORE x15, 15*REGBYTES(sp)
    STORE x16, 16*REGBYTES(sp)
    STORE x17, 17*REGBYTES(sp)
    STORE x18, 18*REGBYTES(sp)
    STORE x19, 19*REGBYTES(sp)
    STORE x20, 20*REGBYTES(sp)
    STORE x21, 21*REGBYTES(sp)
    STORE x22, 22*REGBYTES(sp)
    STORE x23, 23*REGBYTES(sp)
    STORE x24, 24*REGBYTES(sp)
    STORE x25, 25*REGBYTES(sp)
    STORE x26, 26*REGBYTES(sp)
    STORE x27, 27*REGBYTES(sp)
    STORE x28, 28*REGBYTES(sp)
    STORE x29, 29*REGBYTES(sp)
    STORE x30, 30*REGBYTES(sp)
    STORE x31, 31*REGBYTES(sp)

    # get sr, epc, badvaddr, cause
    # Set sscratch register to 0, so that if a recursive exception
    # occurs, the exception vector knows it came from the kernel
    csrrw s0, sscratch, x0
    csrr s1, sstatus
    csrr s2, sepc
    csrr s3, sbadaddr
    csrr s4, scause

    STORE s0, 2*REGBYTES(sp)
    STORE s1, 32*REGBYTES(sp)
    STORE s2, 33*REGBYTES(sp)
    STORE s3, 34*REGBYTES(sp)
    STORE s4, 35*REGBYTES(sp)
    .endm

    .macro RESTORE_ALL

    #lab3 function1 YOUR CODE

    .endm

    .globl __alltraps
    .align(2)
__alltraps:
    SAVE_ALL

    #lab3 function1 YOUR CODE
    # sp should be the same as before "jal trap"

    .globl __trapret
__trapret:
    RESTORE_ALL
    # return from supervisor call
    sret
```

// [注] trap() 定义在 kern/trap/trap.c，函数体只做 trap_dispatch(tf) 分派后返回，本次不改动该文件；trap_dispatch() 及其下游处理函数同样不在本次范围内。

[GUARANTEE]

必须实现或修改的接口：

```asm
    .macro RESTORE_ALL
    .endm
```

- `RESTORE_ALL`：汇编宏，无参数、无返回值，作用于执行时 sp 所指的 trapframe；宏名与作用位置不得更改。
- `__alltraps`：既有的全局 Trap 入口符号，本次只补全其中 SAVE_ALL 之后、`__trapret` 之前的调用衔接，入口符号名、对齐方式与相对位置不得更改。

你可以根据需要添加：
- 汇编宏（仅限本文件内使用）
- `.L` 前缀的局部标签
- 临时寄存器用法

不得新增全局符号（`.globl`），不得改动 `SAVE_ALL`、`__trapret`、`sret` 与 `.align(2)`。

[SPECIFICATION]

## RESTORE_ALL

**Pre-Condition**：
- 执行时 sp 指向本次 Trap 入口由 SAVE_ALL 建立的 trapframe 基址；该 trapframe 的 36 个槽位（每槽 REGBYTES 字节）已按 struct trapframe 的布局完整填充。
- 处理器处于 S 模式，且 Trap 入口已由硬件关闭 S 模式中断（sstatus.SIE 为 0）。
- 宏体不得依赖任何未在 trapframe 中保存的寄存器原值。

**Post-Condition**：
- 通用寄存器 x1、x3–x31 的值等于 Trap 发生前的值；x2（sp）等于 Trap 发生前的栈指针；x0 仍为 0。
- CSR sepc 等于 trapframe 第 33 槽中的值，即 trap() 可能修改过的 tf->epc。
- sp 已不再指向该 trapframe（现场已被消费完毕），此后执行 sret 即可返回原执行流。
- 到 sret 执行为止，sstatus.SIE 保持为 0。

**Requirements**：
- 必须与 SAVE_ALL 的保存关系逐槽对应，不得假设与之不同的布局，不得改动槽位编号与槽位含义。
- 第 2 槽保存的是「分配 trapframe 之前」的 sp，必须与普通通用寄存器区别对待。
- sp 的恢复必须排在所有仍需访问 trapframe 的操作之后；sepc 的回写必须发生在 sp 恢复之前。
- sepc 只能从 trapframe 第 33 槽恢复；本宏是内核中唯一写 sepc 的代码位置。
- 若选择回写 sstatus（第 32 槽），必须先清除 SSTATUS_SIE 位再写回，保证 sret 之前中断保持关闭；中断的重新使能交由 sret 依据 sstatus.SPIE 完成。
- 不得为使 sret 返回而另行改写 sepc 之外的 Trap 返回状态，也不得依赖 trap() 之外的任何 C 函数。

## __alltraps（SAVE_ALL 之后的入口衔接）

**Pre-Condition**：
- SAVE_ALL 已执行完毕，sp 指向本次 Trap 的 trapframe 基址，其中包含完整现场。
- stvec 已指向 __alltraps（由 idt_init 设置），且 __alltraps 入口保持四字节对齐。

**Post-Condition**：
- trap() 已被调用，其唯一实参是当前 trapframe 的指针，按 RISC-V 调用约定经 a0 传递。
- trap() 正常返回后，控制流顺序到达 __trapret，由 RESTORE_ALL 恢复现场并经 sret 返回。
- 调用前后 sp 保持不变，仍指向同一 trapframe 基址。

**Requirements**：
- 不得绕过 trap() 或 trap_dispatch() 自行分派或处理 Trap，也不得在 __alltraps 内实现中断/异常分类逻辑。
- 不得改变 __alltraps 与 __trapret 的全局符号、对齐方式及两者的相对位置关系。
- 返回地址的保存方式必须使 trap() 的返回能落回 __trapret；不得依赖 trap() 不返回（关机等行为不属于本模块）。
