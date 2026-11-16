/* RA PPC agent: linked at its MEM2 home (rappc_shared.h RAPPC_BASE_EA). The
 * header must be the first bytes of the image; the Starlet loads the binary
 * there and zeroes the rest of the area (.bss included). */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(ra_ppc_entry)

SECTIONS {
	. = 0x93170000;
	.image : {
		KEEP(*(.rahead))
		. = ALIGN(32);
		*(.text .text.*)
		*(.rodata .rodata.*)
		*(.data .data.*)
		*(.sdata .sdata.* .sdata2 .sdata2.* .sbss .sbss.*)
		. = ALIGN(32);
	}
	.bss (NOLOAD) : {
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(32);
		ra_ppc_image_end = .;
	}
	/DISCARD/ : { *(.comment) *(.eh_frame) *(.gnu.attributes) }
}
