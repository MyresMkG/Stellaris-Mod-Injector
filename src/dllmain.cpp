// Entry point of the five proxy DLLs.
//
// Every build is linked against def/<name>.def, which forwards each export to
// the real system DLL in System32 -- as far as the rest of the game process is
// concerned this file *is* that DLL, down to the ordinals. DllMain adds one
// thing on top: it loads every DLL from <game root>\injected_mods into the
// process, the same set stellaris_mod_injector.exe would have injected from the
// outside, with the same checks, the same file-name order and the same log
// shape.
//
// Why the work happens on a thread: DllMain runs under the loader lock, and this
// DLL is loaded *while* the game's own imports are still being resolved, so
// calling LoadLibrary here would deadlock the game's start-up. The thread waits
// until the process image is up -- the phase the injector waits out from the
// outside with its 700 ms delay -- and only then loads the mods.
//
// Why that thread is *not* created here any more: a thread created while the
// game is still starting up can be killed by the game's own CRT. Stellaris 4.5.1
// links a CRT whose TLS callback gives every new thread its per-thread data, and
// that callback aborts the process (__fastfail(7), i.e. 0xC0000409 / BEX64, no
// window, no log) when the CRT's process-wide start-up has not run yet. That is
// not a corner case: when the game is started through the launcher, the
// process is created suspended and the loader walks the proxies on an injected
// thread, so the game's main thread cannot run its start-up until after that
// thread is done -- while a thread of ours, waiting for the same loader lock,
// runs its TLS callback first and dies. The old build survived that only when
// the main thread happened to win the same race (a direct start).
//
// So the thread is now created only once the game's CRT is up (src/crtprobe.h
// reads the flag that says so), and only from a wake-up that can happen that
// late: the loader's DLL_THREAD_ATTACH for a new thread, or a callback timer on
// the loading thread once it reaches a message loop. Until then nothing of ours
// runs on any thread; the delay_ms knob keeps its meaning because the worker
// still waits it out from the process start before it loads anything.

#include <windows.h>

#include <string>
#include <vector>

#include "config.h"
#include "crtprobe.h"
#include "log.h"
#include "mods.h"
#include "util.h"

