#ifndef __LIB_IO_H
#define __LIB_IO_H
#include "stdint.h"

// 向端口 写一个字节
static inline void outb(uint16_t port, uint8_t data) {
    // 机器模式 b 寄存器低8位al w寄存器2个字节dx q一个字节 N: 0~255立即数
    asm volatile("outb %b0, %w1": : "a"(data), "Nd"(port));
}
// 将addr的n个字 写入 port
static inline void outsw(uint16_t port, const void* addr, uint32_t word_cnt) {
    asm volatile("cld; rep outsw": "+S"(addr), "+c"(word_cnt): "d"(port));
}
// 读一个字节
static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    asm volatile("inb %w1, %b0": "=a"(data): "Nd"(port));
    return data;
}
static inline void insw(uint16_t port, const void* addr, uint32_t word_cnt) {
    asm volatile ("cld; rep insw" : "+D" (addr), "+c" (word_cnt): "d" (port) : "memory");
}

#endif