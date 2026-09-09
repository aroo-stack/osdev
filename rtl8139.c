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
#define RTL_REG_CAPR    0x38 // read pointer (quirk: trails true pos by 16, mod ring)
#define RTL_REG_IMR     0x3C
#define RTL_REG_ISR     0x3E
#define RTL_REG_RCR     0x44
#define RTL_REG_CONFIG1 0x52
#define RTL_CMD_RST 0x10
#define RTL_CMD_BUFE 0x01 // buffer empty (set = nothing to read)
#define RTL_CMD_RE_TE 0x0C
#define RTL_RCR_INIT 0x8F
#define RTL_RX_RING 8192 // offset arithmetic modulo (RCR size bits = 0)
#define RTL_RX_BUF_SIZE (8192 + 16 + 2048) // ring + slack + overrun margin: worst
// case start 8188 + 4 header + 1600 (length cap) = 9792 < 10256, so a max-size
// frame at max offset can never DMA past the array (would corrupt neighbors).

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

    // 5. Masks + receive config. IMR = TOK+ROK: TOK proved Phase 3, ROK new
    // this phase (drain loop below). RCR unchanged from Phase 2 (0x8F).
    rtl_outw((uint16_t)(io_base + RTL_REG_IMR), 0x0005);
    s_puts("RTL8139: IMR TOK+ROK enabled (0x0005)\n");
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

static int rx_offset = 0; // driver read pointer, always < 8192 (mod ring)
// UDP listener state (single port, most-recent packet only). Declared here,
// before the drain hook that reads/writes them directly.
static uint16_t udp_listen_port = 0;
static uint8_t udp_rx_buf[512];
static int udp_rx_n = 0;
static uint8_t udp_rx_src[4] = {0,0,0,0};
static uint16_t udp_rx_sport = 0;
static int udp_rx_ready = 0;
// Deferred drain: the IRQ only acks + arms; the main loop parses. Keeps IRQ
// short per project discipline (serial logging ~200 bytes would otherwise
// stall other IRQs ~20ms). Single-writer flag (IRQ sets, main loop clears):
// same atomic single-word pattern as task counters.
static volatile int rx_pending = 0;

// Little-endian u16 from the ring (byte-assembled: unaligned-safe, explicit).
static uint16_t rx_u16(int off){
    return (uint16_t)rx_buffer[off] | ((uint16_t)rx_buffer[off+1] << 8);
}

static void s_put_mac(uint8_t *m){
    for(int i=0;i<6;i++){
        uint8_t b = m[i];
        s_putc(b>>4<10?'0'+(b>>4):'A'+(b>>4)-10);
        s_putc((b&0xF)<10?'0'+(b&0xF):'A'+(b&0xF)-10);
        if(i<5) s_putc(':');
    }
}

