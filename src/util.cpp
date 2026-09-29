#include "util.h"

#include <cstdio>
#include <vector>

namespace loader {

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  // n counts the terminating NUL too. Convert into a buffer that really is that
  // long and drop the last byte afterwards: writing n bytes into a string of
  // n-1 characters would put the NUL on data()[size()], which the rule book
  // forbids (and which is exactly what the old version relied on).
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<size_t>(n - 1));
  return out;
}

std::wstring ModulePath(HMODULE module) {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD n = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0) return std::wstring();
    if (n < buffer.size() - 1) return std::wstring(buffer.data(), n);
    buffer.resize(buffer.size() * 2);
  }
}

std::wstring DirectoryOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring(L".");
  if (slash == 0) return path.substr(0, 1);
  return path.substr(0, slash);
}

std::wstring FileNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool FileExists(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirExists(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool SameName(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::string ErrorText(DWORD code) {
  char buf[320];
  snprintf(buf, sizeof(buf), "error %lu", static_cast<unsigned long>(code));
  LPWSTR text = nullptr;
  const DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
  if (n != 0 && text != nullptr) {
    std::string message = Narrow(std::wstring(text, n));
    LocalFree(text);
    while (!message.empty() &&
           (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
      message.pop_back();
    }
    snprintf(buf, sizeof(buf), "error %lu (%s)", static_cast<unsigned long>(code),
             message.c_str());
  }
  return buf;
}

namespace {

unsigned long long FileTimeValue(const FILETIME& ft) {
  return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

unsigned long ProcessUptimeMs() {
  FILETIME creation, exit, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return 0;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const unsigned long long created = FileTimeValue(creation);
  const unsigned long long current = FileTimeValue(now);
  // Both stamps come from the wall clock, so a clock step in the first second of
  // the process (NTP catching up, say) can make "now" look older than the process
  // itself. Read as a very old process it would skip the whole start-up wait --
  // the one thing this value must never do -- so an impossible difference counts
  // as 0: waiting too long is safe, loading too early is not.
  if (current <= created) return 0;
  return static_cast<unsigned long>((current - created) / 10000ULL);
}

}  // namespace loader
