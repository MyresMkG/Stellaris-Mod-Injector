#include "mods.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "util.h"

namespace loader {

std::vector<std::wstring> ListDlls(const std::wstring& dir) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW entry;
  ZeroMemory(&entry, sizeof(entry));
  const std::wstring pattern = dir + L"\\*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return out;
  do {
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
    const std::wstring name(entry.cFileName);
    if (name == L"." || name == L"..") continue;

    // Only files in immediate child directories are candidates. Do not scan
    // files in |dir| itself or descend into any of the child's directories.
    const std::wstring child = dir + L"\\" + name;
    WIN32_FIND_DATAW file;
    ZeroMemory(&file, sizeof(file));
    const std::wstring child_pattern = child + L"\\*";
    HANDLE child_find = FindFirstFileW(child_pattern.c_str(), &file);
    if (child_find == INVALID_HANDLE_VALUE) continue;
    do {
      if ((file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
      const std::wstring file_name(file.cFileName);
      if (file_name.size() < 4) continue;
      if (_wcsicmp(file_name.c_str() + (file_name.size() - 4), L".dll") != 0) continue;
      out.push_back(child + L"\\" + file_name);
    } while (FindNextFileW(child_find, &file));
    FindClose(child_find);
  } while (FindNextFileW(find, &entry));
  FindClose(find);

  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
    const int by_name = _wcsicmp(FileNameOf(a).c_str(), FileNameOf(b).c_str());
    return by_name != 0 ? by_name < 0 : _wcsicmp(a.c_str(), b.c_str()) < 0;
  });
  return out;
}

Mod Inspect(const std::wstring& path) {
  Mod mod;
  mod.path = path;
  mod.name = FileNameOf(path);

  // An empty sibling file disables this DLL before opening or inspecting it.
  // FileExists excludes directories; appending to the full path also keeps
  // markers from affecting a DLL with the same name in another mod folder.
  if (FileExists(path + L"noinject")) {
    mod.why = "disabled by " + Narrow(mod.name) + "noinject";
    return mod;
  }

  FILE* f = _wfopen(path.c_str(), L"rb");
  if (f == nullptr) {
    mod.why = "cannot open the file";
    return mod;
  }

  unsigned char dos[0x40];
  if (fread(dos, 1, sizeof(dos), f) != sizeof(dos)) {
    mod.why = "file is too small to be a PE image";
    fclose(f);
    return mod;
  }
  if (dos[0] != 'M' || dos[1] != 'Z') {
    mod.why = "not a PE image (no MZ signature)";
    fclose(f);
    return mod;
  }

  LONG e_lfanew = 0;
  memcpy(&e_lfanew, dos + 0x3c, sizeof(e_lfanew));
  if (e_lfanew <= 0 || fseek(f, e_lfanew, SEEK_SET) != 0) {
    mod.why = "not a PE image (bad header offset)";
    fclose(f);
    return mod;
  }

  unsigned char nt[24];
  if (fread(nt, 1, sizeof(nt), f) != sizeof(nt)) {
    mod.why = "truncated PE header";
    fclose(f);
    return mod;
  }
  fclose(f);

  if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) {
    mod.why = "not a PE image (no PE signature)";
    return mod;
  }

  unsigned short machine = 0;
  memcpy(&machine, nt + 4, sizeof(machine));
  unsigned short characteristics = 0;
  memcpy(&characteristics, nt + 22, sizeof(characteristics));
  if ((characteristics & 0x2000) == 0) {  // IMAGE_FILE_DLL
    mod.why = "not a DLL image";
    return mod;
  }
  if (machine != 0x8664) {  // IMAGE_FILE_MACHINE_AMD64
    char buf[96];
    snprintf(buf, sizeof(buf), "not an x64 DLL (machine 0x%04x, the game is x64)",
             static_cast<unsigned>(machine));
    mod.why = buf;
    return mod;
  }
  mod.loadable = true;
  return mod;
}

bool ModuleLoaded(const std::wstring& path) {
  // GetModuleHandleExW normalises the path on the way (case, '/' against '\',
  // 8.3 names); comparing Module32*'s szExePath as a plain string does not, so a
  // module loaded through another spelling of the same file read as a fresh load
  // in the log. The reference count is deliberately left alone: the caller only
  // asks whether the module is already there.
  HMODULE module = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, path.c_str(),
                            &module) != 0;
}

HMODULE Load(const std::wstring& path, std::string* why) {
  HMODULE module = LoadLibraryW(path.c_str());
  if (module == nullptr && why != nullptr) *why = ErrorText(GetLastError());
  return module;
}

bool CreateProbeFlag(const std::wstring& dll_path) {
  const std::wstring flag = DirectoryOf(dll_path) + L"\\diplo_action_hook_probe_only.txt";
  if (FileExists(flag)) return true;
  FILE* f = _wfopen(flag.c_str(), L"wb");
  if (f == nullptr) return false;
  fputs("probe only\n", f);
  fclose(f);
  return true;
}

bool RemoveProbeFlag(const std::wstring& dll_path) {
  const std::wstring flag = DirectoryOf(dll_path) + L"\\diplo_action_hook_probe_only.txt";
  if (DeleteFileW(flag.c_str())) return true;
  const DWORD error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

}  // namespace loader
