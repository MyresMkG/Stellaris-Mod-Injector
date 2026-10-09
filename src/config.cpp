#include "config.h"

#include <windows.h>

#include <cstdlib>
#include <cstdio>
#include <string>

#include "log.h"
#include "util.h"

namespace loader {
namespace {

std::string Trim(const std::string& text) {
  size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) return std::string();
  size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

// The UTF-16 code units of |raw| (its two BOM bytes excluded) as UTF-8 bytes.
std::string Utf16ToUtf8(const std::string& raw, bool big_endian) {
  const size_t units = (raw.size() - 2) / 2;
  std::wstring wide(units, L'\0');
  for (size_t i = 0; i < units; ++i) {
    const unsigned char a = static_cast<unsigned char>(raw[2 + i * 2]);
    const unsigned char b = static_cast<unsigned char>(raw[3 + i * 2]);
    const unsigned int unit = big_endian ? ((a << 8) | b) : ((b << 8) | a);
    wide[i] = static_cast<wchar_t>(unit);
  }
  return Narrow(wide);
}

// What the player's editor put in front of the text. Notepad's "UTF-8" used to
// mean "UTF-8 with BOM" and its "Unicode" is UTF-16LE, so both turn up in a
// hand-written ini. A BOM left in place makes the first key unrecognised, and
// the setting then silently does nothing. Everything the parser reads is ASCII,
// so the BOMs are normalised away instead of being passed on.
std::string WithoutBom(const std::string& raw) {
  if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) return raw.substr(3);
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
      static_cast<unsigned char>(raw[1]) == 0xFE) {
    return Utf16ToUtf8(raw, false);
  }
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE &&
      static_cast<unsigned char>(raw[1]) == 0xFF) {
    return Utf16ToUtf8(raw, true);
  }
  return raw;
}

}  // namespace

bool EnsureDefaultSettings(const std::wstring& ini_path, bool* created, std::string* why) {
  if (created != nullptr) *created = false;
  // CREATE_NEW prevents one proxy from overwriting another proxy's file, and
  // never truncates settings the player already has.
  HANDLE file = CreateFileW(ini_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if ((error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) && FileExists(ini_path)) {
      return true;
    }
    if (why != nullptr) *why = ErrorText(error);
    return false;
  }
  const char defaults[] =
      "# Time in milliseconds from process creation before loading mods.\n"
      "# Default: 700. Valid range: 1..599999.\n"
      "delay_ms=700\n"
      "# Probe mode: 0 = normal hooks (default), 1 = resolve addresses only.\n"
      "probe=0\n";
  DWORD written = 0;
  const DWORD size = static_cast<DWORD>(sizeof(defaults) - 1);
  const bool ok = WriteFile(file, defaults, size, &written, nullptr) != 0;
  const DWORD error = ok ? ERROR_WRITE_FAULT : GetLastError();
  CloseHandle(file);
  if (!ok || written != size) {
    if (why != nullptr) *why = ErrorText(error);
    return false;
  }
  if (created != nullptr) *created = true;
  return true;
}

Settings LoadSettings(const std::wstring& ini_path) {
  Settings out;
  FILE* f = _wfopen(ini_path.c_str(), L"rb");
  if (f == nullptr) return out;

  std::string text;
  char chunk[1024];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, n);
  fclose(f);
  text = WithoutBom(text);

  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string line = Trim(text.substr(at, end - at));
    at = end + 1;
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;

    const size_t eq = line.find('=');
    if (eq == std::string::npos) {
      Log("ini   : line without '=' ignored: %s", line.c_str());
      continue;
    }
    const std::string key = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (_stricmp(key.c_str(), "probe") == 0) {
      if (value == "0" || value == "1") {
        out.probe = value == "1";
      } else {
        Log("ini   : probe '%s' is not 0 or 1; using %u", value.c_str(), out.probe ? 1u : 0u);
      }
      continue;
    }
    if (_stricmp(key.c_str(), "delay_ms") != 0) {
      // A typo here used to be a no-op with nothing in the log to show for it --
      // which is also how a wrong file encoding goes unnoticed.
      Log("ini   : unknown key '%s' ignored", key.c_str());
      continue;
    }
    const unsigned long v = strtoul(value.c_str(), nullptr, 10);
    if (v > 0 && v < 600000) {
      out.delay_ms = v;
    } else {
      Log("ini   : delay_ms '%s' is not 1..599999; using %lu", value.c_str(),
          static_cast<unsigned long>(out.delay_ms));
    }
  }

  // One line saying what was used, so the effective delay never has to be
  // guessed from the (absent) "waiting" line.
  Log("ini   : %s (delay_ms=%lu)", Narrow(ini_path).c_str(),
      static_cast<unsigned long>(out.delay_ms));
  Log("ini   : probe=%u", out.probe ? 1u : 0u);
  return out;
}

}  // namespace loader
