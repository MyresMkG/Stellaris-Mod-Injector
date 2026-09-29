// stellaris injected_mods loader.
//
// Put this executable next to stellaris.exe (the game root). It finds the game
// on its own, starts it, and then loads *every* DLL in
//
//     <game root>\injected_mods\*.dll
//
// into the running process with a remote LoadLibraryW call. Nothing on disk is
// patched -- the modules only ever exist inside the process.
//
// This is the loader diplo_action_hook.dll is meant to be used with: drop the
// hook DLL into injected_mods, start the game through this executable, and the
// hooks are in place before the game parses common/diplomatic_actions.
//
// usage:
//   stellaris_mod_injector.exe [options] [-- <game args>]
//
// Every path is handled as UTF-16, so directories outside the active code page
// work. Injection is done one DLL at a time, in file-name order, and one bad
// DLL never stops the others.

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);

const wchar_t* kGameExeName = L"stellaris.exe";
const wchar_t* kModsDirName = L"injected_mods";
const wchar_t* kProbeFlagName = L"diplo_action_hook_probe_only.txt";

struct Options {
  std::wstring exe;
  std::wstring mods_dir;
  std::vector<std::wstring> dlls;  // --dll, injected before the scanned ones
  std::wstring attach;
  std::wstring delay;
  std::wstring game_args;
  bool suspend = false;
  bool wait = false;
  bool probe = false;
  bool list = false;
  bool new_instance = false;
  bool show_help = false;
};

struct Candidate {
  std::wstring path;
  std::wstring name;
  bool injectable = false;
  std::string why;  // rejection reason, empty when injectable
};

struct Injection {
  bool ok = false;
  DWORD error = 0;
  unsigned long long module = 0;
  bool already_loaded = false;
  bool target_exited = false;  // the process was already gone when this ran
  DWORD exit_code = 0;
};

// ---------------------------------------------------------------- text helpers

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

std::string Narrow(const wchar_t* wide) {
  return wide == nullptr ? std::string() : Narrow(std::wstring(wide));
}

void Say(const std::string& utf8) { fputs(utf8.c_str(), stdout); }

// Re-quote one forwarded game argument so the target parses it back the way it
// arrived here: a quote that belongs to the argument is escaped and the
// backslashes in front of it are doubled, which is what CommandLineToArgvW
// expects to undo.
std::wstring QuoteArg(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
  std::wstring out;
  out.push_back(L'"');
  for (size_t i = 0; i < arg.size();) {
    size_t slashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      slashes += 1;
      i += 1;
    }
    if (i == arg.size()) {  // they sit in front of the closing quote
      out.append(slashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(slashes * 2 + 1, L'\\');
    } else {
      out.append(slashes, L'\\');
    }
    out.push_back(arg[i]);
    i += 1;
  }
  out.push_back(L'"');
  return out;
}

// %p for the module handle: the value is a pointer in the target, and it keeps
// the output identical no matter which C runtime is linked in.
std::string Hex(unsigned long long value) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%p", reinterpret_cast<void*>(static_cast<ULONG_PTR>(value)));
  return buf;
}

std::string Number(unsigned long long value) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(value));
  return buf;
}

// Every failure is printed where it happens, but the window that is about to
// close can scroll the reason off screen. The text is kept here as well, and
// listed again in the summary the pause prints.
std::vector<std::string> g_problems;

// A DLL the PE check threw out does not fail the run, but it is exactly the kind
// of thing a window that closes by itself would hide: the mod silently is not
// there. Remembering it makes the pause happen for that case too.
bool g_skipped_any = false;

void Problem(const std::string& line) {
  Say(line + "\n");
  g_problems.push_back(line);
}