// Drain received packets. Ring entry layout (3 sources agree):
//   [+0] status u16 (bit0 ROK, bits1-4 FAE/CRC/LONG/RUNT errors)
//   [+2] length u16, INCLUDES the 4-byte CRC the card appends
//   [+4] frame bytes (length-4 of them)
// With WRAP=1 the card writes overflow linearly into slack, so reads from a
// start offset < 8192 are always linear (header never straddles: max start
// 8188 + 4 header bytes stays in-ring; data may extend into slack, which is
// mapped - no two-segment copy needed, unlike WRAP=0 drivers).
// Advance: next = (off + 4 + length, dword-aligned) mod 8192; CAPR quirk:
// program (next - 16) mod 8192 - the chip trails the true position by 16.
// Stops on empty (BUFE), invalid header (no blind consuming of garbage),
// or 16 packets (livelock guard).
// Phase 5: network configuration from the DHCP offer ("poor man's DHCP":
// parse + store the offer, no REQUEST/ACK exchange - stated simplification,
// revisit if SLIRP ever stops honoring bare discovers).
// Layout per RFC 2131 (BOOTP base) + RFC 2132 (options), verified via excerpts:
//   eth[14] | IPv4 (IHL from low nibble of byte 0 - NEVER assumed 20) |
//   UDP sport 67/dport 68 | DHCP: op(0) htype hlen hops xid(4-7) ...
//   yiaddr = DHCP+16 (our offered IP) | options from DHCP+236: magic
//   99.130.83.99 then TLV (tag, len, value; 0 = pad, 255 = end).
//   option 53 len 1: 1=DISCOVER 2=OFFER (accept 5=ACK too: SLIRP answers our
//   discover with ACK type - observed on the wire, logged verbatim).
//   option 1 = subnet mask, 3 = router list (take first), 54 = server id.
// Every read is bounds-checked against framelen (no trust in option lens).
uint8_t net_our_ip[4] = {0,0,0,0};
uint8_t net_mask[4] = {0,0,0,0};
uint8_t net_gw[4] = {0,0,0,0};
uint8_t net_server[4] = {0,0,0,0};
int net_configured = 0;
static void s_put_ip(uint8_t *a){
    s_put_dec(a[0]); s_putc('.'); s_put_dec(a[1]); s_putc('.');
    s_put_dec(a[2]); s_putc('.'); s_put_dec(a[3]);
}
static void parse_dhcp_offer(uint8_t *f, int framelen){
    int ip_off = 14;
    int ihl = f[ip_off] & 0x0F;
    if(ihl < 5) return; // nonsense header, ignore
    int ip_len = ihl * 4;
    int udp_off = ip_off + ip_len;
    if(framelen < udp_off + 8) return;
    if(f[udp_off]!=0 || f[udp_off+1]!=67 || f[udp_off+2]!=0 || f[udp_off+3]!=68) return;
    int dh = udp_off + 8;
    if(framelen < dh + 236 + 4) return; // fixed part + magic
    if(f[dh]!=2) { s_puts("RTL8139: DHCP not a reply (op!=2), ignore\n"); return; }
    if(f[dh+236]!=99 || f[dh+237]!=130 || f[dh+238]!=83 || f[dh+239]!=99){
        s_puts("RTL8139: DHCP bad magic, ignore\n"); return;
    }
    uint32_t xid = ((uint32_t)f[dh+4]<<24)|((uint32_t)f[dh+5]<<16)|((uint32_t)f[dh+6]<<8)|f[dh+7];
    if(xid != 0x12345678){ s_puts("RTL8139: DHCP XID mismatch, ignore\n"); return; }
    uint8_t yi[4]; for(int i=0;i<4;i++) yi[i] = f[dh+16+i]; // yiaddr (NOT +12: that's ciaddr, zeros here)
    uint8_t mask[4] = {0,0,0,0}, gw[4] = {0,0,0,0}, srv[4] = {0,0,0,0};
    int msgtype = -1;
    int p = dh + 240; // options start after 236 fixed + 4 magic
    int end = framelen; // frame length bounds everything (f has framelen bytes)
    while(p < end){
        uint8_t tag = f[p];
        if(tag == 255) break; // end
        if(tag == 0){ p++; continue; } // pad
        if(p + 1 >= end) break; // truncated len byte
        uint8_t len = f[p+1];
        if(p + 2 + len > end) break; // overrun: stop, keep what we have
        if(tag == 53 && len >= 1) msgtype = f[p+2];
        else if(tag == 1 && len == 4) for(int i=0;i<4;i++) mask[i] = f[p+2+i];
        else if(tag == 3 && len >= 4) for(int i=0;i<4;i++) gw[i] = f[p+2+i];
        else if(tag == 54 && len == 4) for(int i=0;i<4;i++) srv[i] = f[p+2+i];
        p += 2 + len;
    }
    if(msgtype != 2 && msgtype != 5){
        s_puts("RTL8139: DHCP msgtype "); s_put_dec((uint32_t)(msgtype<0?999:msgtype));
        s_puts(" (not OFFER/ACK), ignore\n"); return;
    }
    for(int i=0;i<4;i++){
        net_our_ip[i] = yi[i]; net_mask[i] = mask[i];
        net_gw[i] = gw[i]; net_server[i] = srv[i];
    }
    net_configured = 1;
    s_puts("RTL8139: NET configured ("); s_puts(msgtype==2?"OFFER":"ACK");
    s_puts(") ip="); s_put_ip(net_our_ip);
    s_puts(" mask="); s_put_ip(net_mask);
    s_puts(" gw="); s_put_ip(net_gw);
    s_puts(" server="); s_put_ip(net_server); s_puts("\n");
}
static void rtl8139_drain_rx(void){
    for(int n=0; n<16; n++){
        if(rtl_inb((uint16_t)(io_base + RTL_REG_CMD)) & RTL_CMD_BUFE) break; // empty
        int off = rx_offset;
        uint16_t status = rx_u16(off);
        uint16_t rawlen = rx_u16(off+2);
        if(!(status & 0x01) || (status & 0x1E) || rawlen < 60 || rawlen > 1600){
            s_puts("RTL8139: RX invalid header status=");
            s_put_hex16(status); s_puts(" len="); s_put_dec(rawlen);
            s_puts(" (stop, no advance)\n");
            break;
        }
        int framelen = rawlen - 4; // strip CRC
        uint8_t *f = &rx_buffer[off+4];
        s_puts("RTL8139: RX pkt status="); s_put_hex16(status);
        s_puts(" len="); s_put_dec((uint32_t)framelen);
        s_puts(" dst="); s_put_mac(&f[0]);
        s_puts(" src="); s_put_mac(&f[6]);
        s_puts(" type="); s_put_hex16((uint16_t)((uint16_t)f[12]<<8 | f[13]));
        // ARP frames go to the ARP handler (own length check inside).
        if(f[12]==0x08 && f[13]==0x06 && framelen >= 42)
            rtl8139_handle_arp(f, framelen);
        // DHCP sniff: IPv4/UDP server(67)->client(68) + magic cookie at DHCP+236
        // -> log XID (+ match flag). Offsets from frame start: eth 14, IP 20,
        // UDP 8, DHCP fixed 236; XID at DHCP+4, magic at DHCP+236.
        // NOTE (post-mortem): these literals MUST be hex (0x63=99 etc.) - an
        // earlier version compared decimal 63/82/53/63 and silently never
        // matched, which sent debugging on a long false trail (DMA-race
        // theories). The packet bytes were always correct.
        if(framelen >= 290 && f[12]==0x08 && f[13]==0x00 && f[23]==17 &&
           f[34]==0 && f[35]==67 && f[36]==0 && f[37]==68 &&
           f[278]==0x63 && f[279]==0x82 && f[280]==0x53 && f[281]==0x63){
            uint32_t xid = ((uint32_t)f[46]<<24)|((uint32_t)f[47]<<16)|((uint32_t)f[48]<<8)|f[49];
            s_puts(" DHCP-XID="); s_put_hex32(xid);
            s_puts(xid==0x12345678 ? " (OUR discover reply!)" : " (not ours)");
        }
        s_puts("\n");
        // Phase 5: full parse + store (self-validating: op, magic, XID, bounds).
        // Gated on IPv4/UDP-67-68 shape so ARP etc. never reach the parser.
        if(framelen >= 290 && f[12]==0x08 && f[13]==0x00 && f[23]==17 &&
           f[34]==0 && f[35]==67 && f[36]==0 && f[37]==68)
            parse_dhcp_offer(f, framelen);
        // Phase 7: generic UDP listener. IPv4 (IHL-respecting) + proto 17 +
        // sane UDP length + dport match (0 = off). Stores most recent only.
        // Independent of the DHCP path above (different ports, same frame ok).
        {
            int ihl = f[14] & 0x0F;
            if(ihl >= 5){
                int uo = 14 + ihl*4;
                if(udp_listen_port != 0 && framelen >= uo + 8 &&
                   f[14+9]==17){
                    int dport = ((int)f[uo+2] << 8) | f[uo+3];
                    int ulen = ((int)f[uo+4] << 8) | f[uo+5];
                    if(dport == udp_listen_port && ulen >= 8 &&
                       framelen >= uo + ulen){
                        int paylen = ulen - 8;
                        if(paylen > 512) paylen = 512; // truncate, don't overflow
                        for(int i=0;i<paylen;i++) udp_rx_buf[i] = f[uo+8+i];
                        udp_rx_n = paylen;
                        int ipo = 14;
                        for(int i=0;i<4;i++) udp_rx_src[i] = f[ipo+12+i];
                        udp_rx_sport = (uint16_t)(((uint16_t)f[uo] << 8) | f[uo+1]);
                        udp_rx_ready = 1;
                        s_puts("UDP: recv "); s_put_dec((uint32_t)paylen);
                        s_puts("B on port "); s_put_dec(udp_listen_port);
                        s_puts(" from "); s_put_ip(udp_rx_src);
                        s_putc(':'); s_put_dec(udp_rx_sport); s_puts("\n");
                    }
                }
            }
        }
        int next = (off + 4 + rawlen + 3) & ~3;
        next %= RTL_RX_RING;
        int capr = (next + RTL_RX_RING - 16) % RTL_RX_RING;
        rtl_outw((uint16_t)(io_base + RTL_REG_CAPR), (uint16_t)capr);
        s_puts("RTL8139: CAPR <- "); s_put_dec((uint32_t)capr);
        s_puts(" (read "); s_put_dec((uint32_t)next); s_puts(")\n");
        rx_offset = next;
    }
}

