// RTL8139 Phase 2: bring-up only (reset, buffers, IRQ plumbing). NO packets.
// Register map + init sequence per OSDev wiki "RTL8139" (verified via archive):
//   CONFIG1 (0x52) <- 0x00 ............ power on (LWAKE+LWPTN active high)
//   CMD (0x37) <- 0x10 ................ soft reset; poll until RST bit clears
//     (QEMU note on wiki: RST may read high BEFORE reset - ignored, we only
//     check after writing 0x10, with a timeout so a dead card can't hang us)
//   RBSTART (0x30) <- phys addr ...... 8192+16(+1500 for WRAP) RX buffer
//   IMR (0x3C) <- 0x0000 .............. Phase 2: mask everything (no traffic yet;
//     Phase 3 sets 0x0005 TOK+ROK). Handler still installed + PIC unmasked.
//   RCR (0x44) <- 0x8F ................ 0xF (AB+AM+APM+AAP) + WRAP bit (1<<7)
//   CMD (0x37) <- 0x0C ................ RE|TE (receiver + transmitter enable)
//   ISR (0x3E) read ................... expect 0x0000 (nothing happened yet)
// PCI command (config 0x04) bit 2 = bus master (lets the card DMA to RAM
// without CPU copying), bit 0 = I/O space (BAR0 is I/O type). SeaBIOS/QEMU
// may not set master, so we set bits 0|2 explicitly and verify.
// RX buffer: static .bss array (identity-mapped 0..32MB, so physical address
// == virtual address for card DMA). 8192+16+1500 per wiki WRAP note.
#include <stdint.h>
#include "pci.h"
#include "rtl8139.h"

#define RTL_REG_RBSTART 0x30
#define RTL_REG_CMD     0x37
#define RTL_REG_IMR     0x3C
#define RTL_REG_ISR     0x3E
#define RTL_REG_RCR     0x44
#define RTL_REG_CONFIG1 0x52
#define RTL_CMD_RST 0x10
#define RTL_CMD_RE_TE 0x0C
#define RTL_RCR_INIT 0x8F
#define RTL_RX_BUF_SIZE (8192 + 16 + 1500)

static uint8_t rx_buffer[RTL_RX_BUF_SIZE] __attribute__((aligned(16)));
static uint32_t io_base = 0;
static int initialized = 0;

