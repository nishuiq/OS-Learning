#include "init.h"
#include "print.h"
#include "interrupt.h"
#include "../device/timer.h"
#include "memory.h"
#include "thread.h"
#include "console.h"
#include "keyboard.h"
#include "tss.h"
#include "syscall-init.h"

void init_all() {
    put_str("init_all\n");
    idt_init(); // 初始化中断
    mem_init(); // 初始化内存管理
    timer_init(); // 初始化 PIF
    thread_init(); // 初始化线程
    console_init(); // 控制台初始化
    keyboard_init(); // 初始化键盘 忘记初始化了。。。。
    tss_init(); // 初始化 TSS
    syscall_init(); // 初始化系统调用
}