void rtl8139_irq_handler(void){
    // EOI already sent by idt.c dispatcher (established mouse pattern).
    if(!initialized || !io_base) return;
    uint16_t isr = rtl_inw((uint16_t)(io_base + RTL_REG_ISR));
    // ACK FIRST (wiki QEMU note: write before reading packets, else later
    // packets are never delivered).
    if(isr) rtl_outw((uint16_t)(io_base + RTL_REG_ISR), isr);
    s_puts("RTL8139: IRQ43 ISR="); s_put_hex16(isr);
    if(isr & 0x04) s_puts(" (TOK)");
    if(isr & 0x01) s_puts(" (ROK)");
    s_puts("\n");
    // Defer the drain: parsing runs in main-loop context (keeps IRQ short,
    // per project discipline), not here in IRQ context.
    if(isr & 0x01){ rx_pending = 1; }
}

// Called each main-loop iteration: drains queued RX packets. No-op when idle.
// (An earlier version waited >=5 ticks for "DMA settling" based on misread
// evidence; the actual bug was decimal-vs-hex literals in the magic check.
// No settle wait needed: header-valid at ROK means the frame is complete.)
void rtl8139_poll_rx(void){
    if(!rx_pending) return;
    if(!initialized || !io_base){ rx_pending = 0; return; }
    rx_pending = 0;
    rtl8139_drain_rx();
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
static uint8_t tx_buffer[1024] __attribute__((aligned(16))); // .bss: phys == virt

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
    // 3. Enable TOK interrupt (ROK already on from init; rewrite full mask so
    // this stays correct standalone). ROK drain happens in the IRQ handler.
    rtl_outw((uint16_t)(io_base + RTL_REG_IMR), 0x0005);
    s_puts("RTL8139: IMR TOK+ROK enabled\n");
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

// Phase 4 trigger: DHCP discover broadcast. QEMU user-net runs a built-in
// DHCP server (10.0.2.2) that MUST reply with a DHCPOFFER to our MAC -
// guaranteed real RX traffic to validate the receive path (background
// broadcasts alone may never come). Layout (all multi-byte big-endian):
//   eth: broadcast dst, our MAC src (re-read from IDR), type 0x0800
//   IPv4 (20B): ver/IHL 0x45, total 277, ID, flags 0, TTL 64, proto 17,
//     checksum COMPUTED (SLIRP may validate), src 0.0.0.0, dst 255.255.255.255
//   UDP (8B): sport 68, dport 67, len 257, checksum 0 (none, legal)
//   DHCP (236 fixed + 13 options = 249): op 1 (request), htype 1, hlen 6,
//     xid 0x12345678 (logged; reply must echo it), flags 0x8000 (broadcast
//     reply: safest for us to receive), chaddr = our MAC, magic cookie
//     63 82 53 63, options: msgtype=discover, param-req(1,3,6), end.
// Total frame 14+20+8+249 = 291 bytes (> 60 minimum, no pad needed).
// Descriptor pair 1 (TSAD1 0x24 / TSD1 0x14): pair 0 was consumed by the
// Phase-3 send (round-robin advances per send, per wiki).
#define RTL_REG_TSAD1 0x24
#define RTL_REG_TSD1  0x14
#define RTL_DHCP_XID 0x12345678u
static uint16_t dhcp_ip_checksum(uint8_t *h){
    uint32_t sum = 0;
    for(int i=0;i<20;i+=2) sum += ((uint16_t)h[i] << 8) | h[i+1];
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

void rtl8139_send_dhcp_discover(void){
    if(!initialized || !io_base){
        s_puts("RTL8139: dhcp skipped (not initialized)\n");
        return;
    }
    uint8_t mac[6];
    mac[0]=(uint8_t)rtl_inw((uint16_t)(io_base+0x00));
    mac[1]=(uint8_t)(rtl_inw((uint16_t)(io_base+0x00))>>8);
    mac[2]=(uint8_t)rtl_inw((uint16_t)(io_base+0x02));
    mac[3]=(uint8_t)(rtl_inw((uint16_t)(io_base+0x02))>>8);
    mac[4]=(uint8_t)rtl_inw((uint16_t)(io_base+0x04));
    mac[5]=(uint8_t)(rtl_inw((uint16_t)(io_base+0x04))>>8);
    for(int i=0;i<6;i++) tx_buffer[i] = 0xFF;
    for(int i=0;i<6;i++) tx_buffer[6+i] = mac[i];
    tx_buffer[12] = 0x08; tx_buffer[13] = 0x00;
    uint8_t *ip = &tx_buffer[14];
    for(int i=0;i<20;i++) ip[i] = 0;
    ip[0] = 0x45;
    ip[2] = 0x01; ip[3] = 0x15; // total 277 = 0x0115
    ip[4] = 0x12; ip[5] = 0x34; // ID
    ip[8] = 64; ip[9] = 17; // TTL, UDP
    ip[16]=255; ip[17]=255; ip[18]=255; ip[19]=255; // dst broadcast (src stays 0)
    {
        uint16_t c = dhcp_ip_checksum(ip);
        ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    }
    uint8_t *udp = &tx_buffer[34];
    udp[0]=0; udp[1]=68; udp[2]=0; udp[3]=67; // sport/dport
    udp[4]=0x01; udp[5]=0x01; // len 257 = 0x0101
    udp[6]=0; udp[7]=0; // checksum none
    uint8_t *dh = &tx_buffer[42];
    for(int i=0;i<236;i++) dh[i] = 0;
    dh[0]=1; dh[1]=1; dh[2]=6; // op/htype/hlen
    dh[4]=0x12; dh[5]=0x34; dh[6]=0x56; dh[7]=0x78; // XID
    dh[8]=0x80; dh[9]=0x00; // flags: broadcast reply
    for(int i=0;i<6;i++) dh[28+i] = mac[i]; // chaddr
    dh[236]=63; dh[237]=82; dh[238]=53; dh[239]=63; // magic
    dh[240]=53; dh[241]=1; dh[242]=1; // msgtype = discover
    dh[243]=55; dh[244]=3; dh[245]=1; dh[246]=3; dh[247]=6; // param req: subnet,router,dns
    dh[248]=255; // end
    s_puts("RTL8139: DHCP discover built (291B, XID 0x12345678), sending via pair 1\n");
    rtl_outl(io_base + RTL_REG_TSAD1, (uint32_t)tx_buffer);
    rtl_outl(io_base + RTL_REG_TSD1, 291);
    {
        uint32_t tsd = 0;
        int polls = 0;
        while(polls < 1000000){
            tsd = rtl_inl(io_base + RTL_REG_TSD1);
            if(tsd & RTL_TSD_TOK) break;
            polls++;
        }
        s_puts("RTL8139: DHCP-TX TOK polls "); s_put_dec((uint32_t)polls);
        s_puts(" TSD1="); s_put_hex32(tsd);
        s_puts((tsd & RTL_TSD_TOK) ? " OK (discover on wire, offer should follow)\n" : " TIMEOUT!\n");
    }
}

// Phase 6: ARP (RFC 826, verified via excerpts). Frame: eth dst(6)/src(6)/
// type 0x0806, then htype(2)=1, ptype(2)=0x0800, hlen(1)=6, plen(1)=4,
// op(2)=1 request / 2 reply, sha(6), spa(4), tha(6), tpa(4) = 28B payload,
// 42B frame, zero-padded to the 60B minimum (card CRCs itself).
// TX descriptors: pairs 0,1 consumed by verified Phase-3/4 sends; new sends
// round-robin from pair 2 (static counter, %4 - reuse after idle is legal).
static int tx_next = 2;
static struct { uint8_t ip[4]; uint8_t mac[6]; int valid; } arp_table[ARP_TABLE_SIZE];
static uint8_t arp_frame[64]; // build buffer (.bss, phys==virt)

int arp_lookup(uint8_t *ip, uint8_t *mac_out){
    for(int i=0;i<ARP_TABLE_SIZE;i++){
        if(!arp_table[i].valid) continue;
        if(arp_table[i].ip[0]==ip[0] && arp_table[i].ip[1]==ip[1] &&
           arp_table[i].ip[2]==ip[2] && arp_table[i].ip[3]==ip[3]){
            for(int k=0;k<6;k++) mac_out[k] = arp_table[i].mac[k];
            return 1;
        }
    }
    return 0;
}

static void arp_store(uint8_t *ip, uint8_t *mac){
    int slot = -1;
    for(int i=0;i<ARP_TABLE_SIZE;i++){
        if(arp_table[i].valid &&
           arp_table[i].ip[0]==ip[0] && arp_table[i].ip[1]==ip[1] &&
           arp_table[i].ip[2]==ip[2] && arp_table[i].ip[3]==ip[3]){ slot = i; break; }
        if(slot==-1 && !arp_table[i].valid) slot = i;
    }
    if(slot==-1) slot = 0; // full: evict slot 0 (documented simple policy)
    for(int i=0;i<4;i++) arp_table[slot].ip[i] = ip[i];
    for(int i=0;i<6;i++) arp_table[slot].mac[i] = mac[i];
    arp_table[slot].valid = 1;
    s_puts("RTL8139: ARP cache store "); s_put_ip(ip);
    s_puts(" -> "); s_put_mac(mac); s_puts(" (slot "); s_put_dec((uint32_t)slot); s_puts(")\n");
}

// Raw transmit on the next round-robin pair. Returns 1 if TOK set (else 0).
static int rtl8139_tx_raw(uint8_t *frame, int len){
    int d = tx_next % 4; tx_next++;
    uint32_t tsad = (uint32_t)(io_base + 0x20 + d*4);
    uint32_t tsd = (uint32_t)(io_base + 0x10 + d*4);
    for(int i=0;i<len;i++) tx_buffer[i] = frame[i]; // stage via DMA buffer
    rtl_outl((uint16_t)tsad, (uint32_t)tx_buffer);
    rtl_outl((uint16_t)tsd, (uint32_t)len); // OWN=0 -> GO
    uint32_t v = 0;
    int polls = 0;
    while(polls < 1000000){
        v = rtl_inl((uint16_t)tsd);
        if(v & RTL_TSD_TOK) break;
        polls++;
    }
    s_puts("RTL8139: TX pair "); s_put_dec((uint32_t)d);
    s_puts(" len "); s_put_dec((uint32_t)len);
    s_puts((v & RTL_TSD_TOK) ? " TOK OK\n" : " TOK TIMEOUT!\n");
    return (v & RTL_TSD_TOK) != 0;
}

static void read_our_mac(uint8_t *mac){
    uint16_t w0 = rtl_inw((uint16_t)(io_base+0x00));
    uint16_t w1 = rtl_inw((uint16_t)(io_base+0x02));
    uint16_t w2 = rtl_inw((uint16_t)(io_base+0x04));
    mac[0]=(uint8_t)w0; mac[1]=(uint8_t)(w0>>8);
    mac[2]=(uint8_t)w1; mac[3]=(uint8_t)(w1>>8);
    mac[4]=(uint8_t)w2; mac[5]=(uint8_t)(w2>>8);
}

void rtl8139_send_arp_request(uint8_t *tip){
    if(!initialized || !io_base || !net_configured){
        s_puts("RTL8139: arp request skipped (not ready: init/ip?)\n");
        return;
    }
    uint8_t mac[6]; read_our_mac(mac);
    uint8_t *a = arp_frame;
    for(int i=0;i<6;i++) a[i] = 0xFF; // broadcast (don't know MAC yet)
    for(int i=0;i<6;i++) a[6+i] = mac[i];
    a[12] = 0x08; a[13] = 0x06; // EtherType ARP
    a[14]=0; a[15]=1; // htype Ethernet
    a[16]=0x08; a[17]=0x00; // ptype IPv4
    a[18]=6; a[19]=4; // hlen/plen
    a[20]=0; a[21]=1; // op = request
    for(int i=0;i<6;i++) a[22+i] = mac[i]; // sha
    for(int i=0;i<4;i++) a[28+i] = net_our_ip[i]; // spa
    for(int i=0;i<6;i++) a[32+i] = 0; // tha unknown
    for(int i=0;i<4;i++) a[38+i] = tip[i]; // tpa
    for(int i=42;i<60;i++) a[i] = 0; // pad to 60B minimum
    s_puts("RTL8139: ARP request who-has "); s_put_ip(tip); s_puts(" tell "); s_put_ip(net_our_ip); s_puts("\n");
    rtl8139_tx_raw(a, 60);
}

// Parse + handle one ARP frame (already bounds-checked to >=42B by caller).
// Replies: merge sender mapping (RFC 826 merge rule), log gateway match.
// Requests for OUR ip: build + send reply (sha/spa = us, tha/tpa = asker).
void rtl8139_handle_arp(uint8_t *f, int framelen){
    (void)framelen;
    if(f[14]!=0 || f[15]!=1 || f[16]!=0x08 || f[17]!=0x00 || f[18]!=6 || f[19]!=4){
        s_puts("RTL8139: ARP non-Ethernet/IPv4 params, ignore\n"); return;
    }
    int op = ((int)f[20] << 8) | f[21];
    uint8_t *sha = &f[22], *spa = &f[28], *tha = &f[32], *tpa = &f[38];
    if(op == 2){ // reply
        s_puts("RTL8139: ARP reply ");
        s_put_ip(spa); s_puts(" is-at "); s_put_mac(sha);
        int for_us = net_configured &&
            tpa[0]==net_our_ip[0] && tpa[1]==net_our_ip[1] &&
            tpa[2]==net_our_ip[2] && tpa[3]==net_our_ip[3];
        s_puts(for_us ? " (for us)" : " (not for us, cached anyway)");
        s_puts("\n");
        arp_store(spa, sha);
    } else if(op == 1){ // request
        s_puts("RTL8139: ARP request who-has "); s_put_ip(tpa);
        s_puts(" from "); s_put_mac(sha); s_puts("\n");
        arp_store(spa, sha); // merge rule: learn sender even from requests
        if(!net_configured){ s_puts("RTL8139: no IP yet, not answering\n"); return; }
        if(tpa[0]!=net_our_ip[0] || tpa[1]!=net_our_ip[1] ||
           tpa[2]!=net_our_ip[2] || tpa[3]!=net_our_ip[3]){
            s_puts("RTL8139: not asking for us, no reply\n"); return;
        }
        uint8_t mac[6]; read_our_mac(mac);
        uint8_t *a = arp_frame;
        for(int i=0;i<6;i++) a[i] = sha[i]; // eth dst = asker
        for(int i=0;i<6;i++) a[6+i] = mac[i];
        a[12] = 0x08; a[13] = 0x06;
        a[14]=0; a[15]=1; a[16]=0x08; a[17]=0x00; a[18]=6; a[19]=4;
        a[20]=0; a[21]=2; // op = reply
        for(int i=0;i<6;i++) a[22+i] = mac[i]; // sha = us
        for(int i=0;i<4;i++) a[28+i] = net_our_ip[i]; // spa = us
        for(int i=0;i<6;i++) a[32+i] = sha[i]; // tha = asker
        for(int i=0;i<4;i++) a[38+i] = spa[i]; // tpa = asker
        for(int i=42;i<60;i++) a[i] = 0;
        s_puts("RTL8139: ARP answering with our MAC\n");
        rtl8139_tx_raw(a, 60);
    } else {
        s_puts("RTL8139: ARP unknown op, ignore\n");
    }
}

// Manual RX pump for pre-sti boot waits: check ISR directly (no interrupts
// needed - pure PIO), ack + drain. No EOI here (no IRQ context to end;
// a pending PIC IRQ will fire normally on sti and just find nothing new).
void rtl8139_pump_rx(void){
    if(!initialized || !io_base) return;
    uint16_t isr = rtl_inw((uint16_t)(io_base + RTL_REG_ISR));
    if(!(isr & 0x01)) return; // no receive pending
    rtl_outw((uint16_t)(io_base + RTL_REG_ISR), isr); // ack first (QEMU rule)
    rtl8139_drain_rx();
}

// Phase 7: generic UDP send/receive (not DHCP/ARP-specific).
// Checksums per RFC 768/791: 16-bit one's-complement sum (big-endian words),
// fold carries, complement. UDP covers pseudo-header (src+dst IP, zero,
// protocol 17, UDP length) + header (checksum field zeroed) + payload, with a
// virtual zero pad byte if the payload length is odd (pad NOT transmitted).
static uint16_t ones_sum_finish(uint32_t sum){
    while(sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}
static uint16_t ip_checksum(uint8_t *h, int hlen){
    uint32_t sum = 0;
    for(int i=0;i+1<hlen;i+=2) sum += ((uint16_t)h[i] << 8) | h[i+1];
    if(hlen & 1) sum += (uint16_t)h[hlen-1] << 8; // odd: pad low byte (never for 20B IP, kept general)
    return ones_sum_finish(sum);
}
static uint16_t udp_checksum(uint8_t *sip, uint8_t *dip, uint8_t *u, int ulen){
    // ulen = UDP header (8) + payload; checksum field u[6..7] must be zero in.
    uint32_t sum = 0;
    for(int i=0;i<4;i+=2){ sum += ((uint16_t)sip[i] << 8) | sip[i+1]; sum += ((uint16_t)dip[i] << 8) | dip[i+1]; }
    sum += 17; // zero byte + protocol (0x00, 0x11)
    sum += (uint16_t)ulen;
    for(int i=0;i+1<ulen;i+=2){
        if(i==6) continue; // skip checksum field itself
        sum += ((uint16_t)u[i] << 8) | u[i+1];
    }
    if(ulen & 1) sum += (uint16_t)u[ulen-1] << 8; // odd payload: virtual pad
    // NOTE: if ulen is odd, byte ulen-1 is payload; the (i==6) skip only
    // matters for even ulen>=8 (always true here: header is 8 bytes).
    return ones_sum_finish(sum);
}
void udp_rx_consume(void){ udp_rx_ready = 0; }
void udp_listen(uint16_t port){ udp_listen_port = port; udp_rx_ready = 0; }
int udp_received(void){ return udp_rx_ready; }
uint8_t *udp_rx_data(void){ return udp_rx_buf; }
int udp_rx_len(void){ return udp_rx_n; }
uint8_t *udp_rx_src_ip(void){ return udp_rx_src; }
uint16_t udp_rx_src_port(void){ return udp_rx_sport; }

// Resolve L2 destination: our own IP -> our MAC (loopback-by-construction);
// ARP cache hit -> cached MAC; else ARP-request + bounded pump-wait; final
// fallback: gateway MAC (covers SLIRP addresses that won't answer ARP).
// Returns 1 with dmac filled, 0 on total failure.
static int udp_resolve_mac(uint8_t *dip, uint8_t *dmac){
    uint8_t mymac[6]; read_our_mac(mymac);
    if(dip[0]==net_our_ip[0] && dip[1]==net_our_ip[1] &&
       dip[2]==net_our_ip[2] && dip[3]==net_our_ip[3]){
        for(int i=0;i<6;i++) dmac[i] = mymac[i];
        s_puts("UDP: dest is self, using our MAC\n");
        return 1;
    }
    if(arp_lookup(dip, dmac)){ s_puts("UDP: ARP cache hit\n"); return 1; }
    s_puts("UDP: ARP cache miss, requesting...\n");
    rtl8139_send_arp_request(dip);
    for(int i=0;i<2000000 && !arp_lookup(dip, dmac);i++) rtl8139_pump_rx();
    if(arp_lookup(dip, dmac)){ s_puts("UDP: ARP resolved via request\n"); return 1; }
    s_puts("UDP: ARP unanswered, falling back to gateway MAC\n");
    if(arp_lookup(net_gw, dmac)) return 1;
    s_puts("UDP: no gateway MAC either, FAIL\n");
    return 0;
}
static uint16_t udp_ip_id = 0x2000;

int udp_send(uint8_t *dip, uint16_t dport, uint16_t sport, uint8_t *data, int len){
    if(!initialized || !io_base || !net_configured){
        s_puts("UDP: send skipped (not ready)\n"); return 0;
    }
    if(len < 0 || len > 400){ s_puts("UDP: bad length\n"); return 0; }
    uint8_t dmac[6];
    if(!udp_resolve_mac(dip, dmac)) return 0;
    uint8_t mymac[6]; read_our_mac(mymac);
    static uint8_t frame[1024];
    for(int i=0;i<6;i++){ frame[i] = dmac[i]; frame[6+i] = mymac[i]; }
    frame[12] = 0x08; frame[13] = 0x00;
    uint8_t *ip = &frame[14];
    int ip_total = 20 + 8 + len;
    for(int i=0;i<20;i++) ip[i] = 0;
    ip[0] = 0x45; ip[2] = (uint8_t)(ip_total >> 8); ip[3] = (uint8_t)ip_total;
    udp_ip_id++; ip[4] = (uint8_t)(udp_ip_id >> 8); ip[5] = (uint8_t)udp_ip_id;
    ip[8] = 64; ip[9] = 17;
    for(int i=0;i<4;i++){ ip[12+i] = net_our_ip[i]; ip[16+i] = dip[i]; }
    { uint16_t c = ip_checksum(ip, 20); ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c; }
    uint8_t *u = &frame[34];
    int ulen = 8 + len;
    u[0]=(uint8_t)(sport>>8); u[1]=(uint8_t)sport;
    u[2]=(uint8_t)(dport>>8); u[3]=(uint8_t)dport;
    u[4]=(uint8_t)(ulen>>8); u[5]=(uint8_t)ulen;
    u[6]=0; u[7]=0;
    for(int i=0;i<len;i++) u[8+i] = data[i];
    { uint16_t c = udp_checksum(net_our_ip, dip, u, ulen);
      u[6]=(uint8_t)(c>>8); u[7]=(uint8_t)c; }
    int framelen = 14 + ip_total;
    while(framelen < 60){ frame[framelen] = 0; framelen++; } // 60B minimum
    s_puts("UDP: TX "); s_put_ip(net_our_ip);
    s_puts(" -> "); s_put_ip(dip);
    s_puts(" ports "); s_put_dec(sport); s_putc(':'); s_put_dec(dport);
    s_puts(" payload "); s_put_dec((uint32_t)len); s_puts("B frame "); s_put_dec((uint32_t)framelen); s_puts("B\n");
    return rtl8139_tx_raw(frame, framelen);
}
