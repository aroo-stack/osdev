#include "idt.h"
#include <stdint.h>
#include <stddef.h>

extern void idt_flush(uint32_t);

// ISR/IRQ stubs defined in isr.s
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

struct idt_entry idt[256];
struct idt_ptr idtp;

// low-level port I/O
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void io_wait(void) {
    outb(0x80, 0);
}

// serial helpers (COM1 0x3F8) - duplicated minimal to avoid coupling to kernel.c static
static int serial_transmit_empty(void) { return inb(0x3F8 + 5) & 0x20; }
static void serial_putc(char c) { while (!serial_transmit_empty()); outb(0x3F8, c); }
static void serial_puts(const char *s) { for (size_t i=0; s[i]; i++) serial_putc(s[i]); }
static void serial_put_hex(uint32_t n) {
    serial_puts("0x");
    for (int i=28; i>=0; i-=4) {
        uint8_t nib = (n>>i)&0xF;
        serial_putc(nib < 10 ? '0'+nib : 'A'+nib-10);
    }
}
static void serial_put_dec(uint32_t n) {
    char buf[11]; int i=0;
    if (n==0) { serial_putc('0'); return; }
    while (n>0) { buf[i++] = '0' + (n%10); n/=10; }
    while (i--) serial_putc(buf[i]);
}

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].offset_low = base & 0xFFFF;
    idt[num].offset_high = (base >> 16) & 0xFFFF;
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].type_attr = flags;
}

static void pic_remap(uint8_t offset1, uint8_t offset2) {
    // ICW1: init + ICW4 expected
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    // ICW2: vector offsets
    outb(0x21, offset1); io_wait();
    outb(0xA1, offset2); io_wait();
    // ICW3: wiring
    outb(0x21, 0x04); io_wait(); // master: slave on IRQ2 (00000100)
    outb(0xA1, 0x02); io_wait(); // slave: cascade identity 2 (00000010)
    // ICW4: 8086 mode
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    // OCW1: masks - enable only IRQ1 (keyboard) for this phase
    outb(0x21, 0xFD); // 11111101 - IRQ1 enabled, others masked
    outb(0xA1, 0xFF); // mask all slave
}

void idt_install(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint32_t)&idt;

    // clear
    for (int i=0;i<256;i++) idt_set_gate(i, 0, 0, 0);

    // 0-31 CPU exceptions, selector 0x08, flags 0x8E (P=1 DPL=0 0 1110 interrupt gate)
    idt_set_gate(0,  (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

    // remapped PIC IRQs 32-47
    idt_set_gate(32, (uint32_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E);

    pic_remap(0x20, 0x28);

    idt_flush((uint32_t)&idtp);
}

// human readable names
static const char *exception_names[32] = {
    "DivByZero","Debug","NMI","Breakpoint","Overflow","BoundRange","InvalidOpcode","DeviceNotAvail",
    "DoubleFault","CoprocessorSegOverrun","InvalidTSS","SegNotPresent","StackFault","GPF","PageFault","Reserved",
    "x87FPU","AlignmentCheck","MachineCheck","SIMDFPE","Virtualization","ControlProtection","Reserved","Reserved",
    "Reserved","Reserved","Reserved","Reserved","Reserved","Reserved","Security","Reserved"
};

void isr_handler(struct regs *r) {
    serial_puts("EXCEPTION: int=");
    serial_put_dec(r->int_no);
    serial_puts(" (");
    if (r->int_no < 32) serial_puts(exception_names[r->int_no]);
    else serial_puts("unknown");
    serial_puts(") err=");
    serial_put_hex(r->err_code);
    serial_puts(" eip=");
    serial_put_hex(r->eip);
    serial_puts(" cs=");
    serial_put_hex(r->cs);
    serial_puts(" eflags=");
    serial_put_hex(r->eflags);
    serial_puts("\n");
    serial_puts("System halted.\n");
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

void irq_handler(struct regs *r) {
    // send EOI
    if (r->int_no >= 40) outb(0xA0, 0x20);
    outb(0x20, 0x20);

    if (r->int_no == 33) { // IRQ1 keyboard
        uint8_t scancode = inb(0x60);
        serial_puts("KEY scancode=");
        serial_put_dec(scancode);
        serial_puts(" hex=");
        serial_put_hex(scancode);
        serial_puts("\n");
    } else if (r->int_no == 32) {
        // timer - ignore but already EOI
    } else {
        // other IRQ
        serial_puts("IRQ ");
        serial_put_dec(r->int_no - 32);
        serial_puts(" fired\n");
    }
}
