CC = riscv64-unknown-elf-gcc
OBJCOPY = riscv64-unknown-elf-objcopy
CFLAGS = -nostdlib -ffreestanding -fno-builtin -fno-pic -fno-pie -mcmodel=medany -march=rv32imac -mabi=ilp32 -O2 -Wall -Wextra
LDFLAGS = -T app.ld -nostdlib

all: test.bin hello.elf fault.bin writefault.bin jumpfault.bin selfmem.bin

hello.elf: crt0.c hello.c app.h app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.c hello.c

test.bin: hello.elf
	cp $< $@

fault.elf: crt0.c fault.c app.h app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.c fault.c

fault.bin: fault.elf
	$(OBJCOPY) -O binary $< $@

writefault.elf: crt0.c writefault.c app.h app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.c writefault.c

writefault.bin: writefault.elf
	$(OBJCOPY) -O binary $< $@

jumpfault.elf: crt0.c jumpfault.c app.h app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.c jumpfault.c

jumpfault.bin: jumpfault.elf
	$(OBJCOPY) -O binary $< $@

selfmem.elf: crt0.c selfmem.c app.h app.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ crt0.c selfmem.c

selfmem.bin: selfmem.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f hello.elf test.bin fault.elf fault.bin writefault.elf writefault.bin jumpfault.elf jumpfault.bin selfmem.elf selfmem.bin
