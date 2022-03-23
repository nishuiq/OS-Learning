%include "boot.inc"
section loader vstart=LOADER_BASE_ADDR
LOADER_STACK_TOP equ LOADER_BASE_ADDR

;构建 gdt 及其内部的描述符 ; 这里其实就是GDT的起始地址，第一个描述符为空
    GDT_BASE: dd 0x00000000
               dd 0x00000000
    CODE_DESC: dd 0x0000FFFF ; 代码段描述符，一个dd为4字节，段描述符为8字节，上面为低4字节
                dd DESC_CODE_HIGH4
    DATA_STACK_DESC: dd 0x0000FFFF ; 栈段描述符，和数据段共用
                        dd DESC_DATA_HIGH4
    ; 显卡段，非平坦
    VIDEO_DESC: dd 0x80000007;limit=(0xbffff-0xb8000)/4k=0x7 ;疑问 0xb8000->0xbffff b哪去了
                    dd DESC_VIDEO_HIGH4 ;此时 dpl 为 0
    GDT_SIZE equ $ - GDT_BASE
    GDT_LIMIT equ GDT_SIZE - 1
    times 60 dq 0 ; 此处预留 60 个描述符的空位
    ; 选择子 0x0000 第0个丢弃，从第1个GDT
    SELECTOR_CODE equ (0x0001<<3) + TI_GDT + RPL0 ; 相当于(CODE_DESC - GDT_BASE)/8 + TI_GDT + RPL0 
    SELECTOR_DATA equ (0x0002<<3) + TI_GDT + RPL0 ; 同上
    SELECTOR_VIDEO equ (0x0003<<3) + TI_GDT + RPL0 ; 同上

    ; total_mem_bytes 用于保存内存容量，以字节为单位，此位置比较好记
    ; 当前偏移 loader.bin 文件头 0x200 字节
    ; loader.bin 的加载地址是 0x900 
    ; 故 total_mem_bytes 内存中的地址是 0xb00 
    ; 将来在内核中咱们会引用此地址
    total_mem_bytes dd 0 
    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

    ;以下是 gdt 的指针，前 2 字节是 gdt 界限，后 4 字节是 gdt 起始地址
    gdt_ptr dw GDT_LIMIT 
             dd GDT_BASE 

    ; 手工对齐至0x300:total_mem_bytes 4 + 6 + 244 + 2 = 256 = 0x100
    ards_buf times 244 db 0
    ards_nr dw 0 ; ARDS结构体数量

loader_start:
; int 15h eax = 0000E820h ebx=0 edx=534D4150h ('SMAP') 获取内存布局
   xor ebx, ebx           ; 第一次调用时, ebx值要为0
   mov edx, 0x534d4150
   mov di, ards_buf       ; ARDS结构缓冲区
.e820_mem_get_loop:
   mov eax, 0x0000e820        ; eax会变0x534d4150 重置
   mov ecx, 20            ; ARDS地址范围描述符结构大小是20字节
   int 0x15
   jc .e820_failed_so_try_e801   ; CF 为1则有错
   add di, cx             ; di增加20字节指向缓冲区中新的ARDS结构位置
   inc word [ards_nr]         ; ARDS数量
   cmp ebx, 0             ; 若ebx为0 且 cf不为1 已是最后一个
   jnz .e820_mem_get_loop

   ;在所有 ards 结构中 
   ;找出(base_add_low + length_low)的最大值，即内存的容量
   mov cx, [ards_nr]
   ;遍历每一个 ARDS 结构体,循环次数是 ARDS 的数量
   mov ebx, ards_buf
   xor edx, edx     ; edx 为最大的内存容量，在此先清 0 
.find_max_mem_area: ; 无须判断type是否为1,最大的内存块一定是可被使用
   mov eax, [ebx] ; BaseAddrLow
   add eax, [ebx+8] ; LengthAddrLow
   add ebx, 20 ; 指向缓冲区中下一个ARDS结构
   cmp edx, eax
