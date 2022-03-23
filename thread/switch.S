; 保护 esi edi ebx ebp 让任务恢复执行在任务切换时
; 剩下的尚未执行的内核代码
[bits 32]
section .text
global switch_to
switch_to:
    ; 返回地址
    push esi
    push edi
    push ebx
    push ebp
    mov eax, [esp + 4 * 5] ; 栈中参数 cur
    mov [eax], esp ; 保存esp task_struct self_kstack
; 恢复下一线程
    mov eax, [esp + 24] ; next
    mov esp, [eax] ; 下一个pcb的self_kstack
    ; 记录了 0 级栈指针
    pop ebp
    pop ebx
    pop edi
    pop esi
    ret