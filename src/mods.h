// The DLLs that live in immediate child directories of injected_mods, and the checks
// the injector also runs
// before it hands one to the game.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace loader {

struct Mod {
  std::wstring path;
  std::wstring name;
  bool loadable = false;
  std::string why;  // rejection reason, empty when loadable
};

// *.dll files in immediate child directories of |dir|, in file-name order
// (full path breaks ties). Excludes |dir|'s own files and deeper directories.
std::vector<std::wstring> ListDlls(const std::wstring& dir);

// Fills in path/name, skips DLLs with a sibling <name>.dllnoinject file, then
// runs the injector's PE check (a DLL, x64).
Mod Inspect(const std::wstring& path);

// True when a module with this path is already loaded in this process. The path
// only has to name the same file, it does not have to be spelled the same way.
bool ModuleLoaded(const std::wstring& path);

// LoadLibraryW of the full path. On failure |why| carries the Win32 reason.
HMODULE Load(const std::wstring& path, std::string* why);

// Creates diplo_action_hook_probe_only.txt next to the DLL -- what the
// injector's --probe flag does -- so diplo_action_hook only resolves addresses
// and installs nothing.
bool CreateProbeFlag(const std::wstring& dll_path);

// Removes a previous probe flag so probe=0 takes effect on the next load.
// A missing flag already satisfies this operation.
bool RemoveProbeFlag(const std::wstring& dll_path);

}  // namespace loader
