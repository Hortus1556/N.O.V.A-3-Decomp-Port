/* GameCube log output to the IPL UART (Dolphin's OSReport log), see gc_report.c. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void gc_report_install(void);
void gc_report_raw(const char *s); /* without stdio */
/* The log also to a file (the SD card's): appended by gc_report_flush (the watchdog calls it
 * every second); gc_report_flush_now also from an exception handler or with interrupts off. */
int gc_report_open(const char *path);
void gc_report_flush(void);
void gc_report_flush_now(void);
#ifdef __cplusplus
}
#endif
