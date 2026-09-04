MBALIGN  equ  1<<0
MEMINFO  equ  1<<1
VIDEOMODE equ 1<<2
FLAGS    equ  MBALIGN | MEMINFO | VIDEOMODE
MAGIC    equ  0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

; Trace per OSDev Bare Bones framebuffer example:
; Flags 0x07 (bits 0,1,2 set), header 12B base + 20B address kludge (5 dwords 0) + 16B video
; Even though ADDR flag (bit16) not set, GRUB expects 5 address dwords when VIDEO flag set in many builds
; So we include them as zeros (header_addr etc. unused) to match common working layout.
; Then video: mode_type 0=graphic, width 1024, height 768, depth 32
; Checksum covers only magic+flags, not extra fields.

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM
    dd 0, 0, 0, 0, 0   ; header_addr, load_addr, load_end_addr, bss_end_addr, entry_addr (unused)
    dd 0              ; mode_type 0 graphic
    dd 1024           ; width
    dd 768            ; height
    dd 32             ; depth

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
global _start
extern kernel_main
extern _bss_start
extern _bss_end
_start:
    ; Save multiboot magic (EAX) and info (EBX) before clobbering EAX for BSS zeroing
    push eax
    push ebx
    ; Zero .bss - GRUB loads SHT_NOBITS but does NOT guarantee zero on QEMU reset with leftover RAM
    cld
    mov edi, _bss_start
    mov ecx, _bss_end
    sub ecx, edi
    shr ecx, 2
    xor eax, eax
    rep stosd
    pop ebx
    pop eax
    mov esp, stack_top
    push ebx
    push eax
    call kernel_main
    add esp, 8
    cli
.hang:
    hlt
    jmp .hang
