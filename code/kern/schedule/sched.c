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

//LAB4 function3 2410621

// schedule - 按 FIFO 轮转挑出下一个可运行的线程
//   规则一：从 current 的下一个节点开始找；current 是 idleproc 时从表头开始；
//   规则二：绕 proc_list 走一圈，取第一个 PROC_RUNNABLE 的线程；
//           一个都找不到就回退到 idleproc；
//   规则三：先把 current->need_resched 清零，把这次运行记到 next->runs 上，
//           只有 next 不是 current 时才真正切换。
void
schedule(void) {
    bool intr_flag;
    list_entry_t *le, *last;
    struct proc_struct *next = NULL;

    local_intr_save(intr_flag);
    {
        current->need_resched = 0;
        last = (current == idleproc) ? &proc_list : &(current->list_link);
        le = last;
        do {
            if ((le = list_next(le)) != &proc_list) {
                next = le2proc(le, list_link);
                if (next->state == PROC_RUNNABLE) {
                    break;
                }
            }
        } while (le != last);
        if (next == NULL || next->state != PROC_RUNNABLE) {
            next = idleproc;
        }
        next->runs++;
        if (next != current) {
            proc_run(next);
        }
    }
    local_intr_restore(intr_flag);
}


