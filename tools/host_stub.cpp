// Stand-in for stellaris.exe used by tools/run_tests.py.
//
// It imports the same functions the game does from every proxy name, so a
// forwarder that cannot be resolved fails loudly at load time instead of
// silently. A few of them are called for real (version size, winmm timer, one
// real HLSL compile), the module paths are printed so the test can see which
// ones came from the test folder, and then it stays alive for a few seconds so
// the loader thread inside the proxy has time to load injected_mods.

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <winver.h>

#include <cstdio>
#include <cstring>

namespace {

const char* kNames[] = {"dxgi.dll", "d3d11.dll", "version.dll", "winmm.dll",
                        "d3dcompiler_47.dll"};

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

  // The proxy loader 1.1 does not start its thread at attach any more: it waits
  // for a thread attach or for the host's first message loop, so the stub does
  // what a game does -- it starts a worker early and pumps messages later --
  // and stays alive long enough for the mods to be loaded. The worker appears
  // before the D3D calls below on purpose: those calls take about as long as
  // the proxy's 700 ms no-probe gate, and a thread showing up after that gate
  // would start the loader thread itself, masking the message-loop wake-up this
  // shape of host is here to check (the --no-pump shape has its own late one).
  HANDLE worker = nullptr;
  if (!no_pump) worker = CreateThread(nullptr, 0, IdleThread, nullptr, 0, nullptr);

  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeA("C:/Windows/System32/ntdll.dll", &handle);
  printf("version size : %lu\n", static_cast<unsigned long>(size));

  const DWORD tick = timeGetTime();
  printf("timeGetTime  : %lu (wave devices %u)\n", static_cast<unsigned long>(tick),
         static_cast<unsigned>(waveOutGetNumDevs()));

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

  // The game compiles its shaders with D3DCompile, so a real compile says the
  // forwarder not only resolves but works.
  ID3DBlob* bytecode = nullptr;
  ID3DBlob* compile_errors = nullptr;
  const char* hlsl = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
  const HRESULT compile_hr = D3DCompile(hlsl, strlen(hlsl), "host_stub.hlsl", nullptr,
                                        nullptr, "main", "ps_4_0", 0, 0, &bytecode,
                                        &compile_errors);
  printf("d3dcompiler  : hr=0x%08lx bytes=%lu\n", static_cast<unsigned long>(compile_hr),
         static_cast<unsigned long>(bytecode != nullptr ? bytecode->GetBufferSize() : 0));
  const bool compiled = compile_hr == S_OK && bytecode != nullptr;
  if (bytecode != nullptr) bytecode->Release();
  if (compile_errors != nullptr) compile_errors->Release();

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
    return (size != 0 && tick != 0 && compiled) ? 0 : 1;
  }

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
  return (size != 0 && tick != 0 && compiled) ? 0 : 1;
}

