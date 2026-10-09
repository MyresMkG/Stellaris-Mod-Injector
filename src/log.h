// Log file for the proxy loaders. All builds write the same file inside
// injected_mods, after that directory exists. Every start replaces
// the file: what is in there is the run that just happened, nothing older.
#pragma once

#include <string>

namespace loader {

// Opens |path| for writing (replacing what was there) and remembers it for
// Log().
void LogInit(const std::wstring& path);

// printf-style line, prefixed with milliseconds since the DLL was attached.
void Log(const char* fmt, ...);

}  // namespace loader