; 冒泡排序,找出最大,edx 寄存器始终是最大的内存容量
   jge .next_ards
   mov edx, eax ; edx 为总内存大小
.next_ards: 
   loop .find_max_mem_area
   jmp .mem_get_ok
;------ int 15h ax = E801h 获取内存大小，最大支持 4G ------ 
; 返回后, ax cx 值一样,以 KB 为单位，bx dx 值一样，以 64KB 为单位
; 在 ax 和 cx 寄存器中为低 16MB，在 bx 和 dx 寄存器中为 16MB 到 4GB 
.e820_failed_so_try_e801:
; int 15h ax = E801h 获取内存大小,最大支持4G 
; AX=CX KB低15MB BX=DX 64KB
   mov ax,0xe801
   int 0x15
   jc .e801_failed_so_try88 ;若当前 e801 方法失败，就尝试 0x88 方法
; 1.先算出低15M的内存
; ax 和 cx 中是以 KB 为单位的内存数量，将其转换为以 byte 为单位
   mov cx,0x400 ; 1k ;cx 和 ax 值一样，cx 用作乘数
   mul cx 
   shl edx,16
   and eax,0x0000FFFF ; 清空高4位
   or edx,eax
   add edx, 0x100000 ; 要加为扩展isa的预留内存 内存空洞的1MB ;ax 只是 15MB，故要加 1MB 
   mov esi,edx ;先把低 15MB 的内存容量存入 esi 寄存器备份
;2 再算16MB以上
; 寄存器 bx 和 dx 中是以 64KB 为单位的内存数量
   xor eax,eax
   mov ax,bx        
   mov ecx, 0x10000 ; 64KB
   mul ecx ;32 位乘法，默认的被乘数是 eax，积为 64 位
;高 32 位存入 edx，低 32 位存入 eax
   add esi,eax ; 最大4G 32位eax足够了 edx为0
;由于此方法只能测出 4GB 以内的内存，故 32 位 eax 足够了
; edx 肯定为 0，只加 eax 便可
   mov edx,esi ;edx 为总内存大小
   jmp .mem_get_ok
; int 15h ah = 0x88 获取内存大小,只能获取64M之内
.e801_failed_so_try88: 
   mov ah, 0x88
   int 0x15
   jc .error_hlt
   and eax,0x0000FFFF

;16 位乘法，被乘数是 ax，积为 32 位。积的高 16 位在 dx 中 ;积的低 16 位在 ax 中
   mov cx, 0x400 ; 单位 kb
   mul cx
   shl edx, 16 ; 把dx移到高16位
   or edx, eax ; 把积的低16位组合到edx,为32位的积
   add edx,0x100000 ; 0x88子功能只会返回1MB以上的内存,故实际内存大小要加上1MB

.mem_get_ok:
   mov [total_mem_bytes], edx

; -------------------- 准备进入保护模式 ------------------------------- 
;1 打开 A20 
;2 加载 gdt 
;3 将 cr0 的 pe 位置 1 


    ;----------------- 打开 A20 ---------------- 
    in al,0x92 
    or al,0000_0010B 
    out 0x92,al 

    ;----------------- 加载 GDT ---------------- 
    lgdt [gdt_ptr] 


    ;----------------- cr0 第 0 位置 1 ---------------- 
    mov eax, cr0 
    or eax, 0x00000001 
    mov cr0, eax 

    jmp dword SELECTOR_CODE:p_mode_start ; 刷新流水线
.error_hlt:
    hlt ; 失败了 挂起

[bits 32] 
p_mode_start: 
    mov ax, SELECTOR_DATA 
    mov ds, ax 
    mov es, ax 
    mov ss, ax 
    mov esp,LOADER_STACK_TOP 
    mov ax, SELECTOR_VIDEO 
    mov gs, ax 


