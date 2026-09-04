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

framebuffer.o: framebuffer.c framebuffer.h multiboot.h
	$(CC) $(CFLAGS) -c framebuffer.c -o framebuffer.o

graphics.o: graphics.c graphics.h font.h framebuffer.h
	$(CC) $(CFLAGS) -c graphics.c -o graphics.o

mouse.o: mouse.c mouse.h framebuffer.h
	$(CC) $(CFLAGS) -c mouse.c -o mouse.o

window.o: window.c window.h framebuffer.h graphics.h
	$(CC) $(CFLAGS) -c window.c -o window.o

pit.o: pit.c pit.h
	$(CC) $(CFLAGS) -c pit.c -o pit.o

task.o: task.c task.h
	$(CC) $(CFLAGS) -c task.c -o task.o

sched.o: sched.s
	$(AS) -f elf32 sched.s -o sched.o

kernel.bin: boot.o kernel.o gdt.o gdt_flush.o idt.o isr.o pmm.o paging.o heap.o framebuffer.o graphics.o mouse.o window.o pit.o task.o sched.o linker.ld
	ld $(LDFLAGS) -o kernel.bin boot.o kernel.o gdt.o gdt_flush.o idt.o isr.o pmm.o paging.o heap.o framebuffer.o graphics.o mouse.o window.o pit.o task.o sched.o

os.iso: kernel.bin
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	echo 'set timeout=0' > isodir/boot/grub/grub.cfg
	echo 'set default=0' >> isodir/boot/grub/grub.cfg
	echo 'insmod all_video' >> isodir/boot/grub/grub.cfg
	echo 'insmod vbe' >> isodir/boot/grub/grub.cfg
	echo 'insmod gfxterm' >> isodir/boot/grub/grub.cfg
	echo 'set gfxmode=1920x1080x32' >> isodir/boot/grub/grub.cfg
	echo 'set gfxpayload=keep' >> isodir/boot/grub/grub.cfg
	echo 'terminal_output gfxterm' >> isodir/boot/grub/grub.cfg
	echo 'menuentry "MyOS" {' >> isodir/boot/grub/grub.cfg
	echo '  multiboot /boot/kernel.bin' >> isodir/boot/grub/grub.cfg
	echo '  boot' >> isodir/boot/grub/grub.cfg
	echo '}' >> isodir/boot/grub/grub.cfg
	grub-mkrescue -o os.iso isodir

run:
	qemu-system-i386 -cdrom os.iso -serial stdio -m 128

run-headless:
	qemu-system-i386 -cdrom os.iso -serial stdio -display none -vga std -monitor none -m 128

clean:
	rm -rf *.o kernel.bin os.iso isodir
