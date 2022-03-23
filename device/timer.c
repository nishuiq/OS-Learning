#include "timer.h"
#include "../lib/kernel/io.h"                                          
#include "../lib/kernel/print.h"
#include "../thread/thread.h"

#include "debug.h"
#include "interrupt.h"
#include "stdint.h"

#define IRQ0_FREQUENCY      100 // 时钟中断频率 100hz
#define INPUT_FREQUENCY     1193180 // 计时器脉冲信号频率
#define COUNTER0_VALUE      INPUT_FREQUENCY / IRQ0_FREQUENCY // 初值
#define COUNTER0_PORT       0x40 // 计数器端口
#define COUNTER0_NO         0 // 计数器编号
#define COUNTER_MODE        2 // 工作模式 比率发生器
#define READ_WRITE_LATCH    3 // 读写控制 先读低8位 后读高8位
#define PIT_CONTROL_PORT    0x43 // 控制器控制端口

uint32_t ticks; // 内核开启中断以来 总共的嘀嗒数

/* 时钟的中断处理函数 */
static void intr_timer_handler(void) {
    struct task_struct* cur_thread = running_thread();
    ASSERT(cur_thread->stack_magic == 0x19870916);  // 检查栈是否溢出
    cur_thread->elapsed_ticks++;  // 记录此线程占用的 cpu 时间
    ticks++;
    if (cur_thread->ticks == 0) {
        schedule(); 
    } else {
        cur_thread->ticks--;
    }
}

static void frequency_set(uint8_t counter_port, uint8_t counter_no, uint8_t rwl, uint8_t counter_mode, uint16_t counter_value) {
    outb(PIT_CONTROL_PORT, (uint8_t)(counter_no << 6 | rwl << 4 | counter_mode << 1));
    // 写入 counter_value 低8位
    outb(counter_port, (uint8_t)counter_value);
    // 写入 counter_value 高8位
    outb(counter_port, (uint8_t)counter_value >> 8);
}

// 初始化 PIT8253
void timer_init() {
    put_str("timer init start\n");
    // 设置 8253 定时周期
    frequency_set(COUNTER0_PORT, COUNTER0_NO, READ_WRITE_LATCH, COUNTER_MODE, COUNTER0_VALUE);
    register_handler(0x20, intr_timer_handler);
    put_str("timer init done\n");
}