namespace {

HMODULE g_self = nullptr;
DWORD g_attach_tick = 0;
loader::CrtProbe g_crt;

// With no probe in the host (another program, a different CRT) nothing can be
// checked, so a wake-up is only taken once this much of the process's life has
// passed -- the same 700 ms the loader used before, long after any host has
// walked its imports. The mods still wait out the configured delay_ms.
const unsigned long kNoProbeSafeMs = 700;

// Nothing woke us up although a probe says the CRT is still not up. Rather than
// leaving the process without mods, the worker is then run on the thread that
// is already initialized instead of starting a new one -- see StartInline.
const unsigned long kGiveUpMs = 30000;

// How the worker came to be started. Reported by the worker itself, because all
// of this happens before the log file is open.
enum Start : unsigned {
  kStartAtAttach = 0,   // the game's CRT was already up when the proxy loaded
  kStartThreadAttach,   // a thread attached after the CRT was up
  kStartMessageLoop,    // the timer fired on the loading thread
  kStartGiveUp,         // nothing woke us up in time; run on the loading thread
  kStartNoWakeUp,       // no user32; start right away, as the old build did
};

Start g_start = kStartAtAttach;
unsigned long g_start_ms = 0;
volatile LONG g_armed = 0;    // the start was deferred
volatile LONG g_started = 0;  // the worker was started (or is running inline)

const wchar_t* kGameExeName = L"stellaris.exe";
const wchar_t* kModsDirName = L"injected_mods";
const wchar_t* kLogName = L"stellaris_mod_loader.log";
const wchar_t* kIniName = L"stellaris_mod_loader.ini";
const wchar_t* kProbeMarkerName = L"stellaris_mod_loader_probe.txt";
const wchar_t* kProbeFlagName = L"diplo_action_hook_probe_only.txt";
const char* kVersion = "1.1";

// --------------------------------------------------------- deferred starting

// The one question that decides whether a thread may be created at all: is the
// game's CRT past its own start-up? See src/crtprobe.h for why that matters and
// what the probe reads. Without a probe, time is the only proxy available.
bool SafeToStart() {
  if (g_crt.Found()) return g_crt.Ready();
  return loader::ProcessUptimeMs() >= kNoProbeSafeMs;
}

// True for exactly one caller: the one that may bring up the worker.
bool ClaimStart() { return InterlockedCompareExchange(&g_started, 1, 0) == 0; }

void ReportStart(Start how) {
  g_start = how;
  g_start_ms = loader::ProcessUptimeMs();
}

DWORD WINAPI Worker(void* how);

// Runs the worker on the calling thread. Used when the CRT stays unready far
// too long for a sane host: an existing thread is already initialized, so this
// cannot produce the early-thread abort, and the mods are better off loading a
// little late than not at all.
void StartInline(Start how) {
  ReportStart(how);
  Worker(reinterpret_cast<void*>(static_cast<uintptr_t>(how)));
}

void StartThread(Start how) {
  ReportStart(how);
  HANDLE thread = CreateThread(nullptr, 0, Worker,
                               reinterpret_cast<void*>(static_cast<uintptr_t>(how)), 0,
                               nullptr);
  if (thread != nullptr) {
    CloseHandle(thread);
  } else {
    // No thread left to create: doing the work here is still better than not
    // loading the mods at all, and the "done, ... after attach" line will show
    // that it happened on someone else's thread.
    StartInline(how);
  }
}

// A callback timer on the loading thread: it fires once that thread runs a
// message loop, which the game only does after its own start-up. In the
// launcher case the loading thread is the injected one and dies with its timer,
// which is what the DLL_THREAD_ATTACH path is for.
UINT_PTR g_timer = 0;
UINT_PTR(WINAPI* g_set_timer)(HWND, UINT_PTR, UINT, TIMERPROC) = nullptr;
BOOL(WINAPI* g_kill_timer)(HWND, UINT_PTR) = nullptr;

VOID CALLBACK OnTimer(HWND, UINT, UINT_PTR, DWORD) {
  if (g_started != 0) {
    if (g_timer != 0 && g_kill_timer != nullptr) g_kill_timer(nullptr, g_timer);
    return;
  }
  if (SafeToStart()) {
    if (ClaimStart()) {
      if (g_timer != 0 && g_kill_timer != nullptr) g_kill_timer(nullptr, g_timer);
      StartThread(kStartMessageLoop);
    }
    return;
  }
  if (loader::ProcessUptimeMs() >= kGiveUpMs) {
    if (ClaimStart()) StartInline(kStartGiveUp);
  }
}

// Resolves user32 on demand: importing it would pull it into every host that
// loads one of the five names, including console tools started from the game
// folder, and the timer is only useful in hosts that have a message loop.
bool ArmTimer(unsigned long delay_ms) {
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32 == nullptr) user32 = LoadLibraryW(L"user32.dll");
  if (user32 == nullptr) return false;
  // Through void* so that no cast-between-function-types warning is produced:
  // the two exports are taken by name from the module that is known to have
  // them, and nothing else in this DLL ever calls them.
  g_set_timer = reinterpret_cast<UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC)>(
      reinterpret_cast<void*>(GetProcAddress(user32, "SetTimer")));
  g_kill_timer = reinterpret_cast<BOOL(WINAPI*)(HWND, UINT_PTR)>(
      reinterpret_cast<void*>(GetProcAddress(user32, "KillTimer")));
  if (g_set_timer == nullptr) return false;
  g_timer = g_set_timer(nullptr, 0, static_cast<UINT>(delay_ms), OnTimer);
  return g_timer != 0;
}

// Everything DllMain can decide: start now when the host is demonstrably past
// its start-up, otherwise arm every wake-up that can arrive later.
void ArmDeferredStart() {
  if (SafeToStart()) {
    if (ClaimStart()) StartThread(kStartAtAttach);
    return;
  }
  InterlockedExchange(&g_armed, 1);
  if (!ArmTimer(kNoProbeSafeMs)) {
    // No message loop to be had (no user32 in this host). Nothing can wake us
    // up later, so this is the old behaviour: one thread now, which then logs
    // why it is there.
    if (ClaimStart()) StartThread(kStartNoWakeUp);
  }
}

// ------------------------------------------------------------------- worker