// A console this tool created for itself (started by double clicking, or from a
// shell) closes the moment the process exits, so an error report would flash
// past unseen. Wait for Enter when that is the case -- but only when standard
// input really is that console, so a script or a redirected run is not held up.
void WaitForAck(int exit_code) {
  DWORD mode = 0;
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  if (GetConsoleWindow() == nullptr || input == nullptr ||
      !GetConsoleMode(input, &mode)) {
    return;
  }
  // Repeat the reasons, in the order they happened, right where the window is
  // about to disappear. A run that only skipped something still exits 0, so the
  // heading says which of the two it is.
  if (!g_problems.empty()) {
    Say("\n");
    const char* kind = exit_code != 0 ? "problem" : "warning";
    if (g_problems.size() == 1) {
      Say(std::string("1 ") + kind + ":\n");
    } else {
      Say(Number(g_problems.size()) + " " + kind + "s:\n");
    }
    for (const std::string& problem : g_problems) {
      Say("  * " + problem + "\n");
    }
  }
  Say("\npress Enter to close this window (exit code " +
      Number(static_cast<unsigned long long>(exit_code)) + ")\n");
  fflush(stdout);
  for (int c = getchar(); c != '\n' && c != EOF; c = getchar()) {
  }
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

bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

bool SameDir(const std::wstring& a, const std::wstring& b) {
  std::wstring x = a, y = b;
  while (!x.empty() && (x.back() == L'\\' || x.back() == L'/')) x.pop_back();
  while (!y.empty() && (y.back() == L'\\' || y.back() == L'/')) y.pop_back();
  return _wcsicmp(x.c_str(), y.c_str()) == 0;
}

// ------------------------------------------------------------- path helpers

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

std::wstring SelfPath() {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(),
                                       static_cast<DWORD>(buffer.size()));
    if (n == 0) return std::wstring();
    if (n < buffer.size() - 1) return std::wstring(buffer.data(), n);
    buffer.resize(buffer.size() * 2);
  }
}

// Absolute form of |path|. The target process does its own resolving relative
// to *its* working directory, so a relative DLL path would be checked here and
// loaded there, which is not necessarily the same file.
std::wstring FullPath(const std::wstring& path) {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()),
                                     buffer.data(), nullptr);
    if (n == 0) return path;  // cannot be resolved: leave it for the checks below
    if (n < buffer.size()) return std::wstring(buffer.data(), n);
    buffer.resize(static_cast<size_t>(n) + 1);
  }
}

// Folders that must never be scanned for DLLs by accident (System32 and
// friends); only an explicit --mods-dir may point there.
bool IsSystemDirectory(const std::wstring& dir) {
  std::vector<wchar_t> windows(MAX_PATH);
  const UINT n = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
  if (n == 0 || n >= windows.size()) return false;
  const std::wstring root(windows.data(), n);
  return SameDir(dir, root) || SameDir(dir, root + L"\\System32") ||
         SameDir(dir, root + L"\\SysWOW64") || SameDir(dir, root + L"\\system32");
}

// ------------------------------------------------------------------ PE check

struct PeInfo {
  bool ok = false;
  bool is_dll = false;
  unsigned short machine = 0;
  std::string why;
};

PeInfo InspectPe(const std::wstring& path) {
  PeInfo info;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (f == nullptr) {
    info.why = "cannot open the file";
    return info;
  }

  unsigned char dos[0x40];
  if (fread(dos, 1, sizeof(dos), f) != sizeof(dos)) {
    info.why = "file is too small to be a PE image";
    fclose(f);
    return info;
  }
  if (dos[0] != 'M' || dos[1] != 'Z') {
    info.why = "not a PE image (no MZ signature)";
    fclose(f);
    return info;
  }

  LONG e_lfanew = 0;
  memcpy(&e_lfanew, dos + 0x3c, sizeof(e_lfanew));
  if (e_lfanew <= 0 || fseek(f, e_lfanew, SEEK_SET) != 0) {
    info.why = "not a PE image (bad header offset)";
    fclose(f);
    return info;
  }

  unsigned char nt[24];
  if (fread(nt, 1, sizeof(nt), f) != sizeof(nt)) {
    info.why = "truncated PE header";
    fclose(f);
    return info;
  }
  fclose(f);

  if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0) {
    info.why = "not a PE image (no PE signature)";
    return info;
  }

  memcpy(&info.machine, nt + 4, sizeof(info.machine));
  unsigned short characteristics = 0;
  memcpy(&characteristics, nt + 22, sizeof(characteristics));
  info.is_dll = (characteristics & 0x2000) != 0;  // IMAGE_FILE_DLL

  if (!info.is_dll) {
    info.why = "not a DLL image";
    return info;
  }
  if (info.machine != 0x8664) {  // IMAGE_FILE_MACHINE_AMD64
    char buf[96];
    snprintf(buf, sizeof(buf), "not an x64 DLL (machine 0x%04x, the game is x64)",
             static_cast<unsigned>(info.machine));
    info.why = buf;
    return info;
  }
  info.ok = true;
  return info;
}