; -------------------在分页之前加载 kernel--------------------
    mov eax, KERNEL_START_SECTOR ; kernel扇区号
    mov ebx, KERNEL_BIN_BASE_ADDR
    mov ecx, 200 ; 200个扇区
    call rd_disk_m_32

    ; 创建页目录及页表 ; 创建页目录及页表并初始化页内存位图
    call setup_page

    ; 将描述符地址及偏移量dump存入gdt_ptr ;要将描述符表地址及偏移量写入内存 gdt_ptr，一会儿用新地址重新加载
    sgdt [gdt_ptr]

    ; GDT VIDEO 段描述符段基址+0xc0000000
    ; VIDEO 是第三个段描述符 一个描述符8字节 24=0x18 +4 高4字节 是段基址的31~24
    mov ebx, [gdt_ptr+2] ; gdt_ptr前部分2字节大小的偏移量，后4字节大小GDT基址
    or dword [ebx+0x18+4], 0xc0000000
    add dword [gdt_ptr+2], 0xc0000000 ; gdt+0xc0000000 使其成为内核所在高地址

    ; 将栈指针映射到内核地址
    add esp, 0xc0000000

    ; 将页目录地址给cr3
    mov eax, PAGE_DIR_TABLE_POS
    mov cr3, eax

    ; 打开 cr0 PG位 31位
    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    ; 开启分页后 用GDT新的地址重新加载
    lgdt [gdt_ptr]
    mov byte [gs:160], 'N'
    jmp SELECTOR_CODE:enter_kernel ; 清空流水线
enter_kernel:
    call kernel_init
    mov esp, 0xc009f000
    jmp KERNEL_ENTRY_POINT

; ---------------将 kernel的segment拷贝到编译的地址-----------------
kernel_init:
    xor eax, eax
    xor ebx, ebx ; PROGRAM HEADER ADDRESS
    xor ecx, ecx ; e_phnum ;cx 记录程序头表中的 program header 数量
    xor edx, edx ; e_phentSize ;dx 记录 program header 尺寸

    mov dx, [KERNEL_BIN_BASE_ADDR + 42] ; e_phentsize
    mov ebx, [KERNEL_BIN_BASE_ADDR + 28] ; e_phoff program header 在文件中偏移
    add ebx, KERNEL_BIN_BASE_ADDR
    mov cx, [KERNEL_BIN_BASE_ADDR + 44] ; e_phnum
.each_segment:
    cmp byte [ebx + 0], PT_NULL
    je .PTNULL ; 若 p_type 等于 PT_NULL，说明此 program header 未使用
    
    push dword [ebx + 16] ; 压入参数(dst 目的地址, src 源地址, size)
    mov eax, [ebx + 4] ; p_offset
    add eax, KERNEL_BIN_BASE_ADDR ; eax 为该段物理地址
    push eax
    push dword [ebx + 8] ; p_vaddr
    call mem_cpy ; 段复制

    add esp, 12 ; 强行清理栈 ;三个参数
.PTNULL:
    add ebx, edx
    loop .each_segment
    ret
; mem_cpy(dst, src, size)
mem_cpy:
    cld ; clean direction eFlags DF置0 自动加上搬运字节大小
    push ebp
    mov ebp, esp
    push ecx
    mov edi, [ebp + 8] ; dst
    mov esi, [ebp + 12] ; src
    mov ecx, [ebp + 16] ; size
    rep movsb ; 逐字节拷贝

    ;恢复环境
    pop ecx
    pop ebp
    ret


; ---------------创建页目录及页表--------------
setup_page:
    ; 页目录空间清0 1024*4 ;先把页目录占用的空间逐字节清 0
    mov ecx, 4096
    mov esi, 0
.clear_page_dir:
    mov byte [PAGE_DIR_TABLE_POS+esi], 0
    inc esi
    loop .clear_page_dir
.create_pde: ; 创建 页目录项 PDE PAGE DIRECTORY ENTRY
    mov eax, PAGE_DIR_TABLE_POS
    add eax, 0x1000 ; 此时 eax 为第一个页表的位置及属性
    mov ebx, eax ; 此处为 ebx 赋值，是为.create_pte 做准备，ebx 为基址

