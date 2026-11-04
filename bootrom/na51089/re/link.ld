/*
 * link.ld -- NA51089 mask ROM: 32 KiB at 0x00000000.
 *
 * Layout of the original image (bootrom.txt):
 *   0x0000..0x031f  start.S (vectors, reset, dispatch, loader check)
 *   0x0320..0x78d7  C code
 *   0x78d8..0x7a4f  const: USB descriptors and strings     (usb.o)
 *   0x7a50..0x7d67  const: frame templates (net.o), SCSI replies
 *                   (usb_msc.o), LZMA/debug strings (lzma.o, console.o)
 *   ..0x7fb7        0xFF fill
 *   0x7fb8..0x7fff  .export, the service table (export.S)
 *
 * export.S emits the three const bounds at 0x7FC4/0x7FC8/0x7FCC from
 * __const_start, __const_frames_start and __const_end below.  The ROM has
 * no initialised or zeroed data: .data and .bss must stay empty (globals
 * live at fixed SRAM addresses, see sram.h).
 */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY
{
    rom (rx) : ORIGIN = 0x00000000, LENGTH = 0x8000
}

SECTIONS
{
    .rom 0x00000000 :
    {
        KEEP(re/start.o(.text*))
        *(.text*)

        . = ALIGN(4);
        __const_start = .;
        re/usb.o(.rodata*)
        . = ALIGN(4);
        __const_frames_start = .;
        re/net.o(.rodata*)
        re/usb_msc.o(.rodata*)
        re/lzma.o(.rodata*)
        *(.rodata*)
        . = ALIGN(4);
        __const_end = .;

        ASSERT(. <= 0x7FB8, "code + const overlap the export table at 0x7FB8");
        . = 0x7FB8;
        __export_start = .;
        KEEP(*(.export))
        ASSERT(. <= 0x8000, "export table runs past 32 KiB");
        . = 0x8000;
    } > rom =0xFF

    .data : { *(.data*) } > rom
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } > rom

    ASSERT(SIZEOF(.data) == 0, "the ROM has no .data: use sram.h fixed addresses")
    ASSERT(SIZEOF(.bss) == 0, "the ROM has no .bss: use sram.h fixed addresses")
    ASSERT(__export_start == 0x7FB8, ".export must start at 0x7FB8")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
