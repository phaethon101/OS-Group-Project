#include <assert.h>
#include <clock.h>
#include <console.h>
#include <defs.h>
#include <kdebug.h>
#include <memlayout.h>
#include <mmu.h>
#include <riscv.h>
#include <stdio.h>
#include <trap.h>
#include <vmm.h>

#define TICK_NUM 100

static void print_ticks()
{
    cprintf("%d ticks\n", TICK_NUM);
#ifdef DEBUG_GRADE
    cprintf("End of Test.\n");
    panic("EOT: kernel seems ok.");
#endif
}

/* idt_init - 把中断向量表指向 kern/trap/trapentry.S 里的入口
 */
void idt_init(void)
{
    extern void __alltraps(void);
    /* 把 sscratch 置 0：告诉异常向量表，当前已经在内核里执行 */
    write_csr(sscratch, 0);
    /* 设置异常向量入口地址 */
    write_csr(stvec, &__alltraps);
    /* 允许内核访问用户内存（SUM 位） */
    set_csr(sstatus, SSTATUS_SUM);
}

void print_trapframe(struct trapframe *tf)
{
    cprintf("trapframe at %p\n", tf);
    print_regs(&tf->gpr);
    cprintf("  status   0x%08x\n", tf->status);
    cprintf("  epc      0x%08x\n", tf->epc);
    cprintf("  badvaddr 0x%08x\n", tf->badvaddr);
    cprintf("  cause    0x%08x\n", tf->cause);
}

void print_regs(struct pushregs *gpr)
{
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

extern struct mm_struct *check_mm_struct;

//lab3 function2 2410621

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



/* *
 * trap - 异常/中断的总处理入口。trap() 返回后，
 * kern/trap/trapentry.S 里的代码会从 trapframe 中恢复被保存的 CPU 现场，
 * 再用 sret 从中断/异常中返回。
 * */
void trap(struct trapframe *tf)
{
    // 按 trap 的类型分派：scause 最高位为 1 是中断，否则是异常
    if ((intptr_t)tf->cause < 0)
    {
        // 中断
        interrupt_handler(tf);
    }
    else
    {
        // 异常
        exception_handler(tf);
    }
}
