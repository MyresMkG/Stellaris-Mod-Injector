// Checks src/crtprobe.cpp against a real game executable.
//
// The probe looks for the variable the game's CRT uses to hand every thread its
// per-thread data, and the loader refuses to start a thread while that variable
// still says "not allocated yet" (see src/crtprobe.h). The shape of that code is
// a property of the MSVC CRT, not of the game, so this tool is what to run after
// the game (or its compiler) changes:
//
//   g++ -std=c++17 -O2 -o check_crt_probe.exe tools/check_crt_probe.cpp src/crtprobe.cpp
//   check_crt_probe.exe "D:\SteamLibrary\steamapps\common\Stellaris\stellaris.exe"
//
// The file is laid out the way a loaded image looks (headers first, every
// section at its RVA), which is exactly what the probe reads in a live process.
//
// Expected on Stellaris 4.5.1 (2026-09): found at rva 0x2805b50, not ready (the
// image still holds the -1 the CRT overwrites during its own start-up). A "not
// found" here means the loader falls back to the plain delay, which is safe but
// starts later; the pattern in crtprobe.cpp then needs a look.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/crtprobe.h"

namespace {

std::vector<BYTE> LoadAsImage(const std::wstring& path, bool* ok) {
  *ok = false;
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (f == nullptr) return {};
  std::vector<BYTE> file;
  BYTE chunk[1 << 16];
  size_t n = 0;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) file.insert(file.end(), chunk, chunk + n);
  fclose(f);
  if (file.size() < sizeof(IMAGE_NT_HEADERS)) return {};

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(file.data() + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return {};

  std::vector<BYTE> image(nt->OptionalHeader.SizeOfImage, 0);
  const size_t headers = nt->OptionalHeader.SizeOfHeaders;
  memcpy(image.data(), file.data(), headers < file.size() ? headers : file.size());
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    const size_t at = sec->VirtualAddress;
    const size_t size = sec->SizeOfRawData;
    if (size == 0 || sec->PointerToRawData == 0) continue;
    if (at + size > image.size() || sec->PointerToRawData + size > file.size()) continue;
    memcpy(image.data() + at, file.data() + sec->PointerToRawData, size);
  }
  *ok = true;
  return image;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: check_crt_probe.exe <game.exe>\n");
    return 2;
  }
  const int wide_len = MultiByteToWideChar(CP_ACP, 0, argv[1], -1, nullptr, 0);
  std::wstring path(wide_len > 0 ? wide_len - 1 : 0, L'\0');
  if (wide_len > 0) MultiByteToWideChar(CP_ACP, 0, argv[1], -1, path.data(), wide_len);

  bool ok = false;
  std::vector<BYTE> image = LoadAsImage(path, &ok);
  if (!ok) {
    printf("cannot read %s\n", argv[1]);
    return 2;
  }

  loader::CrtProbe probe;
  probe.Locate(reinterpret_cast<HMODULE>(image.data()));
  if (probe.Found()) {
    printf("found at rva 0x%lx, ready=%d\n", probe.Rva(), probe.Ready() ? 1 : 0);
  } else {
    printf("not found -- the loader falls back to delay_ms only\n");
  }
  return probe.Found() ? 0 : 1;
}
