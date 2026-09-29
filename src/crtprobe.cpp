#include "crtprobe.h"

#include <cstring>
#include <vector>

namespace loader {
namespace {

struct Func {
  unsigned long begin;
  unsigned long end;
};

const IMAGE_SECTION_HEADER* FindSection(BYTE* image, unsigned long rva, unsigned long size,
                                        bool* writable) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    const unsigned long vsize = sec->Misc.VirtualSize ? sec->Misc.VirtualSize
                                                      : sec->SizeOfRawData;
    if (rva >= sec->VirtualAddress && rva + size <= sec->VirtualAddress + vsize) {
      if (writable != nullptr) {
        *writable = (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
      }
      return sec;
    }
  }
  return nullptr;
}

// Function table from .pdata: the only reliable way to know where a function
// starts and ends in an optimized image, and both are needed below.
std::vector<Func> ReadFunctions(BYTE* image) {
  std::vector<Func> out;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (memcmp(sec->Name, ".pdata", 6) != 0) continue;
    const unsigned count = sec->Misc.VirtualSize / sizeof(RUNTIME_FUNCTION);
    const auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(image + sec->VirtualAddress);
    for (unsigned j = 0; j < count; ++j) {
      if (rf[j].BeginAddress != 0) {
        out.push_back(Func{rf[j].BeginAddress, rf[j].EndAddress});
      }
    }
  }
  return out;  // .pdata is sorted by BeginAddress already
}

unsigned long FunctionStart(const std::vector<Func>& funcs, unsigned long rva) {
  size_t lo = 0;
  size_t hi = funcs.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (funcs[mid].begin <= rva) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return 0;
  const Func& f = funcs[lo - 1];
  return (rva < f.end) ? f.begin : 0;
}

const Func* FunctionOf(const std::vector<Func>& funcs, unsigned long rva) {
  const unsigned long begin = FunctionStart(funcs, rva);
  if (begin == 0) return nullptr;
  for (const Func& f : funcs) {
    if (f.begin == begin) return &f;
  }
  return nullptr;
}

// "cmp dword ptr [rip+disp], -1" -- the "has the CRT allocated the index yet"
// test. Returns the RVA of the compared dword.
bool DecodeIndexTest(const BYTE* code, unsigned long rva, unsigned long* target) {
  if (code[0] == 0x83 && code[1] == 0x3D && code[6] == 0xFF) {
    *target = rva + 7 + *reinterpret_cast<const int*>(code + 2);
    return true;
  }
  if (code[0] == 0x81 && code[1] == 0x3D &&
      *reinterpret_cast<const unsigned*>(code + 3) == 0xFFFFFFFFu) {
    *target = rva + 10 + *reinterpret_cast<const int*>(code + 2);
    return true;
  }
  return false;
}

}  // namespace

