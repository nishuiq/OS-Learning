#include "thread.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "memory.h"
#include "interrupt.h"
#include "print.h"
#include "debug.h"
#include "process.h"
#include "sync.h"

#define PG_SIZE 4096

struct task_struct* main_thread; // 主线程 PCB
struct list thread_ready_list; // 就绪队列
struct list thread_all_list; // 所有任务队列
static struct list_elem* thread_tag; // 保存队列的线程结点
struct lock pid_lock;


extern void switch_to(struct task_struct* cur, struct task_struct* next);

// 获取 PCB 指针
struct task_struct* running_thread() {
    uint32_t esp;
    asm ("mov %%esp, %0" : "=g"(esp));
    // 栈顶指针清空低12位 = - 0xfff 一页大小 就等于 pcb的起始地址 
    return (struct task_struct*)(esp & 0xfffff000);
}

// 分配pid
static pid_t allocate_pid(void) {
    static pid_t next_pid = 0;
    lock_acquire(&pid_lock);
    next_pid++;
    lock_release(&pid_lock);
    return next_pid;
}

static void kernel_thread(thread_func* function, void* func_arg) {
    intr_enable(); // 打开中断 避免后面时钟中断被屏蔽 无法调度其他线程
    function(func_arg);
}

// 初始化线程栈
void thread_create(struct task_struct* pthread, thread_func function, void* func_arg) {
    // 预留中断栈空间
    pthread->self_kstack -= sizeof(struct intr_stack);
    // 预留线程栈空间
    pthread->self_kstack -= sizeof(struct thread_stack);
    struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
    kthread_stack->eip = kernel_thread;
    kthread_stack->function = function;
    kthread_stack->func_arg = func_arg;
    kthread_stack->ebp = kthread_stack->ebx = kthread_stack->esi = kthread_stack->edi = 0;
}

// 初始化线程基本信息
void init_thread(struct task_struct* pthread, char* name, int prio) {
    memset(pthread, 0, sizeof(*pthread));
    pthread->pid = allocate_pid();
    strcpy(pthread->name, name);
    if (pthread == main_thread) {
        pthread->status = TASK_RUNNING;
    } else {
        pthread->status = TASK_READY;
    }

    pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE);
    pthread->priority = prio;
    pthread->ticks = prio;
    pthread->elapsed_ticks = 0;
    pthread->pgdir = NULL;

    pthread->stack_magic = 0x19870916; // 自定义数 作为标记防止栈溢出
}

struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg) {
    struct task_struct* thread = get_kernel_pages(1);
    init_thread(thread, name, prio);
    thread_create(thread, function, func_arg);

    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    list_append(&thread_ready_list, &thread->general_tag);

    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    list_append(&thread_all_list, &thread->all_list_tag);

    // asm volatile("movl %0, %%esp; pop %%ebp; pop %%ebx; pop %%edi; pop %%esi; 
    // ret": : "g"(thread->self_kstack) : "memory");
    return thread;
}

static void make_main_thread(void) {
    main_thread = running_thread();
    init_thread(main_thread, "main", 31);

    ASSERT(!elem_find(&thread_all_list, &main_thread->all_list_tag));
    list_append(&thread_all_list, &main_thread->all_list_tag);
}

void schedule() {
    ASSERT(intr_get_status() == INTR_OFF);
    struct task_struct* cur = running_thread();
    if (cur->status == TASK_RUNNING) {
        ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
        list_append(&thread_ready_list, &cur->general_tag);
        cur->ticks = cur->priority;
        cur->status = TASK_READY;
    }  //else {}// 如果不是因为时间片到期而被换下 说明是被阻塞了 不在就绪队列
    // 如果就绪队列为空 没有 可运行任务 就唤醒 idle
    // if (list_empty(&thread_ready_list)) {
    //     thread_unblock(idle_thread);
    // }
    ASSERT(!list_empty(&thread_ready_list));
    thread_tag = NULL;
    thread_tag = list_pop(&thread_ready_list);
    struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);
    next->status = TASK_RUNNING;
    
    // 激活任务页表
    process_activate(next);
    switch_to(cur, next);
}

void thread_init() {
    put_str("thread_init_start \n");
    list_init(&thread_ready_list);
    list_init(&thread_all_list);
    lock_init(&pid_lock);
    make_main_thread();
    //idle_thread = thread_start("idle", 10, idle, NULL);
    put_str("thread_init done\n");
}

// 当前线程自己阻塞自己
void thread_block(enum task_status stat) {
    ASSERT(((stat == TASK_BLOCKED) || (stat == TASK_WAITING) || (stat == TASK_HANGING)));
    enum intr_status old_status = intr_disable();
    struct task_struct* cur = running_thread();
    cur->status = stat;
    schedule();  // 换下处理器
    intr_set_status(old_status);
}

// 其他线程把被阻塞的线程唤醒
void thread_unblock(struct task_struct* pthread) {
    enum intr_status old_status = intr_disable();
    ASSERT(((pthread->status == TASK_BLOCKED) || (pthread->status == TASK_WAITING) || (pthread->status == TASK_HANGING)));
    if (pthread->status != TASK_READY) {
       if (elem_find(&thread_ready_list, &pthread->general_tag)) {
          PANIC("thread_unblock: blocked thread in the ready_list\n");
       }
       list_push(&thread_ready_list, &pthread->general_tag);
       pthread->status = TASK_READY;
    }
    intr_set_status(old_status);
}