// ----------------------------------------------------------- file enumeration

std::vector<std::wstring> ListDlls(const std::wstring& dir) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW entry;
  ZeroMemory(&entry, sizeof(entry));
  const std::wstring pattern = dir + L"\\*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return out;
  do {
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::wstring name(entry.cFileName);
    if (name.size() < 4) continue;
    if (_wcsicmp(name.c_str() + (name.size() - 4), L".dll") != 0) continue;
    out.push_back(dir + L"\\" + name);
  } while (FindNextFileW(find, &entry));
  FindClose(find);

  std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(FileNameOf(a).c_str(), FileNameOf(b).c_str()) < 0;
  });
  return out;
}

// -------------------------------------------------------------- process work

DWORD FindProcessByName(const std::wstring& exe_name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W entry;
  ZeroMemory(&entry, sizeof(entry));
  entry.dwSize = sizeof(entry);
  DWORD found = 0;
  if (Process32FirstW(snap, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, exe_name.c_str()) == 0) {
        found = entry.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return found;
}

// True when a module with this path is already loaded in the target. Used to
// double-check a zero exit code (the thread exit code is only 32 bits wide).
bool ModuleLoadedIn(DWORD pid, const std::wstring& dll_path) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
  if (snap == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W entry;
  ZeroMemory(&entry, sizeof(entry));
  entry.dwSize = sizeof(entry);
  bool found = false;
  if (Module32FirstW(snap, &entry)) {
    do {
      if (SamePath(entry.szExePath, dll_path)) {
        found = true;
        break;
      }
    } while (Module32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return found;
}

HANDLE OpenTarget(DWORD pid, DWORD* last_error) {
  // SYNCHRONIZE is what --wait needs; without it WaitForSingleObject fails on a
  // handle that otherwise opens fine.
  HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                   PROCESS_VM_READ | SYNCHRONIZE,
                               FALSE, pid);
  if (process == nullptr) *last_error = GetLastError();
  return process;
}

// LoadLibraryW sits at the same address in every process of a session because
// system DLLs share one ASLR base, so the local address is valid in the target.
LoadLibraryWFn TargetLoadLibrary() {
  const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (kernel32 == nullptr) return nullptr;
  return reinterpret_cast<LoadLibraryWFn>(
      reinterpret_cast<void*>(GetProcAddress(kernel32, "LoadLibraryW")));
}

Injection InjectOne(HANDLE process, DWORD pid, LoadLibraryWFn load_library_w,
                    const std::wstring& dll_path) {
  Injection result;
  // A process that is already gone makes every call below fail with
  // ERROR_ACCESS_DENIED, which says nothing about the DLL; say what really
  // happened instead.
  auto fail = [&](DWORD code) {
    result.error = code;
    DWORD exit_code = 0;
    if (GetExitCodeProcess(process, &exit_code) && exit_code != STILL_ACTIVE) {
      result.target_exited = true;
      result.exit_code = exit_code;
    }
    return result;
  };

  const SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
  void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
  if (remote == nullptr) return fail(GetLastError());

  if (!WriteProcessMemory(process, remote, dll_path.c_str(), bytes, nullptr)) {
    const DWORD error = GetLastError();
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return fail(error);
  }

  HANDLE thread = CreateRemoteThread(
      process, nullptr, 0,
      reinterpret_cast<LPTHREAD_START_ROUTINE>(
          reinterpret_cast<void*>(load_library_w)),
      remote, 0, nullptr);
  if (thread == nullptr) {
    const DWORD error = GetLastError();
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return fail(error);
  }

  const DWORD waited = WaitForSingleObject(thread, 30000);
  DWORD module = 0;
  GetExitCodeThread(thread, &module);
  CloseHandle(thread);
  if (waited == WAIT_TIMEOUT) {
    // The remote thread may still be inside LoadLibraryW reading the path, so
    // the buffer is deliberately left behind instead of being freed under it.
    return fail(ERROR_TIMEOUT);
  }
  VirtualFreeEx(process, remote, 0, MEM_RELEASE);

  result.module = module;
  if (module != 0) {
    result.ok = true;
    return result;
  }
  // The exit code is a truncated pointer; verify against the module list before
  // calling this a failure.
  if (ModuleLoadedIn(pid, dll_path)) {
    result.ok = true;
    result.already_loaded = true;
    return result;
  }
  return fail(ERROR_DLL_INIT_FAILED);
}

// ----------------------------------------------------------- game discovery

// Read a REG_SZ from HKCU.
std::wstring ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return std::wstring();
  wchar_t buf[1024];
  DWORD size = sizeof(buf);
  DWORD type = 0;
  std::wstring out;
  if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
          ERROR_SUCCESS &&
      type == REG_SZ && size >= sizeof(wchar_t)) {
    const size_t chars = size / sizeof(wchar_t);
    size_t length = 0;
    while (length < chars && buf[length] != L'\0') ++length;
    if (length >= chars) --length;
    out.assign(buf, length);
  }
  RegCloseKey(key);
  return out;
}

