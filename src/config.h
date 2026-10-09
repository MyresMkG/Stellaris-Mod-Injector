// <injected_mods>\stellaris_mod_injector.ini is created with defaults if missing.
// A missing key or an unusable value means "use the default".
#pragma once

#include <string>

namespace loader {

struct Settings {
  // How long to wait after the process was created before loading the mods.
  // The injector's --delay is the same knob, with the same default.
  unsigned long delay_ms = 700;
  // Resolve addresses without installing hooks in mods that support probe flags.
  bool probe = false;
};

// Creates only a missing file, preserving existing player settings. Returns
// false with a Win32 reason in |why| if creation or writing fails.
bool EnsureDefaultSettings(const std::wstring& ini_path, bool* created, std::string* why);

Settings LoadSettings(const std::wstring& ini_path);

}  // namespace loader
