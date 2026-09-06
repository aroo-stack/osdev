// CMOS RTC reader (MC146818-compatible). Register map, UIP handling, BCD and
// 12-hour conversion all per OSDev wiki "CMOS" (verified via archive copy):
// - Ports 0x70 (select, bit 7 = NMI disable) / 0x71 (data). Reselect before
//   EVERY access (reads reset selection to 0xD) + short io_wait after select.
// - Registers: 0x00 seconds 0-59, 0x02 minutes 0-59, 0x04 hours
//   (0-23 in 24h mode; 1-12 + 0x80 PM bit in 12h mode).
// - 0x0A bit 7 = Update-In-Progress (RTC mid-update, values may tear).
// - 0x0B bit 1 (val 2) = 24h mode, bit 2 (val 4) = binary (else BCD).
// - BCD->binary: (b & 0x0F) + ((b >> 4) * 10). 0x42 means 42, not 66.
// - 12h->24h done as h=(h%12)+am/pm adjust (12am->0, 12pm->12); the wiki's
//   example ((h&0x7F)+12)%24 mishandles noon (12pm->0), so not used verbatim.
// - Torn-read guard: wait UIP-clear, read all, re-wait, re-read until two
//   consecutive full reads match (wiki's second alternative).
// NMI: accessed with 0x80 bit set (NMI masked during CMOS access). We have no
// NMI handler/use, so leaving it masked after is harmless and documented.
#include <stdint.h>
#include "rtc.h"

static inline void rtc_outb(uint16_t port, uint8_t val){
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t rtc_inb(uint16_t port){
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
// Select register (reselect every time) + io_wait via POST port 0x80.
static uint8_t rtc_reg(int reg){
    rtc_outb(0x70, (uint8_t)(0x80 | (reg & 0x7F)));
    rtc_outb(0x80, 0); // standard io_wait: let CMOS latch the selection
    return rtc_inb(0x71);
}
static int rtc_updating(void){
    return rtc_reg(0x0A) & 0x80;
}
static int bcd_to_bin(uint8_t b){
    return (b & 0x0F) + ((b >> 4) * 10); // >>4, never /16 (same value, trivially safe)
}

int rtc_read_seconds_of_day(void){
    int sec, min, hr, lsec, lmin, lhr;
    uint8_t regb;
    while(rtc_updating()) {} // wait current update out
    sec = rtc_reg(0x00); min = rtc_reg(0x02); hr = rtc_reg(0x04);
    do {
        lsec = sec; lmin = min; lhr = hr;
        while(rtc_updating()) {}
        sec = rtc_reg(0x00); min = rtc_reg(0x02); hr = rtc_reg(0x04);
        // Any mismatch = torn read across a 1-second rollover: retry.
    } while(sec != lsec || min != lmin || hr != lhr);
    regb = rtc_reg(0x0B);
    if(!(regb & 0x04)){ // BCD mode: convert (QEMU default is BCD)
        sec = bcd_to_bin((uint8_t)sec);
        min = bcd_to_bin((uint8_t)min);
        // Hour keeps PM bit through conversion (wiki form with >>4).
        hr = ((hr & 0x0F) + (((hr & 0x70) >> 4) * 10)) | (hr & 0x80);
    }
    if(!(regb & 0x02)){ // 12-hour mode: 12am->0, 1-11am unchanged, 12pm->12, 1-11pm->+12
        int pm = hr & 0x80;
        hr &= 0x7F;
        if(pm) hr = (hr % 12) + 12;
        else hr = hr % 12;
    }
    if(sec < 0 || sec > 59 || min < 0 || min > 59 || hr < 0 || hr > 23) return -1;
    return hr * 3600 + min * 60 + sec;
}