std::wstring ReadFileUtf8(const std::wstring& path) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (f == nullptr) return std::wstring();
  std::string data;
  char chunk[4096];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) data.append(chunk, n);
  fclose(f);
  if (data.empty()) return std::wstring();
  const int wide = MultiByteToWideChar(CP_UTF8, 0, data.c_str(),
                                       static_cast<int>(data.size()), nullptr, 0);
  if (wide <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, data.c_str(), static_cast<int>(data.size()),
                      out.data(), wide);
  return out;
}

// Every "path" value in a libraryfolders.vdf, with \" and \\ unescaped.
std::vector<std::wstring> LibraryPathsFromVdf(const std::wstring& path) {
  std::vector<std::wstring> out;
  const std::wstring text = ReadFileUtf8(path);
  size_t at = 0;
  while ((at = text.find(L"\"path\"", at)) != std::wstring::npos) {
    at += 6;
    size_t open = text.find(L'"', at);
    if (open == std::wstring::npos) break;
    size_t close = text.find(L'"', open + 1);
    if (close == std::wstring::npos) break;
    std::wstring value;
    for (size_t i = open + 1; i < close; ++i) {
      if (text[i] == L'\\' && i + 1 < close && text[i + 1] == L'\\') continue;
      value.push_back(text[i]);
    }
    if (!value.empty()) out.push_back(value);
    at = close + 1;
  }
  return out;
}

// Best effort: use the Steam registry entry and its library folders.
std::wstring FindInstalledGame() {
  const std::wstring steam = ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam",
                                               L"SteamPath");
  if (steam.empty()) return std::wstring();

  std::vector<std::wstring> roots;
  roots.push_back(steam);
  for (const std::wstring& lib : LibraryPathsFromVdf(steam + L"\\steamapps\\libraryfolders.vdf")) {
    roots.push_back(lib);
  }
  for (const std::wstring& lib : LibraryPathsFromVdf(steam + L"\\config\\libraryfolders.vdf")) {
    roots.push_back(lib);
  }

  for (std::wstring root : roots) {
    std::replace(root.begin(), root.end(), L'/', L'\\');
    while (!root.empty() && root.back() == L'\\') root.pop_back();
    const std::wstring candidate =
        root + L"\\steamapps\\common\\Stellaris\\" + kGameExeName;
    if (FileExists(candidate)) return candidate;
  }
  return std::wstring();
}

