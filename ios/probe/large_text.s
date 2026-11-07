// Padding as large as the translated game's code, to find out whether a device will load an executable that big.
// In pieces, so that the linker can place its branch islands between them as it does for the real code.
    .section __TEXT,__text,regular,pure_instructions
    .globl _probe_large_text_start
    .no_dead_strip _probe_large_text_start
    .p2align 14
_probe_large_text_start:
    .space 0x4000000
    .globl _probe_pad1
    .no_dead_strip _probe_pad1
    .p2align 14
_probe_pad1:
    .space 0x4000000
    .globl _probe_pad2
    .no_dead_strip _probe_pad2
    .p2align 14
_probe_pad2:
    .space 0x4000000
    .globl _probe_pad3
    .no_dead_strip _probe_pad3
    .p2align 14
_probe_pad3:
    .space 0x4000000
    .globl _probe_pad4
    .no_dead_strip _probe_pad4
    .p2align 14
_probe_pad4:
    .space 0x4000000
    .globl _probe_large_text_end
    .p2align 2
_probe_large_text_end:
    .long 0x600DC0DE
    .subsections_via_symbols
