;------------- loader 和 kernel ---------- 
LOADER_BASE_ADDR equ 0x900  ; 说明将来 loader 会在内存地址 0x900 处
LOADER_START_SECTOR equ 0x2 ; 说明 loader 放在了第 2 块扇区
KERNEL_START_SECTOR equ 0x9
KERNEL_BIN_BASE_ADDR equ 0x70000
KERNEL_ENTRY_POINT equ 0xc0001500 ; 0x1500作为内核映像的入口地址
;-------------------页表---------------------
PAGE_DIR_TABLE_POS equ 0x100000
;-------------- GDT 描述符属性 ------------- 
DESC_G_4K equ 1_00000000000000000000000b ; 设置段界限粒度为4k字节
DESC_D_32 equ  1_0000000000000000000000b ; 设置 段内偏移地址及操作数大小为32位
DESC_L    equ   0_000000000000000000000b ; 64 位代码标记,此处标记为 0 便可
DESC_AVL  equ    0_00000000000000000000b ; CPU 不用此位,暂置为 0 
DESC_LIMIT_CODE2 equ 1111_0000000000000000b ; 设置第二段段界限
DESC_LIMIT_DATA2 equ DESC_LIMIT_CODE2 
DESC_LIMIT_VIDEO2 equ 0000_000000000000000b 
DESC_P equ 1_000000000000000b ; 段是否存于内存
; 特权级
DESC_DPL_0 equ 00_0000000000000b
DESC_DPL_1 equ 01_0000000000000b
DESC_DPL_2 equ 10_0000000000000b
DESC_DPL_3 equ 11_0000000000000b
; 设置系统/代码段
DESC_S_CODE equ 1_000000000000b ; 1数据段 0系统段(凡是硬件运行需要用到的东西都可称之为系统) 
DESC_S_DATA equ DESC_S_CODE 
DESC_S_sys equ 0_000000000000b 
; P164 解析有问题: 表4-10中非系统段的R和C列位置互换、W和E列位置互换
DESC_TYPE_CODE equ 1000_00000000b ;x=1,c=0,r=0,a=0 代码段是可执行的，非一致性，不可读，已访问位 a 清 0 
DESC_TYPE_DATA equ 0010_00000000b ;x=0,e=0,w=1,a=0 数据段是不可执行的，向上扩展的，可写，已访问位 a 清 0 

DESC_CODE_HIGH4 equ (0x00 << 24) + DESC_G_4K + DESC_D_32 + \
DESC_L + DESC_AVL + DESC_LIMIT_CODE2 + \
DESC_P+DESC_DPL_0 + DESC_S_CODE +\
DESC_TYPE_CODE + 0x00

DESC_DATA_HIGH4 equ (0x00 << 24) + DESC_G_4K + DESC_D_32 +\
DESC_L + DESC_AVL + DESC_LIMIT_DATA2 + \
DESC_P + DESC_DPL_0 + DESC_S_DATA + \
DESC_TYPE_DATA + 0x00

DESC_VIDEO_HIGH4 equ (0x00 << 24) + DESC_G_4K + DESC_D_32 +\
DESC_L + DESC_AVL + DESC_LIMIT_VIDEO2 + DESC_P + \
DESC_DPL_0 + DESC_S_DATA + DESC_TYPE_DATA + 0x0b
;-------------- 选择子属性 --------------- 
RPL0 equ 00b 
RPL1 equ 01b 
RPL2 equ 10b 
RPL3 equ 11b 
TI_GDT equ 000b 
TI_LDT equ 100b
;----------------页表属性-----------
PG_P equ 1b
PG_RW_R equ 00b
PG_RW_W equ 10b
PG_US_S equ 000b
PG_US_U equ 100b
;--------------Program type ---------
PT_NULL equ 0 ; 忽略