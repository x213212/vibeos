.equ STACK_SIZE, 65536 # 提升到 64KB

.global _start

_start:
    csrr a0, mhartid
    bnez a0, park
    
    la   sp, stacks + STACK_SIZE
    andi sp, sp, -16

    la a0, _bss_start
    la a1, _bss_end
    bgeu a0, a1, skip_bss
bss_loop:
    sw zero, 0(a0)
    addi a0, a0, 4
    bltu a0, a1, bss_loop
skip_bss:

    j    os_main

park:
    wfi
    j park

stacks:
    .skip STACK_SIZE
