// Reproduces the start-up that made the proxies crash the game, without a
// launcher: the game is created suspended and the DLL loader is walked on a
// thread of its own -- which is what Steam's overlay does inside the launcher
#include <cstdlib>
// (its hooked CreateProcess) when the Paradox launcher starts the game.
//
// While the game's main thread is suspended its CRT start-up cannot run, so a
// thread started from a proxy's DllMain during that walk is hit by the game's
// own TLS callback, which calls abort() and kills the process with BEX64.
// Proxies from 1.1 arm a timer and a thread-attach watch instead and wait for
// the CRT, so this harness has to be the way to see the difference:
//
//   suspended_start_check.exe <game.exe> [wait_seconds]
//
// Behaviour to expect, game folder contents being the only difference:
//   proxies 1.0   the process is gone during the walk -- before the main thread
//                 is ever resumed -- with an access violation, WER may not even
//                 get a report out of a half-initialized process
//   proxies 1.1   the process is still alive at the end of the walk: no thread
//                 of ours was created while the game's CRT was not up
//
// What the resume afterwards is *not* good for: an unmodified game dies with an
// access violation as well, because a thread other than the initial one did the
// loader walk and then left. That was measured as a control (no proxy in the
// game folder at all, same harness) and is a property of this harness, not of
// the proxies -- the injection the launcher triggers hijacks the main thread
// itself instead of adding a thread, which is why the game does start there.
// The result that matters here is therefore the line "process still alive after
// N s", not what the resume produces.
//
// It needs no injection of its own beyond one no-op thread: LoadLibrary is not
// called, the thread only makes the loader run, then spins.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::wstring FromArg(const char* arg) {
  const int n = MultiByteToWideChar(CP_ACP, 0, arg, -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_ACP, 0, arg, -1, out.data(), n);
  return out;
}

bool WriteStub(HANDLE process, BYTE** at) {
  // "jmp $" -- the thread's own start-up (the loader walk we are after) runs
  // before it, and the thread then spins so that it never runs the *detach*
  // side of the loader while the process is only half initialized.
  void* memory = VirtualAllocEx(process, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
  if (memory == nullptr) return false;
  const BYTE stub[2] = {0xEB, 0xFE};
  SIZE_T written = 0;
  if (!WriteProcessMemory(process, memory, stub, sizeof(stub), &written) ||
      written != sizeof(stub)) {
    return false;
  }
  *at = static_cast<BYTE*>(memory);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: suspended_start_check.exe <game.exe> [wait_seconds]\n");
    return 2;
  }
  const std::wstring exe = FromArg(argv[1]);
  const int wait_s = argc > 2 ? atoi(argv[2]) : 6;

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  // CREATE_SUSPENDED: the game's main thread is held before its CRT start-up,
  // exactly the state the launcher leaves it in while the overlay is injected.
  if (!CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                      nullptr, nullptr, &si, &pi)) {
    printf("cannot start %ls (%lu)\n", exe.c_str(), GetLastError());
    return 2;
  }
  printf("started pid %lu (main thread suspended)\n", pi.dwProcessId);

  BYTE* stub = nullptr;
  HANDLE walker = nullptr;
  if (WriteStub(pi.hProcess, &stub)) {
    walker = CreateRemoteThread(pi.hProcess, nullptr, 0,
                                reinterpret_cast<LPTHREAD_START_ROUTINE>(stub), nullptr, 0,
                                nullptr);
  }
  printf("loader walk on a thread of its own: %s\n", walker ? "started" : "failed");
  if (walker != nullptr) {
    WaitForSingleObject(walker, (wait_s + 4) * 1000);
    CloseHandle(walker);
  }

  const DWORD state = WaitForSingleObject(pi.hProcess, wait_s * 1000);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  if (state == WAIT_TIMEOUT) {
    printf("process still alive after %d s -- no early-thread abort\n", wait_s);
    printf("resuming the main thread to let the game start\n");
    ResumeThread(pi.hThread);
    Sleep(8000);
    GetExitCodeProcess(pi.hProcess, &code);
    if (GetExitCodeProcess(pi.hProcess, &code) && code == STILL_ACTIVE) {
      printf("game is running (pid %lu)\n", pi.dwProcessId);
      printf("leaving it running; the loader log says what the proxy did\n");
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      return 0;
    }
  }
  printf("process ended, exit code 0x%08lx", code);
  if (code == 0xC0000409L) printf("  <- STATUS_STACK_BUFFER_OVERRUN (__fastfail)");
  printf("\n");
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 1;
}
