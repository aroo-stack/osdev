CC = gcc
AS = nasm
CFLAGS = -m32 -ffreestanding -fno-pie -nostdlib -Wall -Wextra
LDFLAGS = -m elf_i386 -T linker.ld

all: os.iso

boot.o: boot.s
	$(AS) -f elf32 boot.s -o boot.o

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o

gdt.o: gdt.c gdt.h
	$(CC) $(CFLAGS) -c gdt.c -o gdt.o

gdt_flush.o: gdt_flush.s
	$(AS) -f elf32 gdt_flush.s -o gdt_flush.o

idt.o: idt.c idt.h
	$(CC) $(CFLAGS) -c idt.c -o idt.o

isr.o: isr.s
	$(AS) -f elf32 isr.s -o isr.o

pmm.o: pmm.c pmm.h multiboot.h
	$(CC) $(CFLAGS) -c pmm.c -o pmm.o

paging.o: paging.c paging.h
	$(CC) $(CFLAGS) -c paging.c -o paging.o

heap.o: heap.c heap.h
	$(CC) $(CFLAGS) -c heap.c -o heap.o

kernel.bin: boot.o kernel.o gdt.o gdt_flush.o idt.o isr.o pmm.o paging.o heap.o linker.ld
	ld $(LDFLAGS) -o kernel.bin boot.o kernel.o gdt.o gdt_flush.o idt.o isr.o pmm.o paging.o heap.o

os.iso: kernel.bin
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "MyOS" {' >> isodir/boot/grub/grub.cfg
	echo '  multiboot /boot/kernel.bin' >> isodir/boot/grub/grub.cfg
	echo '  boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o os.iso isodir

run:
	qemu-system-i386 -cdrom os.iso -nographic

clean:
	rm -rf *.o kernel.bin os.iso isodir
