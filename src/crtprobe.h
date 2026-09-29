// "Has the game's own CRT started yet?" -- the check that keeps the loader from
// starting threads too early.
//
// Why this exists (Stellaris 4.5.1, this machine, 2026-09-29): the game's
// statically linked CRT registers a TLS callback that gives every *new* thread
// its per-thread CRT data. That callback needs one process-wide value -- a TLS
// index the CRT allocates while the game's own start-up runs -- and a thread
// whose callback runs before that value exists does not fail quietly: the CRT
// helper returns NULL and its caller calls abort(), i.e. __fastfail(7). The
// game then dies with 0xC0000409 (BEX64) before its first window, which is what
// happens when the main thread is suspended during start-up (the launcher's
// process creation does that) while a proxy DLL has already started a thread of
// its own. Crash dumps of that abort (stellaris_mod_loader's own thread as the
// faulting thread) are the evidence.
//
// So before any thread of ours is created, this probe answers one question: has
// the game's CRT allocated that index yet? The variable is located in the main
// module by its code shape -- the "index == -1 means not yet" test followed by
// the read of the same index -- and cross-checked against the module's import
// table: the same function has to call the TLS functions through the IAT. The
// value is then read, never poked: while it still is 0xFFFFFFFF the CRT is not
// up and nothing may be started; any other small value means a thread can be
// created safely.
//
// A file that cannot be matched (another host, a future build, a different CRT)
// simply yields Found() == false; the caller then falls back to the plain
// delay, which is what the loader did before this probe existed.
#pragma once

#include <windows.h>

namespace loader {

class CrtProbe {
 public:
  // Runs the (read-only) scan over a mapped module. Safe to call from DllMain:
  // it only reads memory and never touches the loader.
  void Locate(HMODULE main_module);

  bool Found() const { return found_; }

  // RVA of the variable inside the main module, for the log.
  unsigned long Rva() const { return rva_; }

  // True once the game's CRT has allocated its index, i.e. once a thread can be
  // created without being aborted by the game's own TLS callback. False while
  // the variable still holds -1, and also when no probe was found.
  bool Ready() const;

 private:
  bool found_ = false;
  unsigned long rva_ = 0;
  BYTE* image_ = nullptr;
};

}  // namespace loader
