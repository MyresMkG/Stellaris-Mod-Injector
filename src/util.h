// Small shared helpers: paths, text conversion and Win32 error strings.
#pragma once

#include <windows.h>

#include <string>

namespace loader {

// UTF-8 view of a wide string, for log lines.
std::string Narrow(const std::wstring& wide);

// Full path of a module, "" when it cannot be read. NULL means the host exe.
std::wstring ModulePath(HMODULE module);

std::wstring DirectoryOf(const std::wstring& path);
std::wstring FileNameOf(const std::wstring& path);

bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);

// Windows compares file names case-insensitively; the loader does the same.
bool SameName(const std::wstring& a, const std::wstring& b);

// "error 5 (access denied)", or "error 9999" when there is no message for it.
std::string ErrorText(DWORD code);

// Milliseconds since this process was created. The proxy waits out the game's
// own start-up with it, exactly like the injector's --delay does from outside.
unsigned long ProcessUptimeMs();

}  // namespace loader
