/* Places the multiboot header first and loads the kernel where a multiboot
   loader expects it. Used as:
     ld -m elf_i386 -T demos/os/link.ld kernel.o -o kernel.elf */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }
}
