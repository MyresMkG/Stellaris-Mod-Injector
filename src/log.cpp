#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "util.h"

namespace loader {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
DWORD g_start = 0;
HANDLE g_log_owner = nullptr;  // held for the life of the process, see LogInit

}  // namespace

void LogInit(const std::wstring& path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_start = GetTickCount();

  // A player may install more than one proxy name, and all of them report
  // into this one file. The first proxy to get here replaces the file, the ones
  // that follow add to it, so the file is still "this run only" without throwing
  // the second proxy's lines away. The name carries the pid, so the next game
  // start starts a fresh file. The handle stays open: closing it would destroy
  // the mutex and make the next proxy think it is the first one.
  wchar_t mutex_name[64];
  swprintf(mutex_name, 64, L"Local\\stellaris_mod_loader_log_%lu",
           static_cast<unsigned long>(GetCurrentProcessId()));
  g_log_owner = CreateMutexW(nullptr, FALSE, mutex_name);
  const bool fresh = g_log_owner == nullptr || GetLastError() != ERROR_ALREADY_EXISTS;

  // "w" for the first proxy of a run, "a" for the others.
  g_file = _wfopen(path.c_str(), fresh ? L"w" : L"a");
}

void Log(const char* fmt, ...) {
  char body[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  const DWORD now = GetTickCount();
  char line[2200];
  snprintf(line, sizeof(line), "[%6lu.%03lu] %s\n",
           static_cast<unsigned long>((now - g_start) / 1000),
           static_cast<unsigned long>((now - g_start) % 1000), body);

  OutputDebugStringA(line);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_file != nullptr) {
    fputs(line, g_file);
    fflush(g_file);
  }
}

}  // namespace loader