// -------------------------------------------------------------------- usage

void Usage(const std::wstring& self) {
  Say("usage: " + Narrow(FileNameOf(self)) + " [options] [-- <game args>]\n");
  Say("\n");
  Say("Loads every DLL from the injected_mods folder into Stellaris. Put this\n");
  Say("executable next to stellaris.exe and run it; the game files stay untouched.\n");
  Say("\n");
  Say("options:\n");
  Say("  --exe <path>        game executable (default: stellaris.exe next to this\n");
  Say("                      tool, else the Steam library that has it installed)\n");
  Say("  --mods-dir <path>   folder whose *.dll files are injected (default:\n");
  Say("                      injected_mods next to this tool, else next to the game)\n");
  Say("  --dll <path>        inject this DLL as well; repeatable, and these are\n");
  Say("                      injected before the ones found by the scan\n");
  Say("  --attach <name|pid> inject into an already running process instead of\n");
  Say("                      starting the game\n");
  Say("  --delay <ms>        wait this long after starting the game before injecting\n");
  Say("                      (default 700; the game is still loading its data then)\n");
  Say("  --new-instance      start the game even when a copy of it is already\n");
  Say("                      running (otherwise the running copy is injected into)\n");
  Say("  --list              only print what would be injected, then exit\n");
  Say("  --probe             create " + Narrow(kProbeFlagName) + "\n");
  Say("                      next to each injected DLL, so diplo_action_hook.dll only\n");
  Say("                      reports addresses instead of hooking anything\n");
  Say("  --wait              wait for the game to exit\n");
  Say("  --suspend           NOT RECOMMENDED: create the game suspended and inject\n");
  Say("                      before it runs a single instruction. Verified to crash\n");
  Say("                      Stellaris 4.5 on this machine. Off by default.\n");
  Say("  --help, -h          this text\n");
}

// --------------------------------------------------------------------- main

