#include <assert.h>
#include <clock.h>
#include <console.h>
#include <defs.h>
#include <kdebug.h>
#include <memlayout.h>
#include <mmu.h>
#include <riscv.h>
#include <sbi.h>
#include <stdio.h>
#include <trap.h>

#define TICK_NUM 100
/* 每累计 TICK_NUM 次时钟中断输出一行统计信息；累计输出 SHUTDOWN_AFTER 行后关机。
 * 1 行 = TICK_NUM 次中断 = 1 秒（QEMU 上 timebase 使中断频率约为 100Hz）。 */
#define SHUTDOWN_AFTER 10

static void print_ticks() {
    cprintf("%d ticks\n", TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* idt_init - initialize IDT to each of the entry points in kern/trap/vectors.S
 */
void idt_init(void) {

    extern void __alltraps(void);
    /* Set sup0 scratch register to 0, indicating to exception vector
       that we are presently executing in the kernel */
    write_csr(sscratch, 0);
    /* Set the exception vector address */
    write_csr(stvec, &__alltraps);
}

/* trap_in_kernel - test if trap happened in kernel */
bool trap_in_kernel(struct trapframe *tf) {
    return (tf->status & SSTATUS_SPP) != 0;
}

void print_trapframe(struct trapframe *tf) {
    cprintf("trapframe at %p\n", tf);
    print_regs(&tf->gpr);
    cprintf("  status   0x%08x\n", tf->status);
    cprintf("  epc      0x%08x\n", tf->epc);
    cprintf("  badvaddr 0x%08x\n", tf->badvaddr);
    cprintf("  cause    0x%08x\n", tf->cause);
}

void print_regs(struct pushregs *gpr) {
    cprintf("  zero     0x%08x\n", gpr->zero);
    cprintf("  ra       0x%08x\n", gpr->ra);
    cprintf("  sp       0x%08x\n", gpr->sp);
    cprintf("  gp       0x%08x\n", gpr->gp);
    cprintf("  tp       0x%08x\n", gpr->tp);
    cprintf("  t0       0x%08x\n", gpr->t0);
    cprintf("  t1       0x%08x\n", gpr->t1);
    cprintf("  t2       0x%08x\n", gpr->t2);
    cprintf("  s0       0x%08x\n", gpr->s0);
    cprintf("  s1       0x%08x\n", gpr->s1);
    cprintf("  a0       0x%08x\n", gpr->a0);
    cprintf("  a1       0x%08x\n", gpr->a1);
    cprintf("  a2       0x%08x\n", gpr->a2);
    cprintf("  a3       0x%08x\n", gpr->a3);
    cprintf("  a4       0x%08x\n", gpr->a4);
    cprintf("  a5       0x%08x\n", gpr->a5);
    cprintf("  a6       0x%08x\n", gpr->a6);
    cprintf("  a7       0x%08x\n", gpr->a7);
    cprintf("  s2       0x%08x\n", gpr->s2);
    cprintf("  s3       0x%08x\n", gpr->s3);
    cprintf("  s4       0x%08x\n", gpr->s4);
    cprintf("  s5       0x%08x\n", gpr->s5);
    cprintf("  s6       0x%08x\n", gpr->s6);
    cprintf("  s7       0x%08x\n", gpr->s7);
    cprintf("  s8       0x%08x\n", gpr->s8);
    cprintf("  s9       0x%08x\n", gpr->s9);
    cprintf("  s10      0x%08x\n", gpr->s10);
    cprintf("  s11      0x%08x\n", gpr->s11);
    cprintf("  t3       0x%08x\n", gpr->t3);
    cprintf("  t4       0x%08x\n", gpr->t4);
    cprintf("  t5       0x%08x\n", gpr->t5);
    cprintf("  t6       0x%08x\n", gpr->t6);
}

//lab3 function2 2411204

/* *
 * trap_cause_code - 取出 scause 里的原因编码
 *
 * 中断时 scause 的最高位（bit 63）为 1，低 63 位才是中断编号；
 * 异常时最高位为 0。比较前必须把它屏蔽掉，否则 IRQ_S_TIMER(5)
 * 这类小整数永远匹配不上。
 * */
static uintptr_t trap_cause_code(uintptr_t cause) {
    return cause & ~((uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1));
}

/* *
 * instruction_length - 返回 epc 处那条指令的字节长度（2 或 4）
 *
 * RISC-V 规定 32 位指令的低 2 位恒为 0b11，否则是 16 位压缩指令。
 * 开启 C 扩展后，汇编器会把 ebreak 压缩成 c.ebreak（2 字节），
 * 所以 sepc 的推进量不能一律按 4 计算。
 * */
static size_t instruction_length(uintptr_t epc) {
    uint16_t insn = *(volatile uint16_t *)epc;
    return ((insn & 0x3) == 0x3) ? 4 : 2;
}

/* *
 * interrupt_handler - 处理中断（scause 最高位为 1）
 *
 * 时钟中断必须在这里重新装载下一次时钟事件，否则只会响一次；
 * 其余中断只做类型区分，无法识别的用 print_trapframe() 兜底。
 * */
void interrupt_handler(struct trapframe *tf) {
    uintptr_t cause = trap_cause_code(tf->cause);

    switch (cause) {
    case IRQ_S_TIMER:
        /* 先续上下一次时钟事件，避免后续处理把定时精度拖偏 */
        clock_set_next_event();
        ticks++;
        if (ticks % TICK_NUM == 0) {
            print_ticks();
            if (ticks >= (size_t)SHUTDOWN_AFTER * TICK_NUM) {
                sbi_shutdown();
            }
        }
        break;
    case IRQ_S_SOFT:
        cprintf("Supervisor software interrupt at 0x%016lx\n", tf->epc);
        break;
    case IRQ_S_EXT:
        cprintf("Supervisor external interrupt at 0x%016lx\n", tf->epc);
        break;
    default:
        cprintf("Unknown interrupt: cause %lu\n", (unsigned long)cause);
        print_trapframe(tf);
        break;
    }
}

/* *
 * exception_handler - 处理异常（scause 最高位为 0）
 *
 * 非法指令和断点异常处理完必须把 tf->epc 推进到"下一条指令"，
 * 否则 sret 回去会重新执行同一条指令，形成异常死循环。
 * 其余异常用 print_trapframe() 兜底。
 * */
void exception_handler(struct trapframe *tf) {
    switch (tf->cause) {
    case CAUSE_ILLEGAL_INSTRUCTION:
        cprintf("Illegal instruction at 0x%016lx\n", tf->epc);
        tf->epc += instruction_length(tf->epc);
        break;
    case CAUSE_BREAKPOINT:
        cprintf("Breakpoint at 0x%016lx\n", tf->epc);
        tf->epc += instruction_length(tf->epc);
        break;
    default:
        print_trapframe(tf);
        break;
    }
}


static inline void trap_dispatch(struct trapframe *tf) {
    if ((intptr_t)tf->cause < 0) {
        // interrupts
        interrupt_handler(tf);
    } else {
        // exceptions
        exception_handler(tf);
    }
}

/* *
 * trap - handles or dispatches an exception/interrupt. if and when trap()
 * returns,
 * the code in kern/trap/trapentry.S restores the old CPU state saved in the
 * trapframe and then uses the iret instruction to return from the exception.
 * */
void trap(struct trapframe *tf) {
    // dispatch based on what type of trap occurred
    trap_dispatch(tf);
}