DWORD WINAPI Worker(void* how_param) {
  const std::wstring self_path = loader::ModulePath(g_self);
  const std::wstring self_name = loader::FileNameOf(self_path);
  const std::wstring host_path = loader::ModulePath(nullptr);
  const std::wstring game_root = loader::DirectoryOf(host_path);
  const std::wstring mods_dir = game_root + L"\\" + kModsDirName;
  const bool mods_present = loader::DirExists(mods_dir);

  // The loader's own log lives in the game root, next to the exe the player (and
  // the launcher) look at, and outside a folder that may not be writable. The
  // mods keep writing their own logs inside injected_mods.
  loader::LogInit(game_root + L"\\" + kLogName);
  loader::Log("stellaris_mod_loader %s -- proxy '%s', pid %lu", kVersion,
              loader::Narrow(self_name).c_str(),
              static_cast<unsigned long>(GetCurrentProcessId()));
  loader::Log("game  : %s", loader::Narrow(host_path).c_str());
  loader::Log("mods  : %s%s", loader::Narrow(mods_dir).c_str(),
              mods_present ? "" : "  (does not exist)");

  // Say what was waited for: the start-up of the game's CRT (when the probe
  // could find it) or the plain 700 ms fallback, and which wake-up brought the
  // worker here.
  if (g_crt.Found()) {
    // The flag lives in the host image (the game's own exe), not in this proxy.
    loader::Log("crt   : per-thread data flag at %s+0x%lx, %s",
                loader::Narrow(loader::FileNameOf(host_path)).c_str(),
                static_cast<unsigned long>(g_crt.Rva()),
                g_crt.Ready() ? "ready" : "never became ready");
  } else {
    loader::Log("crt   : no per-thread data flag found in %s; delay_ms only",
                loader::Narrow(loader::FileNameOf(host_path)).c_str());
  }
  const Start how = static_cast<Start>(reinterpret_cast<uintptr_t>(how_param));
  switch (how) {
    case kStartAtAttach:
      loader::Log("start : the game's CRT was already up at attach");
      break;
    case kStartThreadAttach:
      loader::Log("start : a thread attached after the CRT was up (%lu ms into the process)",
                  g_start_ms);
      break;
    case kStartMessageLoop:
      loader::Log("start : the message loop started (%lu ms into the process)", g_start_ms);
      break;
    case kStartGiveUp:
      loader::Log("start : the CRT stayed unready for %lu ms; loading on the loading thread",
                  g_start_ms);
      break;
    case kStartNoWakeUp:
      loader::Log("start : no user32 for a deferred start; thread started at attach");
      break;
  }

  // The same file name can be picked up by any other program started from this
  // folder. Only the game is supposed to get the mods.
  if (!loader::SameName(loader::FileNameOf(host_path), kGameExeName)) {
    loader::Log("host is not %s; nothing to do", loader::Narrow(kGameExeName).c_str());
    return 0;
  }

  const loader::Settings settings = loader::LoadSettings(mods_dir + L"\\" + kIniName);

  // Same shape as the injector: start the game, wait out its own start-up, then
  // hand it the DLLs. 700 ms is the injector's default and is what was verified
  // against Stellaris 4.5; loading before the image is up is what crashed it.
  //
  // No extra "is the loader idle" probe: LoadLibraryW blocks on the loader lock
  // by itself, so a process that is still bringing its image up simply delays
  // the load instead of being disturbed by it. (Trying to ask ntdll -- probing
  // LdrLockLoaderLock with TRY_ONLY from this thread -- was measured to wedge
  // Stellaris 4.5 in the local tests, so it is deliberately not done here.)
  const unsigned long uptime = loader::ProcessUptimeMs();
  if (uptime < settings.delay_ms) {
    loader::Log("waiting %lu ms before loading (delay_ms=%lu, process is %lu ms old)",
                static_cast<unsigned long>(settings.delay_ms - uptime),
                static_cast<unsigned long>(settings.delay_ms),
                static_cast<unsigned long>(uptime));
    Sleep(settings.delay_ms - uptime);
  }

  // A player may install more than one of the five names. The first proxy to get
  // here loads; the others walk in behind it and find the modules already in
  // place, which reads as "was already loaded in the process".
  wchar_t mutex_name[64];
  swprintf(mutex_name, 64, L"Local\\stellaris_mod_loader_%lu",
           static_cast<unsigned long>(GetCurrentProcessId()));
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name);
  bool holding = false;
  if (mutex != nullptr) {
    holding = WaitForSingleObject(mutex, 60000) == WAIT_OBJECT_0;
    if (!holding) loader::Log("another proxy DLL holds the loader lock; going ahead anyway");
  }

  const bool probe = mods_present && loader::FileExists(mods_dir + L"\\" + kProbeMarkerName);
  if (probe) {
    loader::Log("probe mode: creating %s next to every DLL (nothing will be hooked)",
                loader::Narrow(kProbeFlagName).c_str());
  }

  std::vector<loader::Mod> candidates;
  if (mods_present) {
    for (const std::wstring& path : loader::ListDlls(mods_dir)) {
      loader::Mod mod = loader::Inspect(path);
      if (mod.loadable && loader::SameName(mod.name, self_name)) {
        mod.loadable = false;
        mod.why = "this is the loader itself";
      }
      // The flag has to exist before the DLL's DllMain runs, so it is written
      // here. A folder that cannot be written to is worth saying out loud: the
      // mod then hooks for real instead of only reporting addresses.
      if (mod.loadable && probe && !loader::CreateProbeFlag(path)) {
        loader::Log("  warning: could not create %s next to %s",
                    loader::Narrow(kProbeFlagName).c_str(), loader::Narrow(mod.name).c_str());
      }
      candidates.push_back(mod);
    }
  }

  size_t loadable = 0;
  for (const loader::Mod& mod : candidates) {
    if (mod.loadable) ++loadable;
  }
  loader::Log("%lu candidate(s), %lu to load", static_cast<unsigned long>(candidates.size()),
              static_cast<unsigned long>(loadable));

  size_t index = 0;
  size_t ok = 0;
  std::vector<std::string> failures;
  for (const loader::Mod& mod : candidates) {
    if (!mod.loadable) {
      loader::Log("  [skip] %s: %s", loader::Narrow(mod.name).c_str(), mod.why.c_str());
      continue;
    }
    ++index;
    // An already loaded module is not loaded again, so its DllMain does not run
    // a second time -- worth saying out loud when it happens.
    if (loader::ModuleLoaded(mod.path)) {
      ++ok;
      loader::Log("loading [%lu/%lu] %s ... ok (was already loaded in the process)",
                  static_cast<unsigned long>(index), static_cast<unsigned long>(loadable),
                  loader::Narrow(mod.name).c_str());
      continue;
    }
    std::string why;
    const HMODULE module = loader::Load(mod.path, &why);
    if (module != nullptr) {
      ++ok;
      loader::Log("loading [%lu/%lu] %s ... ok (module %p)",
                  static_cast<unsigned long>(index), static_cast<unsigned long>(loadable),
                  loader::Narrow(mod.name).c_str(), reinterpret_cast<void*>(module));
      continue;
    }
    loader::Log("loading [%lu/%lu] %s ... FAILED: %s",
                static_cast<unsigned long>(index), static_cast<unsigned long>(loadable),
                loader::Narrow(mod.name).c_str(), why.c_str());
    failures.push_back(loader::Narrow(mod.name) + ": " + why);
  }

  loader::Log("%lu of %lu DLL(s) loaded into pid %lu", static_cast<unsigned long>(ok),
              static_cast<unsigned long>(loadable),
              static_cast<unsigned long>(GetCurrentProcessId()));
  for (const std::string& failure : failures) loader::Log("failed: %s", failure.c_str());
  if (!mods_present) {
    loader::Log("create '%s' next to stellaris.exe and put the DLLs to load in it",
                loader::Narrow(kModsDirName).c_str());
  } else if (loadable == 0) {
    loader::Log("nothing to load");
  }
  loader::Log("done, %lu ms after attach",
              static_cast<unsigned long>(GetTickCount() - g_attach_tick));

  if (mutex != nullptr) {
    if (holding) ReleaseMutex(mutex);
    CloseHandle(mutex);
  }
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = instance;
    g_attach_tick = GetTickCount();
    // DisableThreadLibraryCalls is deliberately *not* called: the wake-up that
    // works even when the loading thread is an injected one -- and therefore
    // cannot host a useful timer -- is the loader's DLL_THREAD_ATTACH for the
    // first thread the game starts after its CRT is up.
    g_crt.Locate(GetModuleHandleW(nullptr));
    ArmDeferredStart();
  } else if (reason == DLL_THREAD_ATTACH) {
    // Reaching this notification at all means the thread is past its own TLS
    // callbacks, which the game's CRT would have aborted had its start-up not
    // run; SafeToStart() double-checks that instead of trusting the order.
    if (g_armed != 0 && g_started == 0 && SafeToStart() && ClaimStart()) {
      StartThread(kStartThreadAttach);
    }
  }
  return TRUE;
}
