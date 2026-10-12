// The smallest GameCube program for the test harness (tools/gc_run.py): logs to Dolphin's
// OSReport channel and ends.
#include <gccore.h>
#include <stdio.h>

#include "../../runtime/gc_report.h"

int main(void) {
    gc_report_raw("gc: raw write\n");
    gc_report_install();
    printf("gc: hello, arena %p-%p, ARAM %u KB\n", SYS_GetArenaLo(), SYS_GetArenaHi(), AR_GetSize() >> 10);
    printf("gc: done\n");
    for (;;) VIDEO_WaitVSync();
}
