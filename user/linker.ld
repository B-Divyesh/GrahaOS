/* user/linker.ld */
/* Linker script for GrahaOS user programs */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

/* User program base address - standard for x86_64 user programs */
. = 0x400000;

SECTIONS
{
    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(COMMON)
        *(.bss .bss.*)
    }

    /* Discard debug sections and other unnecessary sections */
    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.dynamic .dynsym .dynstr .hash .gnu.hash)
        *(.comment)
    }
}