bool ParseArgs(int argc, wchar_t** argv, Options* opt, bool* bad) {
  *bad = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto value = [&](std::wstring* out) {
      if (i + 1 < argc) {
        *out = argv[++i];
        return true;
      }
      return false;
    };
    auto need_value = [&](std::wstring* out, const char* what) {
      if (value(out)) return true;
      Problem(std::string(what) + " needs a value");
      *bad = true;
      return false;
    };
    if (a == L"--exe") {
      if (!need_value(&opt->exe, "--exe")) return false;
    } else if (a == L"--mods-dir") {
      if (!need_value(&opt->mods_dir, "--mods-dir")) return false;
    } else if (a == L"--dll") {
      std::wstring d;
      if (!need_value(&d, "--dll")) return false;
      opt->dlls.push_back(d);
    } else if (a == L"--attach") {
      if (!need_value(&opt->attach, "--attach")) return false;
    } else if (a == L"--delay") {
      if (!need_value(&opt->delay, "--delay")) return false;
    } else if (a == L"--suspend") {
      opt->suspend = true;
    } else if (a == L"--no-suspend") {
      opt->suspend = false;
    } else if (a == L"--new-instance") {
      opt->new_instance = true;
    } else if (a == L"--list") {
      opt->list = true;
    } else if (a == L"--probe") {
      opt->probe = true;
    } else if (a == L"--wait") {
      opt->wait = true;
    } else if (a == L"--help" || a == L"-h" || a == L"/?") {
      opt->show_help = true;
    } else if (a == L"--") {
      for (int j = i + 1; j < argc; ++j) {
        opt->game_args += L" ";
        opt->game_args += QuoteArg(argv[j]);
      }
      break;
    } else {
      Problem("unknown argument: " + Narrow(a));
      *bad = true;
      return false;
    }
  }
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);

  Options opt;
  bool bad = false;
  if (!ParseArgs(argc, argv, &opt, &bad) || bad) {
    Say("\n");
    Usage(argv[0]);
    return 1;
  }
  if (opt.show_help) {
    Usage(argv[0]);
    return 0;
  }

  const std::wstring self = SelfPath();
  const std::wstring self_dir = DirectoryOf(self);

  // ---- game executable ----
  std::wstring exe = opt.exe;
  if (exe.empty() && opt.attach.empty()) {
    const std::wstring beside = self_dir + L"\\" + kGameExeName;
    if (FileExists(beside)) {
      exe = beside;
    } else {
      exe = FindInstalledGame();
      if (!exe.empty()) {
        Say("note: " + Narrow(kGameExeName) + " is not next to this tool; using the installed copy\n");
      }
    }
    if (exe.empty()) {
      Problem("game not found: no " + Narrow(kGameExeName) + " next to this tool (" +
              Narrow(self_dir) + ") and none in the Steam libraries");
      Say("put this executable next to " + Narrow(kGameExeName) + ", or pass --exe <path>\n");
      return 1;
    }
  }
  if (!exe.empty() && !FileExists(exe)) {
    Problem("game not found: " + Narrow(exe));
    return 1;
  }
  if (!exe.empty()) exe = FullPath(exe);

  // ---- folder that holds the DLLs ----
  std::wstring mods = opt.mods_dir;
  const bool derived_mods_dir = mods.empty();
  if (mods.empty()) {
    const std::wstring own = self_dir + L"\\" + kModsDirName;
    if (DirExists(own)) {
      mods = own;
    } else if (!exe.empty()) {
      mods = DirectoryOf(exe) + L"\\" + kModsDirName;
    } else {
      mods = own;
    }
  }
  if (derived_mods_dir && IsSystemDirectory(mods)) {
    Problem("refusing to scan " + Narrow(mods) + ": that is a Windows system folder");
    Say("pass --mods-dir to say explicitly where the DLLs are\n");
    return 1;
  }

  // ---- collect the DLLs ----
  std::vector<Candidate> candidates;
  std::vector<std::wstring> paths;
  for (const std::wstring& dll : opt.dlls) {
    const std::wstring full = FullPath(dll);
    bool seen = false;
    for (const std::wstring& other : paths) seen = seen || SamePath(other, full);
    if (!seen) paths.push_back(full);
  }
  const bool mods_dir_present = DirExists(mods);
  if (mods_dir_present) {
    for (const std::wstring& dll : ListDlls(mods)) {
      const std::wstring full = FullPath(dll);
      bool seen = false;
      for (const std::wstring& other : paths) seen = seen || SamePath(other, full);
      if (!seen) paths.push_back(full);
    }
  }

  Say("game : " + (exe.empty() ? std::string("(attach only)") : Narrow(exe)) + "\n");
  Say("mods : " + Narrow(mods) + (mods_dir_present ? "" : "  (does not exist)") + "\n");
  Say("dlls : " + Number(paths.size()) + " candidate(s)\n");

  for (const std::wstring& path : paths) {
    Candidate c;
    c.path = path;
    c.name = FileNameOf(path);
    if (!FileExists(path)) {
      c.why = "file not found";
    } else {
      const PeInfo info = InspectPe(path);
      c.injectable = info.ok;
      c.why = info.why;
    }
    candidates.push_back(c);
  }

  std::vector<Candidate*> injectable;
  std::vector<Candidate*> rejected;
  for (Candidate& c : candidates) {
    if (c.injectable) {
      injectable.push_back(&c);
    } else {
      rejected.push_back(&c);
    }
  }

  for (size_t i = 0; i < injectable.size(); ++i) {
    Say("  [" + Number(i + 1) + "] " + Narrow(injectable[i]->name) + "\n");
  }
  for (const Candidate* c : rejected) {
    Say("  [skip] " + Narrow(c->name) + ": " + c->why + "\n");
    g_problems.push_back("skipped " + Narrow(c->name) + ": " + c->why);
    g_skipped_any = true;
  }
  if (!mods_dir_present && opt.dlls.empty()) {
    Say("\n");
    Problem(Narrow(mods) + " does not exist; create it and put the DLLs to inject in it");
  }

  if (opt.list) {
    Say("--list: nothing was injected\n");
    if (injectable.empty()) Problem("no injectable DLL found in " + Narrow(mods));
    return injectable.empty() ? 1 : 0;
  }

  if (injectable.empty()) {
    Say("\n");
    Problem("nothing to inject; no process was touched");
    return 1;
  }

  // ---- probe flag ----
  if (opt.probe) {
    std::vector<std::wstring> done;
    for (Candidate* c : injectable) {
      const std::wstring dir = DirectoryOf(c->path);
      bool seen = false;
      for (const std::wstring& other : done) seen = seen || SameDir(other, dir);
      if (seen) continue;
      done.push_back(dir);
      const std::wstring flag = dir + L"\\" + kProbeFlagName;
      if (FileExists(flag)) {
        Say("probe : " + Narrow(flag) + " already present\n");
        continue;
      }
      FILE* f = _wfopen(flag.c_str(), L"wb");
      if (f != nullptr) {
        fputs("probe only\n", f);
        fclose(f);
        Say("probe : created " + Narrow(flag) + "\n");
      } else {
        Say("probe : could not create " + Narrow(flag) + "\n");
      }
    }
  }

  const LoadLibraryWFn load_library_w = TargetLoadLibrary();
  if (load_library_w == nullptr) {
    Problem("could not find LoadLibraryW in kernel32.dll");
    return 1;
  }

  // ---- get a process to inject into ----
  HANDLE process = nullptr;
  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));
  bool launched = false;
  DWORD pid = 0;
  DWORD delay_ms = 700;

  if (!opt.attach.empty()) {
    // ---- attach to a running process ----
    wchar_t* endp = nullptr;
    const unsigned long as_number = wcstoul(opt.attach.c_str(), &endp, 10);
    if (endp != nullptr && *endp == L'\0' && as_number != 0) {
      pid = as_number;
    } else {
      pid = FindProcessByName(opt.attach);
    }
    if (pid == 0) {
      Problem("no running process named '" + Narrow(opt.attach) + "'");
      return 1;
    }
    DWORD error = 0;
    process = OpenTarget(pid, &error);
    if (process == nullptr) {
      Problem("OpenProcess(" + Number(pid) + ") failed: " + ErrorText(error) +
              " (run as administrator?)");
      return 1;
    }
    Say("attached to pid " + Number(pid) + "\n");
    Say("note: attaching late can miss the initial pass over the game's data;\n");
    Say("      starting the game through this tool gives a clean result\n");
  } else {
    // ---- launch (or adopt an already running copy) ----
    if (!opt.delay.empty()) {
      const unsigned long v = wcstoul(opt.delay.c_str(), nullptr, 10);
      if (v > 0 && v < 600000) {
        delay_ms = v;
      } else {
        Say("--delay ignored: use 1..599999 milliseconds\n");
      }
    }

    const std::wstring exe_name = FileNameOf(exe);
    if (!opt.new_instance && !opt.suspend) {
      const DWORD running = FindProcessByName(exe_name);
      if (running != 0) {
        DWORD error = 0;
        process = OpenTarget(running, &error);
        if (process != nullptr) {
          pid = running;
          Say(Narrow(exe_name) + " is already running (pid " + Number(pid) +
              "); injecting into it instead of starting a second copy\n");
          Say("note: it has already loaded its data, so anything that has to be in\n");
          Say("      place early may be missed (use --new-instance to force a launch)\n");
        } else {
          Problem(Narrow(exe_name) + " is already running (pid " + Number(running) +
                  ") but could not be opened: " + ErrorText(error));
          Say("close it, or run this tool as administrator\n");
          return 1;
        }
      }
    }

    if (process == nullptr) {
      Say("mode : " + std::string(opt.suspend ? "create suspended, inject, then resume"
                                             : "start, then inject") +
          "\n");
      std::wstring cmdline = L"\"" + exe + L"\"" + opt.game_args;
      const DWORD flags = opt.suspend ? CREATE_SUSPENDED : 0;
      STARTUPINFOW si;
      ZeroMemory(&si, sizeof(si));
      si.cb = sizeof(si);

      if (!CreateProcessW(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, flags,
                          nullptr, DirectoryOf(exe).c_str(), &si, &pi)) {
        Problem("CreateProcess failed: " + ErrorText(GetLastError()));
        return 1;
      }
      launched = true;
      process = pi.hProcess;
      pid = pi.dwProcessId;
      Say("started " + Narrow(exe_name) + ", pid " + Number(pid) + "\n");

      if (!opt.suspend) {
        // Let the process finish bringing its own image up before touching it:
        // injecting while the loader is still working crashed the game.
        Say("waiting " + Number(delay_ms) + " ms before injecting\n");
        Sleep(delay_ms);
      }
    }
  }

  // ---- inject, one DLL at a time ----
  size_t ok = 0;
  bool aborted = false;
  std::vector<std::string> failures;
  for (size_t i = 0; i < injectable.size(); ++i) {
    const std::wstring& path = injectable[i]->path;
    Say("injecting [" + Number(i + 1) + "/" + Number(injectable.size()) + "] " +
        Narrow(injectable[i]->name) + " ... ");
    fflush(stdout);
    // An already loaded module is not loaded again, so its DllMain will not run
    // a second time -- worth saying out loud when it happens.
    const bool pre_loaded = ModuleLoadedIn(pid, path);
    const Injection result = InjectOne(process, pid, load_library_w, path);
    if (result.ok) {
      ++ok;
      Say("ok (module " + Hex(result.module) +
          (result.already_loaded || pre_loaded ? ", was already loaded in the target" : "") +
          ")\n");
      continue;
    }
    if (result.target_exited) {
      Say("FAILED: the process had already exited with code " + Number(result.exit_code) +
          "; nothing more can be injected\n");
      g_problems.push_back(Narrow(injectable[i]->name) +
                           ": the target process had already exited with code " +
                           Number(result.exit_code));
      if (launched) {
        Say("the program you started stopped before the DLLs were loaded -- check its\n");
        Say("own error output, or raise --delay so it is injected later\n");
      }
      aborted = true;
      break;
    }
    Say("FAILED: " + ErrorText(result.error) + "\n");
    failures.push_back(Narrow(injectable[i]->name) + ": " + ErrorText(result.error));
    g_problems.push_back(failures.back());
  }

  Say("\n" + Number(ok) + " of " + Number(injectable.size()) + " DLL(s) injected into pid " +
      Number(pid) + "\n");
  if (!failures.empty()) {
    Say("failed:\n");
    for (const std::string& f : failures) Say("  " + f + "\n");
  }

  if (opt.suspend && launched) {
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
      Problem("ResumeThread failed: " + ErrorText(GetLastError()));
    } else {
      Say("main thread resumed\n");
    }
  }

  if (opt.wait && process != nullptr) {
    Say("waiting for the process to exit...\n");
    if (WaitForSingleObject(process, INFINITE) == WAIT_FAILED) {
      Problem("cannot wait for pid " + Number(pid) + ": " + ErrorText(GetLastError()));
      aborted = true;
    } else {
      DWORD code = 0;
      GetExitCodeProcess(process, &code);
      Say("process exited with code " + Number(code) + "\n");
    }
  }

  if (launched) {
    if (pi.hThread != nullptr) CloseHandle(pi.hThread);
  }
  if (process != nullptr) CloseHandle(process);

  return (failures.empty() && !aborted) ? 0 : 1;
}

// MinGW links a console subsystem binary against main() unless -municode is
// used, so provide a narrow entry point that hands over the real wide command
// line. This keeps UTF-16 paths intact regardless of the active code page.
int main() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return 1;
  const int rc = wmain(argc, argv);
  LocalFree(argv);
  // A clean run still closes by itself; a failure -- or a DLL that was skipped
  // and therefore silently is not in the game -- waits to be read.
  if (rc != 0 || g_skipped_any) WaitForAck(rc);
  return rc;
}
