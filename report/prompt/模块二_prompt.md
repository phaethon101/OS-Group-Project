[PROMPT]
任务：修改kern/process/proc.c，在注释LAB4 function2 YOUR CODE下方和注释LAB4 function3 YOUR CODE上方，利用文件内已经提供好的get_pid、setup_kstack、put_kstack、hash_proc、find_proc、forkret等工具函数，完成 fork 创建子进程。实现完成后，系统可以通过do_fork创建新进程 / 线程，加入全局进程链表与 pid 哈希表，调度器可以调度运行新进程。
操作要求：修改项目真实源文件，不只是输出代码片段。proc.c中已有的函数、现有头文件、原有注释禁止修改，已有的结构体、接口签名不能改动，不改动do_exit占位实现，头文件proc.h结构体、函数签名禁止修改，不要新增跨文件对外接口。内部可增加 static 辅助函数。自行实现进程创建相关内部逻辑，完成函数注册。
输出要求：输出可直接写入目标文件的完整实现代码，保留原有全部代码与测试函数；严格遵循 [RELY] 提供结构体、库接口、全局变量；遵守 [GUARANTEE] 强制接口约束；满足 [SPECIFICATION] 每个函数的前置、后置条件和功能要求。
[RELY]
进程控制块 PCB 定义位于kern/process/proc.h：
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
内存管理、内存分配接口：
```c
// pmm物理内存分配
struct Page *alloc_pages(size_t n)
void free_pages(struct Page *base, size_t n)
// vmm虚拟内存管理
struct mm_struct *mm_create(void)
void mm_destroy(struct mm_struct *mm)
struct vma_struct *vma_create(start, end, flags)
void insert_vma_struct(mm, vma)
struct vma_struct *find_vma(mm, addr)
```
可用的全局变量
```c
extern list_entry_t proc_list;
static list_entry_t hash_list[HASH_LIST_SIZE];
struct proc_struct *idleproc, *initproc, *current;
static int nr_process;
#define pid_hashfn(x) (hash32(x, HASH_SHIFT))
#define HASH_LIST_SIZE (1 << HASH_SHIFT)
#define KSTACKPAGE 
```
可调用的函数
```c
static int get_pid(void);
static void hash_proc(struct proc_struct *proc);
struct proc_struct *find_proc(int pid);
static int setup_kstack(struct proc_struct *proc);
static void put_kstack(struct proc_struct *proc);
static void forkret(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);
char *set_proc_name(struct proc_struct *proc, const char *name);
char *get_proc_name(struct proc_struct *proc);
```
[GUARANTEE]
必须实现下面全部 static / 全局函数，完成功能链路：
```c
static struct proc_struct *alloc_proc(void);
void proc_init(void);
int kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags);
int do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf);
static void copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf);
static int copy_mm(uint32_t clone_flags, struct proc_struct *proc);
```
alloc_proc 负责得到一个干净的进程控制块
proc_init 负责把当前执行现场包装成 idleproc 并创建第一个真正的内核线程 initproc
kernel_thread 负责把函数入口和参数整理成线程首次运行所需的临时上下文
do_fork 负责把这些信息和内核栈、PID、链表、状态位等资源真正组合成一个可运行实体
copy_thread 负责把 trapframe 和 context 安放到新线程自己的内核栈上
copy_mm 则在当前实验阶段承担地址空间处理接口的占位作用。
允许增加：static 辅助函数、模块内部宏；
禁止修改：禁止修改头文件内函数原型签名，禁止新增跨文件导出接口，禁止修改已有函数、结构体和注释
[SPECIFICATION]
#alloc_proc
Pre‑Condition：内核kmalloc内存分配器可用。
Post‑Condition：成功返回完全清零、初始化完成的struct proc_struct *；内存不足返回NULL。
Requirements
1.kmalloc分配sizeof(struct proc_struct)；
2.memset将整个 PCB 全部清零；
3.proc->state = PROC_UNINIT；proc->need_resched = 0；
4.对list_link、hash_link执行list_init；
5.parent/cptr/yptr/optr全部赋值NULL；
6.kstack/mm/tf/pid置 0，不分配任何内核栈、地址空间资源。
#proc_init
Pre‑Condition：pmm、kmalloc 已经初始化；尚未初始化进程子系统。
Post‑Condition：进程管理子系统就绪；idleproc代表当前内核执行现场；创建initproc；全局current = idleproc；系统存在两个线程。
Requirements
1.初始化全局进程链表proc_list；循环初始化hash_list全部哈希桶；
2.alloc_proc()得到idleproc；设置idleproc->pid = 0；idleproc->state = PROC_RUNNABLE；
3.setup_kstack(idleproc)分配 idle 进程内核栈；idleproc->mm = NULL；父指针置NULL；
4.将idleproc加入proc_list；调用hash_proc(idleproc)；nr_process = 1；current = idleproc；
5.调用kernel_thread(init_main, "Hello world!!", CLONE_VM)，保存返回的 pid 到initproc（通过find_proc得到initproc指针）；
6.设置initproc进程名字；此时系统拥有idleproc、initproc两个线程。
#copy_mm（实验阶段占位实现）
Pre‑Condition：proc为合法子进程 PCB；源进程为全局current。
Post‑Condition：返回 0；子进程 mm 字段设置完成。
Requirements
1.本阶段仅做占位；内核线程场景current->mm == NULL，直接proc->mm = NULL；
2.返回 0；暂不实现用户态 vma 拷贝逻辑。
#copy_thread
Pre‑Condition：子进程proc已经分配 PCB 与内核栈；esp为子进程内核栈栈顶；tf为父进程中断帧。
Post‑Condition：proc->tf、proc->context全部布置在子进程内核栈上。
Requirements
1.在esp内核栈顶部向下开辟struct trapframe空间，proc->tf指向该栈内 trapframe；
2.拷贝父进程trapframe到子进程栈；修改寄存器，保证子进程从 fork 返回时值为 0；
3.proc->esp0 = esp；
4.初始化proc->context上下文：设置栈指针，返回地址ra = forkret；保证switch_to切换后子进程从forkret开始执行。
#do_fork
Pre‑Condition：clone_flags合法；tf为当前进程有效中断帧。
Post‑Condition：成功返回子进程 pid；失败返回负错误码；失败路径释放全部已经分配资源，无内存泄漏。
Requirements
1.调用get_pid()获取新 pid；
2.alloc_proc()分配子 PCB；失败返回‑E_NO_MEM；子进程填入 pid；proc->parent = current；
3.setup_kstack(proc)分配内核栈；失败则kfree释放 PCB，返回错误；
4.计算内核栈栈顶esp = proc->kstack + KSTACKSIZE；
5.copy_mm(clone_flags, proc)；失败回滚：put_kstack(proc) + kfree PCB；
6.copy_thread(proc, esp, tf)；
7.设置进程名字；proc->state = PROC_RUNNABLE；
8.插入proc_list全局链表；调用hash_proc(proc)加入 pid 哈希；nr_process++；
9.更新父子兄弟链表关系；
10.返回子进程 pid。
#kernel_thread
Pre‑Condition：fn线程入口函数、arg参数合法；clone_flags合法。
Post‑Condition：成功返回新建内核线程 pid；失败返回负错误码。
Requirements
1.在栈上构造临时伪造trapframe，把线程入口fn、参数arg填入上下文；内核线程无用户栈，stack 传 0；
2.内核线程使用CLONE_VM标志，调用do_fork(clone_flags | CLONE_VM, 0, &tf)；
3.返回do_fork的返回 pid。