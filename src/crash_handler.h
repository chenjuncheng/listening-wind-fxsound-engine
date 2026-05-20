/*
 * crash_handler.h - Crash capture for fxsound_engine.exe
 * Output: crash_dumps/crash_YYYYMMDD_HHMMSS_<pid>.dmp + .txt
 */
#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#include <string>
#include <vector>

namespace CrashHandler {
bool install(const std::wstring& exeDir);
void uninstall();
bool isInstalled();
std::wstring getCrashDumpsDir();
std::vector<std::wstring> findCrashDumps();
int cleanupOldDumps(int maxAgeDays = 7);
}

#endif
