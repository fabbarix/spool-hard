#pragma once
// Console "housekeeping" task: runs the slow SD/crash-log duties that used to
// live in loop(): SdCard::update() (2 s liveness/remount gate) and
// CrashLogger::update() (3 s log spill gate). 5 KB internal stack, core 0,
// prio 1 (below AsyncTCP), 100 ms tick. Call once after CrashLogger::begin().
// Once running, loopTask must NOT call those update()s (it checks
// housekeepingActive()); the entry points stay callable as the fallback if
// task creation fails.
void housekeepingBegin();
bool housekeepingActive();