// How the helper is found, and why it is worth this much code:
//
//   the game's CRT calls abort() when this helper hands it NULL, and abort() in
//   this CRT is its own little function with an inlined __fastfail
//   (mov ecx, 7 / int 29h). Walking that chain backwards gives exactly one
//   candidate: "a function whose NULL return makes a wrapper call abort()".
//   What is left to verify is that the candidate really is the per-thread data
//   helper and not, say, a malloc wrapper: it has to begin with the
//   "cmp dword [global], -1" test, followed by "no index yet -> return NULL",
//   and read that same global again when it does run. Only if exactly one
//   candidate survives all of that is the global used as the gate.
//
// Nothing is ever written to the host image: the probe only reads.
void CrtProbe::Locate(HMODULE main_module) {
  image_ = reinterpret_cast<BYTE*>(main_module);
  if (image_ == nullptr) return;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image_ + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return;
  if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return;

  const std::vector<Func> funcs = ReadFunctions(image_);
  if (funcs.empty()) return;

  const IMAGE_SECTION_HEADER* text = nullptr;
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (memcmp(sec->Name, ".text", 5) == 0) text = sec;
  }
  if (text == nullptr) return;
  const unsigned long begin = text->VirtualAddress;
  const unsigned long end = begin + (text->Misc.VirtualSize ? text->Misc.VirtualSize
                                                            : text->SizeOfRawData);

  // 1. abort(): "mov ecx, 7; int 29h" (__fastfail(FAST_FAIL_FATAL_APP_EXIT)),
  //    inlined in the CRT function that ends the process.
  static const BYTE kFastFail[] = {0xB9, 0x07, 0x00, 0x00, 0x00, 0xCD, 0x29};
  std::vector<unsigned long> aborts;
  for (unsigned long rva = begin; rva + sizeof(kFastFail) < end; ++rva) {
    if (memcmp(image_ + rva, kFastFail, sizeof(kFastFail)) != 0) continue;
    const unsigned long entry = FunctionStart(funcs, rva);
    if (entry == 0) continue;
    bool known = false;
    for (unsigned long a : aborts) known = known || a == entry;
    if (!known) aborts.push_back(entry);
  }
  if (aborts.empty()) return;

  // 2. + 3. Every call to abort() that is guarded by a NULL test, and the call
  //    right before that guard: the helper that returned NULL.
  std::vector<unsigned long> getters;
  for (unsigned long rva = begin; rva + 5 < end; ++rva) {
    if (image_[rva] != 0xE8) continue;
    const unsigned long target = rva + 5 + *reinterpret_cast<const int*>(image_ + rva + 1);
    bool to_abort = false;
    for (unsigned long a : aborts) to_abort = to_abort || a == target;
    if (!to_abort) continue;

    bool guarded = false;
    for (unsigned long back = 1; back <= 0x20 && rva >= back; ++back) {
      const BYTE* p = image_ + rva - back;
      if (p[0] == 0x48 && p[1] == 0x85 && p[2] == 0xC0) guarded = true;
      if (guarded) break;
    }
    if (!guarded) continue;

    for (unsigned long back = 1; back <= 0x20 && rva >= back; ++back) {
      const BYTE* p = image_ + rva - back;
      if (p[0] != 0xE8) continue;
      const unsigned long call = rva - back + 5 + *reinterpret_cast<const int*>(p + 1);
      const unsigned long entry = FunctionStart(funcs, call);
      if (entry == 0 || entry != call) continue;  // must be the function itself
      bool to_abort2 = false;
      for (unsigned long a : aborts) to_abort2 = to_abort2 || a == entry;
      if (to_abort2) continue;
      bool known = false;
      for (unsigned long g : getters) known = known || g == entry;
      if (!known) getters.push_back(entry);
      break;
    }
  }
  if (getters.empty()) return;

  // 4. Verify the candidates and keep the global each one tests.
  std::vector<unsigned long> found;
  for (unsigned long entry : getters) {
    const Func* f = FunctionOf(funcs, entry);
    if (f == nullptr || f->end <= f->begin + 16) continue;
    // The index test sits after the register saves, so it is looked for in the
    // prologue rather than at the first byte.
    unsigned long target = 0;
    unsigned long test_at = 0;
    for (unsigned long k = 0; k + 10 < 0x40 && entry + k + 10 < f->end; ++k) {
      if (DecodeIndexTest(image_ + entry + k, entry + k, &target)) {
        test_at = entry + k;
        break;
      }
    }
    if (test_at == 0) continue;
    const unsigned long test_len = (image_[test_at] == 0x83) ? 7 : 10;
    // "not allocated yet -> return NULL": a branch right after the test and an
    // unconditional jump into the exit path (an optional "xor eax, eax" sets the
    // NULL up first).
    const BYTE* after = image_ + test_at + test_len;
    if (after[0] != 0x75 && after[0] != 0x0F) continue;
    const unsigned long after_len = (after[0] == 0x0F) ? 6 : 2;
    const BYTE* next = image_ + test_at + test_len + after_len;
    if (next[0] != 0xEB && next[0] != 0xE9) {
      // The NULL that is returned may be set up first: xor eax, eax (31 C0 or
      // 33 C0) or mov eax, 0.
      unsigned long set_len = 0;
      if ((next[0] == 0x31 || next[0] == 0x33) && next[1] == 0xC0) {
        set_len = 2;
      } else if (next[0] == 0xB8 && next[1] == 0 && next[2] == 0 && next[3] == 0 &&
                 next[4] == 0) {
        set_len = 5;
      }
      if (set_len == 0) continue;
      if (next[set_len] != 0xEB && next[set_len] != 0xE9) continue;
    }
    // The index is read again later in the same function.
    bool read_back = false;
    for (unsigned long rva = test_at + test_len; rva + 6 < f->end; ++rva) {
      const BYTE* q = image_ + rva;
      if (q[0] == 0x8B && (q[1] & 0xC7) == 0x05) {
        if (rva + 6 + *reinterpret_cast<const int*>(q + 2) == target) read_back = true;
      }
    }
    if (!read_back) continue;

    bool writable = false;
    if (FindSection(image_, target, sizeof(DWORD), &writable) == nullptr || !writable) {
      continue;
    }
    const DWORD value = *reinterpret_cast<const DWORD*>(image_ + target);
    if (value != 0xFFFFFFFFu && value > 0x4000u) continue;

    bool known = false;
    for (unsigned long g : found) known = known || g == target;
    if (!known) found.push_back(target);
  }

  if (found.size() != 1) return;  // several matches mean the shape was not specific
  found_ = true;
  rva_ = found[0];
}

bool CrtProbe::Ready() const {
  if (!found_ || image_ == nullptr) return false;
  return *reinterpret_cast<const DWORD*>(image_ + rva_) != 0xFFFFFFFFu;
}

}  // namespace loader