static inline void rtl_outb(uint16_t port, uint8_t v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline uint8_t rtl_inb(uint16_t port){ uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }
static inline void rtl_outw(uint16_t port, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(port)); }
static inline uint16_t rtl_inw(uint16_t port){ uint16_t r; __asm__ volatile("inw %1,%0":"=a"(r):"Nd"(port)); return r; }
static inline void rtl_outl(uint16_t port, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(port)); }
static inline uint32_t rtl_inl(uint16_t port){ uint32_t r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(port)); return r; }
static void s_putc(char c){ while(!(rtl_inb(0x3F8+5)&0x20)); rtl_outb(0x3F8,(uint8_t)c); }
static void s_puts(const char*s){ for(int i=0;s[i];i++) s_putc(s[i]); }
static void s_put_hex16(uint16_t n){ s_puts("0x"); for(int i=12;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
static void s_put_hex32(uint32_t n){ s_puts("0x"); for(int i=28;i>=0;i-=4){ uint8_t v=(n>>i)&0xF; s_putc(v<10?'0'+v:'A'+v-10); } }
static void s_put_dec(uint32_t n){ char b[11]; int i=0; if(n==0){s_putc('0');return;} while(n){b[i++]='0'+n%10; n/=10;} while(i--) s_putc(b[i]); }

uint32_t rtl8139_iobase(void){ return io_base; }

void rtl8139_init(void){
    if(!pci_rtl8139_present()){
        s_puts("RTL8139: absent, skipping init\n");
        return;
    }
    if(!pci_rtl8139_bar0_is_io()){
        s_puts("RTL8139: BAR0 is not I/O type, PIO driver aborts\n");
        return;
    }
    io_base = pci_rtl8139_iobase();
    int bus = pci_rtl8139_bus(), dev = pci_rtl8139_dev(), fn = pci_rtl8139_fn();
    s_puts("RTL8139: init, iobase "); s_put_hex32(io_base); s_puts("\n");

    // 1. PCI bus mastering (+ I/O space): read command, set bits 0|2, verify.
    {
        uint16_t before = pci_config_read_word((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x04);
        s_puts("RTL8139: PCI command before "); s_put_hex16(before); s_puts("\n");
        pci_config_write_word((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x04, before | 0x05);
        uint16_t after = pci_config_read_word((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x04);
        s_puts("RTL8139: PCI command after "); s_put_hex16(after);
        s_puts(((after & 0x05) == 0x05) ? " (IO+MASTER OK)\n" : " (FAILED to set bits!)\n");
    }

    // 2. Power on via CONFIG1.
    rtl_outb((uint16_t)(io_base + RTL_REG_CONFIG1), 0x00);
    s_puts("RTL8139: CONFIG1 power-on write done\n");

    // 3. Soft reset, poll RST-clear with timeout (never hang on dead hardware).
    {
        rtl_outb((uint16_t)(io_base + RTL_REG_CMD), RTL_CMD_RST);
        int polls = 0;
        while((rtl_inb((uint16_t)(io_base + RTL_REG_CMD)) & RTL_CMD_RST) && polls < 1000000) polls++;
        uint8_t cmd = rtl_inb((uint16_t)(io_base + RTL_REG_CMD));
        s_puts("RTL8139: reset polls "); s_put_dec((uint32_t)polls);
        if(cmd & RTL_CMD_RST){
            s_puts(" RST STILL SET (timeout!)\n");
            return; // do not continue with a non-responsive card
        }
        s_puts(" RST cleared OK, CMD="); s_put_hex16(cmd); s_puts("\n");
    }

    // 4. RX buffer address (.bss identity-mapped => physical == virtual).
    {
        uint32_t phys = (uint32_t)rx_buffer;
        rtl_outl(io_base + RTL_REG_RBSTART, phys);
        uint32_t back = rtl_inl(io_base + RTL_REG_RBSTART);
        s_puts("RTL8139: RBSTART set "); s_put_hex32(phys);
        s_puts(back == phys ? " readback MATCH\n" : " readback MISMATCH!\n");
    }

    // 5. Masks + receive config (IMR=0 this phase: silent until Phase 3).
    rtl_outw((uint16_t)(io_base + RTL_REG_IMR), 0x0000);
    s_puts("RTL8139: IMR masked (0x0000, Phase 3 enables TOK+ROK)\n");
    rtl_outl(io_base + RTL_REG_RCR, RTL_RCR_INIT);
    {
        uint32_t rcr = rtl_inl(io_base + RTL_REG_RCR);
        s_puts("RTL8139: RCR readback "); s_put_hex32(rcr);
        s_puts(rcr == RTL_RCR_INIT ? " OK\n" : " (differs!)\n");
    }

    // 6. Enable receiver + transmitter, verify.
    rtl_outb((uint16_t)(io_base + RTL_REG_CMD), RTL_CMD_RE_TE);
    {
        uint8_t cmd = rtl_inb((uint16_t)(io_base + RTL_REG_CMD));
        s_puts("RTL8139: CMD readback "); s_put_hex16(cmd);
        s_puts((cmd & RTL_CMD_RE_TE) == RTL_CMD_RE_TE ? " (RE|TE OK)\n" : " (RE|TE MISSING!)\n");
    }

    // 7. Status should be quiet (no traffic yet).
    {
        uint16_t isr = rtl_inw((uint16_t)(io_base + RTL_REG_ISR));
        s_puts("RTL8139: ISR "); s_put_hex16(isr);
        s_puts(isr == 0 ? " (quiet, as expected)\n" : " (unexpected bits!)\n");
    }

    // 8. Unmask IRQ line on slave PIC (IRQ11 = slave bit 3). IDT gate 43
    // (0x2B) is pre-installed by idt_install (all IRQs 32-47 wired there).
    {
        uint8_t before = rtl_inb(0xA1);
        rtl_outb(0xA1, before & ~0x08u);
        uint8_t after = rtl_inb(0xA1);
        s_puts("RTL8139: PIC slave mask "); s_put_hex16(before);
        s_puts(" -> "); s_put_hex16(after);
        s_puts(" (IRQ11 unmasked, vector 43 = 0x28+3)\n");
    }

    initialized = 1;
    s_puts("RTL8139: init complete, awaiting Phase 3 (packets)\n");
}

void rtl8139_irq_handler(void){
    // EOI already sent by idt.c dispatcher (established mouse pattern).
    if(!initialized || !io_base) return;
    uint16_t isr = rtl_inw((uint16_t)(io_base + RTL_REG_ISR));
    s_puts("RTL8139: IRQ43 ISR="); s_put_hex16(isr);
    if(isr & 0x04) s_puts(" (TOK)");
    if(isr & 0x01) s_puts(" (ROK)");
    s_puts("\n");
    if(isr) rtl_outw((uint16_t)(io_base + RTL_REG_ISR), isr); // ack (QEMU needs the write)
}

// Phase 3: transmit one real broadcast frame, poll TOK for completion.
// Ethernet II layout (destination 6, source 6, ethertype 2, then payload):
//   dst[6] = FF:FF:FF:FF:FF:FF (broadcast: no real destination needed)
//   src[6] = burned-in MAC from IDR0-5 (read, never made up)
//   type[2] = 0x0800 (IPv4 EtherType, big-endian on the wire)
//   payload[46] = ASCII tag, zero-padded to the 60-byte minimum
//     (60 = 14 header + 46 payload; card appends the 4-byte CRC itself).
// Descriptor pair 0 (only usable pair after reset, per wiki): TSAD0 (0x20) =
// buffer physical address, then TSD0 (0x10) = length with OWN=0 to trigger.
// TOK = TSD bit 15, set by hardware when the frame is on the wire.
#define RTL_REG_TSAD0 0x20
#define RTL_REG_TSD0  0x10
#define RTL_TSD_TOK 0x8000u
#define RTL_TX_FRAME_LEN 60
static uint8_t tx_buffer[128] __attribute__((aligned(16))); // .bss: phys == virt

void rtl8139_send_test(void){
    if(!initialized || !io_base){
        s_puts("RTL8139: send_test skipped (not initialized)\n");
        return;
    }
    // 1. Burned-in MAC from IDR0-5 (3x16-bit reads, little-endian bytes in order).
    uint8_t mac[6];
    {
        uint16_t w0 = rtl_inw((uint16_t)(io_base + 0x00));
        uint16_t w1 = rtl_inw((uint16_t)(io_base + 0x02));
        uint16_t w2 = rtl_inw((uint16_t)(io_base + 0x04));
        mac[0]=(uint8_t)w0; mac[1]=(uint8_t)(w0>>8);
        mac[2]=(uint8_t)w1; mac[3]=(uint8_t)(w1>>8);
        mac[4]=(uint8_t)w2; mac[5]=(uint8_t)(w2>>8);
        s_puts("RTL8139: MAC ");
        for(int i=0;i<6;i++){
            uint8_t b = mac[i];
            s_putc(b>>4<10?'0'+(b>>4):'A'+(b>>4)-10);
            s_putc((b&0xF)<10?'0'+(b&0xF):'A'+(b&0xF)-10);
            if(i<5) s_putc(':');
        }
        s_puts("\n");
    }
    // 2. Build the frame in the DMA buffer.
    for(int i=0;i<6;i++) tx_buffer[i] = 0xFF; // broadcast dst
    for(int i=0;i<6;i++) tx_buffer[6+i] = mac[i]; // our MAC as src
    tx_buffer[12] = 0x08; tx_buffer[13] = 0x00; // EtherType IPv4
    {
        const char *tag = "RTL8139-TX-TEST ";
        int p = 0;
        while(tag[p] && p < 46){ tx_buffer[14+p] = (uint8_t)tag[p]; p++; }
        while(p < 46){ tx_buffer[14+p] = 0; p++; } // pad to 60-byte minimum
    }
    s_puts("RTL8139: TX frame hex:\n");
    for(int r=0;r<RTL_TX_FRAME_LEN;r+=16){
        s_puts("  ");
        for(int c=0;c<16 && r+c<RTL_TX_FRAME_LEN;c++){
            uint8_t b = tx_buffer[r+c];
            s_putc(b>>4<10?'0'+(b>>4):'A'+(b>>4)-10);
            s_putc((b&0xF)<10?'0'+(b&0xF):'A'+(b&0xF)-10);
            s_putc(' ');
        }
        s_puts("\n");
    }
    // 3. Enable TOK interrupt (ROK stays for Phase 4), then trigger via pair 0.
    rtl_outw((uint16_t)(io_base + RTL_REG_IMR), 0x0004);
    s_puts("RTL8139: IMR TOK enabled\n");
    rtl_outl(io_base + RTL_REG_TSAD0, (uint32_t)tx_buffer); // phys == virt (identity)
    rtl_outl(io_base + RTL_REG_TSD0, RTL_TX_FRAME_LEN); // length, OWN=0 -> GO
    s_puts("RTL8139: TX triggered (TSAD0 set, TSD0=60)\n");
    // 4. Poll TOK with timeout (primary proof; IRQ43 is the bonus proof).
    {
        uint32_t tsd = 0;
        int polls = 0;
        while(polls < 1000000){
            tsd = rtl_inl(io_base + RTL_REG_TSD0);
            if(tsd & RTL_TSD_TOK) break;
            polls++;
        }
        s_puts("RTL8139: TOK polls "); s_put_dec((uint32_t)polls);
        s_puts(" TSD0="); s_put_hex32(tsd);
        s_puts((tsd & RTL_TSD_TOK) ? " TRANSMIT OK (frame left the card)\n" : " TOK NEVER SET (timeout!)\n");
    }
}
