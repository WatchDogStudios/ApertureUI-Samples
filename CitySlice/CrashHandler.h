/*
 * CrashHandler.h - CitySlice crash reporting: on an unhandled exception write
 * a minidump next to the executable (CrashDump/CitySlice_<date>_<pid>.dmp),
 * a plain-text report to %TEMP%/CitySlice-crash.txt with the exception code,
 * the symbolised call stack of the crashing thread and whatever the host
 * chose to add (game state, the last console lines), and flush stdout so the
 * buffered frame log is not lost.
 *
 * Built on Foundation's nsCrashHandler (process-wide unhandled-exception
 * filter). The Aperture SDK's own Sentry integration, when the engine is built
 * with it, stays in place: it reports to the SDK's project, this reports to
 * the developer's disk.
 */
#pragma once

#include <cstdio>
#include <functional>

/// Extra lines the host wants in the crash report (called with the open report
/// file, from the crashing thread: keep it simple and allocation-free where possible).
using CrashContextWriter = std::function<void(FILE* report)>;

/// Installs the process-wide handler. Safe to call before the SDK initialises.
void InstallCitySliceCrashHandler(CrashContextWriter contextWriter);

/// Path of the text report the handler writes.
const char* CitySliceCrashReportPath();
