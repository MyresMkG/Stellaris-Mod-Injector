// Optional <injected_mods>\stellaris_mod_loader.ini. A missing file, a missing
// key or an unusable value all mean "use the default", so the DLL can simply be
// dropped into the game folder.
#pragma once

#include <string>

namespace loader {

struct Settings {
  // How long to wait after the process was created before loading the mods.
  // The injector's --delay is the same knob, with the same default.
  unsigned long delay_ms = 700;
};

Settings LoadSettings(const std::wstring& ini_path);

}  // namespace loader
