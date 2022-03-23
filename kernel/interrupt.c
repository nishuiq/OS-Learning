#include "interrupt.h"
#include "stdint.h"
#include "global.h"
#include "io.h"
#include "print.h"

#define IDT_DESC_CNT 0x81 // 总共支持的中断数

#define PIC_M_CTRL 0x20 // 主片的控制端口是 0x20 
#define PIC_M_DATA 0x21 // 主片的数据端口是 0x21 
#define PIC_S_CTRL 0xA0 // 从片的控制端口是 0xa0 
#define PIC_S_DATA 0xA1 // 从片的数据端口是 0xa1 

#define EFLAG_IF 0x00000200 // EFLAGS 寄存器 第9 if 位为1
#define GET_EFLAGS(EFLAG_VAR) asm volatile("pushfl; popl %0" : "=g"(EFLAG_VAR)) // =g 可存放到任意寄存器或内存

extern uint32_t syscall_handler(void);

// 中断门描述符结构体
struct gate_desc {
    uint16_t func_offset_low_word;
    uint16_t selector;
    uint8_t dcount; // 高32位里的 0~7 位都是0
    uint8_t attribute;
    uint16_t func_offset_high_word;
};

static void make_idt_desc(struct gate_desc* p_gdesc, uint8_t attr, intr_handler function);
static struct gate_desc idt[IDT_DESC_CNT]; // 中断门描述符


char* intr_name[IDT_DESC_CNT]; // 中断名
intr_handler idt_table[IDT_DESC_CNT];
//定义中断处理程序数组，在 kernel.S 中定义的 intrXXentry 
//只是中断处理程序的入口，最终调用的是 idt_table 中的处理程序
extern intr_handler intr_entry_table[IDT_DESC_CNT]; // 中断处理程序 // 声明引用定义在 kernel.S

static void make_idt_desc(struct gate_desc* p_gdesc, uint8_t attr, intr_handler function) {
    p_gdesc->func_offset_low_word = (uint32_t)function & 0x0000FFFF;
    p_gdesc->selector = SELECTOR_K_CODE;
    p_gdesc->dcount = 0;
    p_gdesc->attribute = attr;
    p_gdesc->func_offset_high_word = ((uint32_t)function & 0xFFFF0000) >> 16;
}

// 初始化 可编程 8259A 中断控制器
static void pic_init(void) {
    // 初始化主片
    outb (PIC_M_CTRL, 0x11); // ICW1: 边沿触发,级联 x86必须要 ICW4
    outb (PIC_M_DATA, 0x20); // ICW2: 起始中断向量号为 0x20 起始中断向量为 32
     // 也就是 IR[0-7] 为 0x20 ～ 0x27 
    outb (PIC_M_DATA, 0x04); // ICW3: IR2 接从片
    outb (PIC_M_DATA, 0x01); // ICW4: 8086 模式 手动EOI 

    // 初始化从片
    outb (PIC_S_CTRL, 0x11); // ICW1: 边沿触发,级联 8259, 需要 ICW4 
    outb (PIC_S_DATA, 0x28); // ICW2: 起始中断向量号为 0x28 
     // 也就是 IR[8-15]为 0x28 ～ 0x2F 
    outb (PIC_S_DATA, 0x02); // ICW3: 设置从片连接到主片的 IR2 引脚
    outb (PIC_S_DATA, 0x01); // ICW4: 8086 模式, 正常 EOI 

    // 打开主片上 IR0,也就是目前只接受时钟产生的中断 
    // outb (PIC_M_DATA, 0xfe); 
    // outb (PIC_S_DATA, 0xff);

    // 测试键盘，只打开键盘中断，其他全部关闭
    // 位1置0 键盘中断0xfd 0xff屏蔽从片 0xfc=时钟和键盘打开
    outb (PIC_M_DATA, 0xfc); 
    outb (PIC_S_DATA, 0xff);

    put_str("   pic_init done\n");
}

static void idt_desc_init(void) {
    int i, lastindex = IDT_DESC_CNT - 1;
    for (i = 0; i < IDT_DESC_CNT; ++i) {
        make_idt_desc(&idt[i], IDT_DESC_ATTR_DPL0, intr_entry_table[i]);
    }
    make_idt_desc(&idt[lastindex], IDT_DESC_ATTR_DPL3, syscall_handler);
    put_str("   idt_desc_init done\n");
}


