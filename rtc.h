#ifndef RTC_H
#define RTC_H

// CMOS Real-Time Clock (MC146818-compatible). Read-only use: seed the taskbar
// clock once at boot. No timezone conversion (shows host/UTC time as-is).
// Returns seconds since midnight, or -1 if the RTC read looks insane
// (flat battery / garbage) - caller falls back to 00:00:00 in that case.
int rtc_read_seconds_of_day(void);

#endif
