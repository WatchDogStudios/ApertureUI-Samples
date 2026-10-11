/*
 * CrashHandler.cpp - CitySlice crash reporting (see CrashHandler.h)
 */
#include "CrashHandler.h"

#include <Foundation/Platform/Win/Utils/IncludeWindows.h>
#include <Foundation/IO/OSFile.h>
#include <Foundation/System/CrashHandler.h>
#include <Foundation/System/StackTracer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
  /// Spawns the Watchdog crash viewer (Code/Tools/CrashHandler,
  /// crashhandler_dev.exe, built next to this executable) with the report as
  /// its argument, the same way APPlayer does, so a crash shows the person at
  /// the machine what happened instead of a vanished window. Best effort: a
  /// build without the tool (Shipping, PartialCore without Tools) just skips it.
  void LaunchCrashViewer(const char* reportPath)
  {
    char exeDir[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
      return;
    char* lastSlash = std::strrchr(exeDir, '\\');
    if (!lastSlash)
      return;
    *(lastSlash + 1) = '\0';
    char commandLine[MAX_PATH * 2];
    _snprintf_s(commandLine, _TRUNCATE, "\"%scrashhandler_dev.exe\" \"%s\"", exeDir, reportPath);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    // BREAKAWAY_FROM_JOB: a game usually runs inside a job object (Steam, a
    // launcher, a test harness), and those kill the whole tree when the parent
    // dies - which is exactly what is happening to us. Break out so the viewer
    // outlives the crash; fall back to a plain spawn when the job forbids it.
    if (!CreateProcessA(nullptr, commandLine, nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, exeDir, &si, &pi)
      && !CreateProcessA(nullptr, commandLine, nullptr, nullptr, FALSE, 0, nullptr, exeDir, &si, &pi))
    {
      std::fprintf(stderr, "Watchdog viewer not launched: %s (error %lu)\n", commandLine,
        static_cast<unsigned long>(GetLastError()));
      std::fflush(stderr);
      return;
    }
    std::fprintf(stderr, "Watchdog viewer launched (pid %lu)\n", static_cast<unsigned long>(pi.dwProcessId));
    std::fflush(stderr);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }

  class CitySliceCrashHandler : public nsCrashHandler_WriteMiniDump
  {
  public:
    CrashContextWriter m_context;

    void HandleCrash(void* pOsSpecificData) override
    {
      // The frame log is fully buffered (see main.cpp): get it out first.
      std::fflush(stdout);
      std::fflush(stderr);

      // Truncate + close first: the resolver callback below reopens the file
      // in append mode, and holding this handle open makes every append fail.
      if (FILE* f = std::fopen(CitySliceCrashReportPath(), "w"))
      {
        std::fprintf(f, "CitySlice crashed.\n\n");
        std::fclose(f);
      }
      auto* exception = static_cast<_EXCEPTION_POINTERS*>(pOsSpecificData);
      if (FILE* f = std::fopen(CitySliceCrashReportPath(), "a"))
      {
        const unsigned code = exception && exception->ExceptionRecord
          ? static_cast<unsigned>(exception->ExceptionRecord->ExceptionCode) : 0u;
        const void* address = exception && exception->ExceptionRecord ? exception->ExceptionRecord->ExceptionAddress : nullptr;
        std::fprintf(f, "Exception 0x%08X at %p on thread %lu\n", code, address, static_cast<unsigned long>(GetCurrentThreadId()));
        if (code == 0xC0000005 && exception->ExceptionRecord->NumberParameters >= 2)
          std::fprintf(f, "Access violation: %s address %p\n",
            exception->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            reinterpret_cast<void*>(exception->ExceptionRecord->ExceptionInformation[1]));
        std::fprintf(f, "\nCall stack (most recent first):\n");
        std::fclose(f);
      }

      // GetStackTrace wants a CONTEXT record; ResolveStackTrace symbolises.
      // The exception's own record carries XSTATE flags that the tracer's
      // InitializeContext copy rejects (zero frames), so walk a CONTEXT_FULL
      // copy, and fall back to a capture from this handler's frame.
      void* frames[128] = {};
      nsArrayPtr<void*> trace(frames);
      nsUInt32 count = 0;
      if (exception && exception->ContextRecord)
      {
        CONTEXT ctx = *exception->ContextRecord;
        ctx.ContextFlags = CONTEXT_FULL;
        count = nsStackTracer::GetStackTrace(trace, &ctx);
      }
      if (count == 0)
        count = nsStackTracer::GetStackTrace(trace, nullptr);
      nsStackTracer::ResolveStackTrace(trace.GetSubArray(0, count), [](const char* text) {
        if (FILE* f = std::fopen(CitySliceCrashReportPath(), "a"))
        {
          std::fputs(text, f);
          std::fclose(f);
        }
      });

      if (m_context)
        if (FILE* f = std::fopen(CitySliceCrashReportPath(), "a"))
        {
          std::fprintf(f, "\nGame state:\n");
          m_context(f);
          std::fclose(f);
        }

      // Minidump next to the exe (CrashDump/CitySlice_<date>_<pid>.dmp), plus
      // the base class's own stack print to the log.
      nsCrashHandler_WriteMiniDump::HandleCrash(pOsSpecificData);

      if (FILE* f = std::fopen(CitySliceCrashReportPath(), "a"))
      {
        std::fprintf(f, "\nMinidump: %s\n", nsString(m_sDumpFilePath).GetData());
        std::fclose(f);
      }
      std::fprintf(stderr, "CitySlice crashed. Report: %s\n", CitySliceCrashReportPath());
      std::fflush(stderr);
      // Report fully written: hand it to the Watchdog viewer.
      LaunchCrashViewer(CitySliceCrashReportPath());
    }
  };
  CitySliceCrashHandler g_handler;
} // namespace

const char* CitySliceCrashReportPath()
{
  // %TEMP% so the report lands somewhere writable regardless of where the
  // game was launched from.
  static std::string s_path = [] {
    const char* tmp = std::getenv("TEMP");
    if (!tmp || !*tmp)
      tmp = std::getenv("TMP");
    std::string p = (tmp && *tmp) ? std::string(tmp) : std::string(".");
    p += "\\CitySlice-crash.txt";
    return p;
  }();
  return s_path.c_str();
}

void InstallCitySliceCrashHandler(CrashContextWriter contextWriter)
{
  g_handler.m_context = std::move(contextWriter);
  g_handler.SetDumpFilePath("CitySlice"); // {exe dir}/CrashDump/CitySlice_<date>_<pid>.dmp
  nsCrashHandler::SetCrashHandler(&g_handler);
}
