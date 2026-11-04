/*
 * export.S -- the ROM service table, 0x7FB8..0x7FFF (data_33, data_34).
 *
 * The loader (Novatek bl_func.c, ROM_LZMA_POSITION and friends) calls
 * ROM routines through these fixed slots instead of linking against
 * addresses, so the slots, not the functions, are the ROM's ABI.
 * link.ld pins section .export at 0x7FB8 and provides the three const
 * bounds; the 0xFF fill before it is the linker's.
 *
 * 0x7FC4..0x7FCC are not functions: they are the start of the const data
 * (USB descriptors), the start of the frame templates / SCSI replies /
 * strings, and the end of the const data (inferred from where they
 * point).  0x7FEC repeats put_hex32 (0x7FC0).  0x7FFC = 0x116 is
 * presumably the ROM version, 1.16 (inferred).
 */
    .section .export, "a"
    .align  2

    .word   0xffffffff                  @ 0x7FB8 empty slot (vprintf on NA51103)
    .word   sha256                      @ 0x7FBC 0x6d68 sha256(data, len, out)
    .word   put_hex32                   @ 0x7FC0 0x6d20 put_hex32(v)
    .word   __const_start               @ 0x7FC4 0x78d8 const data start
    .word   __const_frames_start        @ 0x7FC8 0x7a50 frames/strings start
    .word   __const_end                 @ 0x7FCC 0x7d68 const data end
    .word   lzma_inflate                @ 0x7FD0 0x716c
    .word   LZ_Uncompress               @ 0x7FD4 0x6f2c
    .word   debug_msg                   @ 0x7FD8 0x700c debug_msg(str)
    .word   debug_msg_var               @ 0x7FDC 0x7144 debug_msg_var(str, v)
    .word   debug_msg_err               @ 0x7FE0 0x7114 debug_msg_err(str, v)
    .word   efuse_read                  @ 0x7FE4 0x6eb4 efuse_read(idx)
    .word   memcpy32                    @ 0x7FE8 0x16e8 memcpy32(dst, src, n)
    .word   put_hex32                   @ 0x7FEC 0x6d20 again

    /* data_34 */
    .word   0xffffffff                  @ 0x7FF0
    .ascii  "REAL"                      @ 0x7FF4 tag
    .word   0x00000000                  @ 0x7FF8
    .word   0x00000116                  @ 0x7FFC version (inferred)
