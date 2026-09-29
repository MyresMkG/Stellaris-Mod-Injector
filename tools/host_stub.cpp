// Stand-in for stellaris.exe used by tools/run_tests.py.
//
// It imports the same functions the game does from all five proxy names, so a
// forwarder that cannot be resolved fails loudly at load time instead of
// silently. A few of them are called for real (version size, winmm timer, D3D9
// factory), the module paths are printed so the test can see which ones came
// from the test folder, and then it stays alive for a few seconds so the loader
// thread inside the proxy has time to load injected_mods.

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <winver.h>

#include <cstdio>
#include <cstring>

namespace {

const char* kNames[] = {"dxgi.dll", "d3d11.dll", "d3d9.dll", "version.dll", "winmm.dll"};

// Spelled out instead of __uuidof so the test does not depend on compiler
// extensions; a wrong IID would only turn the result into E_NOINTERFACE.
const GUID kIID_IDXGIFactory1 = {0x770aae78, 0xf26f, 0x4dba,
                                 {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};

DWORD WINAPI IdleThread(void*) { return 0; }

}  // namespace

int main(int argc, char** argv) {
  // --no-pump: like the launcher's start, where the proxy is walked on a thread
  // of its own and the game's own message loop only starts much later. Nothing
  // pumps here; the only wake-up left for the proxy is a thread appearing.
  const bool no_pump = argc > 1 && strcmp(argv[1], "--no-pump") == 0;

  char exe[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  printf("host    : %s%s\n", exe, no_pump ? " (no message loop)" : "");

  for (const char* name : kNames) {
    HMODULE module = GetModuleHandleA(name);
    char path[MAX_PATH] = {0};
    if (module == nullptr) {
      printf("%-12s: NOT LOADED\n", name);
      continue;
    }
    GetModuleFileNameA(module, path, MAX_PATH);
    printf("%-12s: %s\n", name, path);
  }

  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeA("C:/Windows/System32/ntdll.dll", &handle);
  printf("version size : %lu\n", static_cast<unsigned long>(size));

  const DWORD tick = timeGetTime();
  printf("timeGetTime  : %lu (wave devices %u)\n", static_cast<unsigned long>(tick),
         static_cast<unsigned>(waveOutGetNumDevs()));

  IDirect3D9* d3d9 = Direct3DCreate9(D3D_SDK_VERSION);
  printf("d3d9 factory : %p\n", reinterpret_cast<void*>(d3d9));
  if (d3d9 != nullptr) d3d9->Release();

  IDXGIFactory1* factory = nullptr;
  const HRESULT dxgi_hr = CreateDXGIFactory1(kIID_IDXGIFactory1,
                                             reinterpret_cast<void**>(&factory));
  printf("dxgi factory : hr=0x%08lx ptr=%p\n", static_cast<unsigned long>(dxgi_hr),
         reinterpret_cast<void*>(factory));
  if (factory != nullptr) factory->Release();

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
  const HRESULT d3d11_hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                             nullptr, 0, D3D11_SDK_VERSION, &device, &level,
                                             &context);
  printf("d3d11 device : hr=0x%08lx level=0x%04x\n", static_cast<unsigned long>(d3d11_hr),
         static_cast<unsigned>(level));
  if (context != nullptr) context->Release();
  if (device != nullptr) device->Release();

  // The proxy loader 1.1 does not start its thread at attach any more: it waits
  // for a thread attach or for the host's first message loop, so the stub does
  // what a game does -- it starts a worker and then pumps messages for a few
  // seconds -- and stays alive long enough for the mods to be loaded.
  if (no_pump) {
    // The launcher's shape: nobody pumps, and the first thread after start-up
    // arrives late. That thread attach is the only wake-up the proxy can use.
    Sleep(1200);
    HANDLE late = CreateThread(nullptr, 0, IdleThread, nullptr, 0, nullptr);
    if (late != nullptr) {
      WaitForSingleObject(late, 2000);
      CloseHandle(late);
    }
    Sleep(2500);
    printf("host done\n");
    fflush(stdout);
    return (size != 0 && tick != 0) ? 0 : 1;
  }

  HANDLE worker = CreateThread(nullptr, 0, IdleThread, nullptr, 0, nullptr);
  if (worker != nullptr) {
    WaitForSingleObject(worker, 2000);
    CloseHandle(worker);
  }
  MSG msg;
  const DWORD until = GetTickCount() + 4000;
  while (GetTickCount() < until) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(1);
  }

  printf("host done\n");
  fflush(stdout);
  return (size != 0 && tick != 0) ? 0 : 1;
}

