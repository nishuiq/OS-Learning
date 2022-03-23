#ifndef __THREAD_THREAD_H
#define __THREAD_THREAD_H
#include "stdint.h"
#include "list.h"
#include "memory.h"

typedef void thread_func(void*);
typedef int16_t pid_t;

// 线程进程状态
enum task_status {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_WAITING,
    TASK_HANGING,
    TASK_DIED
};

// 中断栈 中断时 保护上下文环境
struct intr_stack {
    uint32_t vec_no; // 中断号
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy; // popad 会忽略掉 esp
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;
    // 低特权级向高特权级转移时压入
    uint32_t error_code; // err_code 会被压入在 eip 之后
    void (*eip)(void);
    uint32_t cs;
    uint32_t eflags;
    void* esp;
    uint32_t ss;
};


// 线程栈
struct thread_stack {
    // ABI ebp ebx edi esi esp 归主调函数 
    uint32_t ebp;
    uint32_t ebx;
    uint32_t edi;
    uint32_t esi;

    // 线程要执行的函数 作为ret返回地址
    void (*eip)(thread_func* func, void* func_arg);
    // 占位的返回地址
    void (*unused_retaddr);
    thread_func* function;
    void* func_arg;
};

// PCB
struct task_struct {
    uint32_t* self_kstack; // 内核栈
    pid_t pid;
    enum task_status status;
    char name[16];
    uint8_t priority;
    uint8_t ticks; // 每次在cpu上执行的嘀嗒数
    uint32_t elapsed_ticks; // 运行了多久

    struct list_elem general_tag; // 线程一般队列结点
    struct list_elem all_list_tag; // 线程队列结点
    uint32_t* pgdir; // 进程页表虚拟地址
    struct virtual_addr userprog_vaddr; // 用户进程的虚拟地址
    struct mem_block_desc u_block_desc[DESC_CNT]; // 用户进程内存块描述符
    uint32_t stack_magic; // 栈的边界标记
};


void thread_create(struct task_struct* pthread, thread_func function, void* func_arg);
void init_thread(struct task_struct* pthread, char* name, int prio);
struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg);
struct task_struct* running_thread(void);
void schedule(void);
void thread_init(void);
void thread_block(enum task_status stat);
void thread_unblock(struct task_struct* pthread);

// 不知道是干嘛的!好像知道哦
extern struct list thread_ready_list;
extern struct list thread_all_list;

#endif