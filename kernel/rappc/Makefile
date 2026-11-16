#---------------------------------------------------------------------------------
# RA PPC agent (Broadway side) -> rappc.h (embedded in the kernel, installed
# into MEM2 by kernel/ra_module.c). Freestanding: no libc, no small data (the
# game owns r2/r13), no floating point.
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif
include $(DEVKITPPC)/wii_rules

ifeq ($(OS),Windows_NT)
	BIN2H =	../bin2h/bin2h.exe
else
	BIN2H =	../bin2h/bin2h
endif

CFLAGS  := -O2 -Wall -mcpu=750 -meabi -msoft-float -mno-sdata -G0 \
           -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns \
           -fno-jump-tables -fno-stack-protector -nostdlib -nostartfiles
LDFLAGS := -T link.ld -nostdlib -nostartfiles -Wl,--no-relax

.PHONY: all clean
all: rappc.h

rappc.elf: rappc.c ra_ppc_entry.S rappc_shared.h ../gc_ra_protocol.h link.ld
	@echo " COMPILE     rappc"
	@$(CC) $(CFLAGS) -c -o rappc.o rappc.c
	@$(CC) $(CFLAGS) -x assembler-with-cpp -c -o ra_ppc_entry.o ra_ppc_entry.S
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ rappc.o ra_ppc_entry.o

rappc.bin: rappc.elf
	@$(OBJCOPY) -O binary -j .image $< $@

rappc.h: rappc.bin
	@echo " BIN2H       $<"
	@$(BIN2H) $<

clean:
	-$(RM) rappc.o ra_ppc_entry.o rappc.elf rappc.bin rappc.h