// 中断处理函数
static void general_intr_handler(uint8_t vec_nr) {
    if (vec_nr == 0x27 || vec_nr == 0x2f) { return; } // IRQ7 和 IRQ15会产生伪中断 无需处理
    set_cursor(0); // 光标 置0
    int cursor_pos = 0;
    while (cursor_pos < 320) {
        put_char(' ');
        cursor_pos++;
    }
    set_cursor(0);
    put_str("!!!!!!!      excetion message begin  !!!!!!!!\n");
    set_cursor(88);
    put_str(intr_name[vec_nr]);
    if (vec_nr == 14) { // page fault 将缺失地址打印出来并中断
        int page_fault_vaddr = 0;
        asm ("movl %%cr2, %0 " : "=r"(page_fault_vaddr)); // cr2 存造成 page_fault 的地址
        put_str("\npage fault addr is ");
        put_int(page_fault_vaddr);
    }
    put_str("\n!!!!!!!      excetion message end    !!!!!!!!\n");
    // 进入中断后 eFlags IF 置 0 关闭中断
    while (1);
}

static void exception_init(void) {
    int i;
    for (i = 0; i < IDT_DESC_CNT; ++i) {
        idt_table[i] = general_intr_handler;
        intr_name[i] = "unknown";
    }
    intr_name[0] = "#DE Divide Error";
    intr_name[1] = "#DB Debug Exception";
    intr_name[2] = "NMI Interrupt";
    intr_name[3] = "#BP Breakpoint Exception";
    intr_name[4] = "#OF Overflow Exception";
    intr_name[5] = "#BR BOUND Range Exceeded Exception";
    intr_name[6] = "#UD Invalid Opcode Exception";
    intr_name[7] = "#NM Device Not Available Exception";
    intr_name[8] = "#DF Double Fault Exception";
    intr_name[9] = "Coprocessor Segment Overrun";
    intr_name[10] = "#TS Invalid TSS Exception";
    intr_name[11] = "#NP Segment Not Present";
    intr_name[12] = "#SS Stack Fault Exception";
    intr_name[13] = "#GP General Protection Exception";
    intr_name[14] = "#PF Page-Fault Exception";
    // intr_name[15] intel保留项，未使用
    intr_name[16] = "#MF x87 FPU Floating-Point Error";
    intr_name[17] = "#AC Alignment Check Exception";
    intr_name[18] = "#MC Machine-Check Exception";
    intr_name[19] = "#XF SIMD Floating-Point Exception";

    //intr_name[33] = "33 0x21";  //0x21 键盘中断，
}

void idt_init() {
    put_str("idt_init start\n");
    idt_desc_init(); // 初始化中断描述符表
    exception_init(); // 异常名初始化并注册通常的中断处理函数
    pic_init(); // 初始化 8259A

    // 加载 idt
    uint64_t idt_operand = ((sizeof(idt) - 1) | ((uint64_t)(uint32_t)idt << 16));
    asm volatile("lidt %0" : : "m"(idt_operand));
    put_str("idt_init done\n");
}

/* 开中断并返回开中断前的状态*/
enum intr_status intr_enable() {
    enum intr_status old_status;
    if (INTR_ON == intr_get_status()) {
        old_status = INTR_ON;
        return old_status;
    } else {
        old_status = INTR_OFF;
        asm volatile("sti"); // 打开中断 IF 置1
        return old_status;
    }
}
/* 关中断，并且返回关中断前的状态 */
enum intr_status intr_disable() {
    enum intr_status old_status;
    if (INTR_ON == intr_get_status()) {
        old_status = INTR_ON;
        asm volatile("cli": : :"memory");
        return old_status;
    } else {
        old_status = INTR_OFF;
        return old_status;
    }
}
/* 将中断状态设置为 status */
enum intr_status intr_set_status(enum intr_status status) {
    return status & INTR_ON ? intr_enable() : intr_disable();
}
/* 获取当前中断状态 */
enum intr_status intr_get_status() {
    uint32_t eflags = 0;
    GET_EFLAGS(eflags);
    return (EFLAG_IF & eflags) ? INTR_ON : INTR_OFF;
}

// 为第vector_no个中断处理程序 安装中断处理程序 function
void register_handler(uint8_t vector_no, intr_handler function) {
    idt_table[vector_no] = function;
}