; 下面将页目录项 0 和 0xc00 都存为第一个页表的地址，每个页表表示 4MB 内存
; 这样 0xc03fffff 以下的地址和 0x003fffff 以下的地址都指向相同的页表
; 这是为将地址映射为内核地址做准备
    or eax, PG_US_U | PG_RW_W | PG_P ; 属性7 111
; 页目录项的属性 RW 和 P 位为 1，US 为 1，表示用户属性，所有特权级别都可以访问 
    mov [PAGE_DIR_TABLE_POS+0x0], eax ; 第一个页目录项
; 在页目录表中的第 1 个目录项写入第一个页表的位置(0x101000)及属性(7) 
    mov [PAGE_DIR_TABLE_POS+0xc00], eax ; 0xc0/4 = 768个页表所占用目录项 3G边界的第一个页内存项
; 一个页表项占用 4 字节
; 0xc00 表示第 768 个页表占用的目录项，0xc00 以上的目录项用于内核空间
;也就是页表的 0xc0000000～0xffffffff 共计 1G 属于内核
; 0x0～0xbfffffff 共计 3G 属于用户进程
    sub eax, 0x1000
    mov [PAGE_DIR_TABLE_POS+4092], eax ; 页目录项最后一个指向页目录表本身 4字节要-4

    ;下面创建页表项(PTE) 
    mov ecx, 256 ; 这里只创建了 低端1M 创建页表项 PTE ; 1M 低端内存 / 每页大小 4k = 256 
    mov esi, 0
    mov edx, PG_US_U | PG_RW_W | PG_P ; 0x000007; 属性为 7，US=1，RW=1，P=1 
.create_pte: ; 创建 Page Table Entry 
    mov [ebx+esi*4], edx ; 为每个页表添加属性，不管其他内容0
; 此时的 ebx 已经在上面通过 eax 赋值为 0x101000，也就是第一个页表的地址
    inc esi
    add edx, 0x1000 ; 绕过一个4kb内存
    loop .create_pte

    ;创建内核其他页表的 PDE 
    mov eax, PAGE_DIR_TABLE_POS
    add eax, 0x2000 ; 此时 eax 为第二个页表的位置
    or eax, PG_US_U | PG_RW_W | PG_P ; 页目录项的属性 US､ RW 和 P 位都为 1 
    mov ebx, PAGE_DIR_TABLE_POS
    mov ecx, 254 ; 范围为第 769～1022 的所有目录项数量 ;[0,1023] 1023已被占用，用于指向了页目录表本身
    mov esi, 769 ; 769~1022
.create_kernel_pde:
    mov [ebx+esi*4], eax
    inc esi
    add eax, 0x1000
    loop .create_kernel_pde
    ret

; 读取硬盘N个扇区
rd_disk_m_32:
    mov esi, eax
    mov di, cx
    ; 写入扇区数
    mov dx, 0x1f2
    mov al, cl
    out dx, al
    mov eax, esi

    ; LBA 7~0 写入起始扇区
    inc dx ; 0x1f3
    out dx, al

    ; LBA 15~8
    inc dx ; 0x1f4
    mov cl, 8
    shr eax, cl
    out dx, al

    ; LBA 23~16
    inc dx ; 0x1f5
    shr eax, cl
    out dx, al

    ; LBA 27~24
    inc dx ; 0x1f6
    shr eax, cl
    and al, 0x0f ; 清空高4位
    or al, 0xe0 ; 设置 7~4位 1110 LBA模式
    out dx, al
    ; 0x1f7 写入 0x20读
    inc dx ; 0x1f7
    mov al, 0x20
    out dx, al
.not_ready:
    nop
    in al, dx
    and al, 0x88 ; 3位表示已准备好 7位表示硬盘忙
    cmp al, 0x08
    jnz .not_ready

    mov ax, di
    mov dx, 256 ; 一次读入一个2字节 512/2=256*扇区个数=循环次数
    mul dx
    mov cx, ax
    mov dx, 0x1f0
.go_on_read:
    in ax, dx
    mov [ebx], ax
    add ebx, 2 ; 16位情况下 扇区数过大 会导致 栈被覆盖
    loop .go_on_read
    ret