# -*- coding: utf-8 -*-
r"""Build the five proxy DLLs and test them without touching the game.

  1. export check: every name and ordinal of the System32 DLL must appear in the
     proxy, and every export must be a forwarder back to that file
  2. host stub: a program that imports the very functions the game imports from
     all of the names, so an unresolvable forwarder fails the test at load time
  3. loader tests: a host named stellaris.exe plus immediate child directories
     of injected_mods (root files and deeper directories must be ignored) --
     the test DLL must be loaded once, bad files skipped, a foreign host name
     must be ignored, several proxies together must still load the mods once
  4. loader options: the ini in every shape an editor writes it (with and
     without BOM, UTF-16, unusable values, unknown keys), the probe setting, and
     a directory named *.dll inside a mod folder must not be taken for a DLL

The DLLs built here land in build_out\ -- what players get is built by
build.bat into ..\stellaris_mod_injector_dll\. When both exist their code
sections are compared and a note says whether the shipped DLLs are the ones
that were just tested.

usage: py -3 tools/run_tests.py [--no-build] [--out <dir>]
       --no-build tests the DLLs already in build_out (or in --out)
       --out <dir> tests an existing build instead, e.g. the shipped DLLs
"""
import hashlib
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))          # ...\stellaris_mod_injector_dll_src\tools
SRC = os.path.dirname(ROOT)                                # ...\stellaris_mod_injector_dll_src
SHIPPED = os.path.join(os.path.dirname(SRC), 'stellaris_mod_injector_dll')
OUT = os.path.join(SRC, 'build_out')                       # the DLLs under test
WORK = os.path.join(SRC, 'test_out')
SYS32 = r'C:\Windows\System32'
NAMES = ['dxgi', 'd3d11', 'version', 'winmm', 'd3dcompiler_47']
GXX = shutil.which('g++')

FAILURES = []
CHECKS = [0]


def check(condition, message):
    CHECKS[0] += 1
    if condition:
        print('  ok   %s' % message)
    else:
        print('  FAIL %s' % message)
        FAILURES.append(message)


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gendef = load_module(os.path.join(ROOT, 'gen_proxy_def.py'), 'gendef')


def run(cmd, cwd=None, timeout=180, env=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout,
                          env=env)


def code_section(path):
    """md5 of the .text section: the compiled code, without the headers, the
    build timestamp and the image base the linker picks for every build."""
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    fh = pe + 4
    nsec, = struct.unpack_from('<H', data, fh + 2)
    optsz, = struct.unpack_from('<H', data, fh + 16)
    table = fh + 20 + optsz
    for i in range(nsec):
        if data[table + 40 * i:table + 40 * i + 8].rstrip(b'\0') != b'.text':
            continue
        _, _, raw_size, raw_at = struct.unpack_from('<IIII', data, table + 40 * i + 8)
        return hashlib.md5(data[raw_at:raw_at + raw_size]).hexdigest()
    return None


def report_shipped():
    """What players have is not necessarily what was just tested."""
    if os.path.abspath(OUT) == os.path.abspath(SHIPPED) or not os.path.isdir(SHIPPED):
        return
    print('== shipped DLLs ==')
    differ = []
    for name in NAMES:
        built = os.path.join(OUT, name + '.dll')
        shipped = os.path.join(SHIPPED, name + '.dll')
        if not os.path.exists(shipped):
            continue
        if not os.path.exists(built) or code_section(built) != code_section(shipped):
            differ.append(name)
    if differ:
        print('  NOTE ..\\stellaris_mod_injector_dll holds other code than the build tested'
              ' here (%s)' % ', '.join(n + '.dll' for n in differ))
        print('       run build.bat to refresh it, or --out ..\\stellaris_mod_injector_dll'
              ' --no-build to test it as it is')
    else:
        print('  ok   ..\\stellaris_mod_injector_dll holds the same code as the build '
              'under test')


def warn_if_stale():
    """--no-build tests whatever sits in OUT; say so when the sources moved on."""
    sources = []
    for folder, endings in ((os.path.join(SRC, 'src'), ('.cpp', '.h')),
                            (os.path.join(SRC, 'def'), ('.def',))):
        sources += [os.path.join(folder, e) for e in os.listdir(folder)
                    if e.endswith(endings)]
    if not sources:
        return
    newest = max(os.path.getmtime(p) for p in sources)
    stale = [n for n in NAMES
             if os.path.exists(os.path.join(OUT, n + '.dll'))
             and os.path.getmtime(os.path.join(OUT, n + '.dll')) < newest]
    if stale:
        print('  NOTE %s in %s are older than the sources -- rebuild before trusting '
              'a green run' % (', '.join(stale), OUT))


# --------------------------------------------------------------- 1. exports

def check_exports():
    print('== export tables ==')
    for name in NAMES:
        dll = name + '.dll'
        real, real_ordinal_only = gendef.read_exports(os.path.join(SYS32, dll))
        proxy, proxy_ordinal_only = gendef.read_exports(os.path.join(OUT, dll))
        real_map = {e['name']: e['ordinal'] for e in real}
        proxy_map = {e['name']: e['ordinal'] for e in proxy}
        missing = sorted(set(real_map) - set(proxy_map))
        extra = sorted(set(proxy_map) - set(real_map))
        wrong_ord = sorted(n for n in set(real_map) & set(proxy_map)
                           if real_map[n] != proxy_map[n])
        expected = ('%s/%s' % (SYS32.replace('\\', '/'), dll)).lower()
        bad_forwarder = [e['name'] for e in proxy
                         if not e['forwarder']
                         or e['forwarder'].replace('\\', '/').lower() !=
                         ('%s.%s' % (expected, e['name'])).lower()]
        check(not missing, '%s: every name is exported (%d, missing: %s)'
              % (dll, len(real_map), missing or 'none'))
        check(not extra, '%s: no invented names (%s)' % (dll, extra or 'none'))
        check(not wrong_ord, '%s: ordinals match (%s)' % (dll, wrong_ord or 'none'))
        check(not bad_forwarder, '%s: every export forwards to System32 (%s)'
              % (dll, bad_forwarder or 'none'))
        if real_ordinal_only:
            print('       note: %s has %d ordinal-only export(s); those cannot be '
                  'forwarded by name' % (dll, real_ordinal_only))


# ------------------------------------------------------------ 2. build tools

def build_tools():
    print('== build host stub and test mod ==')
    host_dir = os.path.join(WORK, 'host')
    mod_dir = os.path.join(WORK, 'mod')
    os.makedirs(host_dir, exist_ok=True)
    os.makedirs(mod_dir, exist_ok=True)
    host = os.path.join(host_dir, 'stellaris.exe')
    r = run([GXX, '-O1', '-static', '-o', host, os.path.join(ROOT, 'host_stub.cpp'),
             '-lversion', '-lwinmm', '-ldxgi', '-ld3d11', '-ld3dcompiler_47'], cwd=ROOT)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit('cannot build the host stub')
    r = run([GXX, '-O1', '-static', '-shared', '-o', os.path.join(mod_dir, 'zz_test_mod.dll'),
             os.path.join(ROOT, 'test_mod.cpp')], cwd=ROOT)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit('cannot build the test mod')
    print('  host: %s' % host)


# -------------------------------------------------------------- 3. helpers

def setup_case(case, proxies, host_name='stellaris.exe', with_mods=True, ini=None,
               extra_files=(), extra_dirs=(), self_copy=False):
    case_dir = os.path.join(WORK, case)
    shutil.rmtree(case_dir, ignore_errors=True)
    os.makedirs(case_dir)
    for name in proxies:
        shutil.copy(os.path.join(OUT, name + '.dll'), os.path.join(case_dir, name + '.dll'))
    shutil.copy(os.path.join(WORK, 'host', 'stellaris.exe'), os.path.join(case_dir, host_name))
    if with_mods:
        mods = os.path.join(case_dir, 'injected_mods')
        mod_dir = os.path.join(mods, 'test_mod')
        os.makedirs(mod_dir)
        shutil.copy(os.path.join(WORK, 'mod', 'zz_test_mod.dll'), mod_dir)
        for source, target in extra_files:
            shutil.copy(source, os.path.join(mod_dir, target))
        for name in extra_dirs:
            os.makedirs(os.path.join(mod_dir, name))
        if self_copy:
            shutil.copy(os.path.join(OUT, proxies[0] + '.dll'),
                        os.path.join(mod_dir, proxies[0] + '.dll'))
        if ini is not None:
            # bytes go in as they are: the ini is read in several encodings
            blob = ini if isinstance(ini, bytes) else ini.encode('ascii')
            with open(os.path.join(mods, 'stellaris_mod_injector.ini'), 'wb') as fh:
                fh.write(blob)
    return case_dir


def run_host(case_dir, exe='stellaris.exe', timeout=60, args=()):
    return run([os.path.join(case_dir, exe)] + list(args), cwd=case_dir, timeout=timeout)


def read(path):
    if not os.path.exists(path):
        return ''
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        return fh.read()


def bad_files():
    """A 32-bit DLL, a text file that pretends to be one (long enough to pass the
    size check, so the MZ check is what rejects it), and a file with a good MZ
    whose header offset points nowhere."""
    syswow = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'SysWOW64', 'version.dll')
    broken = os.path.join(WORK, 'broken.dll')
    with open(broken, 'w', encoding='ascii') as fh:
        fh.write('this is not a PE image, it is only here to be skipped\n' * 5)
    bad_header = os.path.join(WORK, 'bad_header.dll')
    blob = bytearray(b'\0' * 0x80)
    blob[0:2] = b'MZ'                        # passes the MZ check ...
    struct.pack_into('<I', blob, 0x3c, 0)    # ... and fails on e_lfanew == 0
    with open(bad_header, 'wb') as fh:
        fh.write(bytes(blob))
    return syswow, broken, bad_header


# --------------------------------------------------------------- 4. tests

def test_noinject_markers():
    print('== loader: sibling dllnoinject markers ==')
    for name in NAMES:
        case = setup_case('t16_noinject_' + name, [name], ini='probe=1\n')
        mods = os.path.join(case, 'injected_mods')
        disabled = os.path.join(mods, 'test_mod')
        marker = os.path.join(disabled, 'zz_test_mod.dllnoinject')
        with open(marker, 'wb'):
            pass
        # Invalid DLLs must be disabled before the PE check, too. Marker
        # contents are irrelevant and marker files are never candidates.
        with open(os.path.join(disabled, 'broken.dll'), 'wb') as fh:
            fh.write(b'not a DLL')
        with open(os.path.join(disabled, 'broken.dllnoinject'), 'w') as fh:
            fh.write('disabled')
        for folder in ['same_name', 'directory_marker']:
            mod_dir = os.path.join(mods, folder)
            os.makedirs(mod_dir)
            shutil.copy(os.path.join(WORK, 'mod', 'zz_test_mod.dll'), mod_dir)
        os.makedirs(os.path.join(mods, 'directory_marker', 'zz_test_mod.dllnoinject'))
        with open(os.path.join(mods, 'same_name', 'zz_test_mod.noinject'), 'wb'):
            pass
        # The old suffix no longer disables a DLL.
        with open(os.path.join(mods, 'same_name', 'zz_test_mod.dllnoload'), 'wb'):
            pass

        r = run_host(case)
        log_path = os.path.join(mods, 'stellaris_mod_injector.log')
        log = read(log_path)
        probe_flag = os.path.join(disabled, 'diplo_action_hook_probe_only.txt')
        check(r.returncode == 0, '[%s] noinject host ran' % name)
        check('[skip] zz_test_mod.dll: disabled by zz_test_mod.dllnoinject' in log,
              '[%s] an empty sibling marker skips the DLL with a reason' % name)
        check('[skip] broken.dll: disabled by broken.dllnoinject' in log,
              '[%s] noinject is checked before the PE header' % name)
        check('4 candidate(s), 2 to load' in log and '2 of 2 DLL(s) loaded' in log,
              '[%s] marker files are not candidates or counted as loadable' % name)
        check(not os.path.exists(os.path.join(disabled, 'zz_test_mod.log')),
              '[%s] the disabled DLL never ran DllMain' % name)
        check(not os.path.exists(probe_flag),
              '[%s] probe=1 does not create a flag for disabled DLLs' % name)
        for folder in ['same_name', 'directory_marker']:
            check(read(os.path.join(mods, folder, 'zz_test_mod.log')).count(
                      'zz_test_mod loaded') == 1,
                  '[%s] %s still loads its DLL' % (name, folder))
        check('probe flag: present' in read(os.path.join(
                  mods, 'same_name', 'zz_test_mod.log')),
              '[%s] the old dllnoload marker no longer disables loading or probe handling' % name)

        with open(os.path.join(mods, 'stellaris_mod_injector.ini'), 'w') as fh:
            fh.write('probe=0\n')
        with open(probe_flag, 'w') as fh:
            fh.write('keep while disabled\n')
        r = run_host(case)
        check(r.returncode == 0 and read(probe_flag) == 'keep while disabled\n',
              '[%s] probe=0 preserves a disabled DLL\'s existing flag' % name)
        check(not os.path.exists(os.path.join(disabled, 'zz_test_mod.log')),
              '[%s] the DLL stays disabled on the next launch' % name)

        os.remove(marker)
        r = run_host(case)
        log = read(log_path)
        check(r.returncode == 0 and '3 of 3 DLL(s) loaded' in log,
              '[%s] removing the marker restores loading on the next launch' % name)
        check(read(os.path.join(disabled, 'zz_test_mod.log')).count(
                  'zz_test_mod loaded') == 1 and not os.path.exists(probe_flag),
              '[%s] the restored DLL runs once and normal probe handling resumes' % name)


def test_each_proxy():
    print('== loader: one proxy per case ==')
    syswow, broken, bad_header = bad_files()
    for name in NAMES:
        case = setup_case('t1_' + name, [name], ini='delay_ms=300\n',
                          extra_files=[(syswow, 'bad_32bit.dll'), (broken, 'broken.dll'),
                                       (bad_header, 'bad_header.dll')],
                          extra_dirs=['nested.dll'], self_copy=True)
        mods = os.path.join(case, 'injected_mods')
        r = run_host(case)
        log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
        mod_log = read(os.path.join(mods, 'test_mod', 'zz_test_mod.log'))
        check(os.path.exists(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log')),
              '[%s] the loader log is inside injected_mods' % name)
        check(not os.path.exists(os.path.join(case, 'stellaris_mod_loader.log')),
              '[%s] the old log name is not created' % name)
        check(not os.path.exists(os.path.join(case, 'stellaris_mod_injector.log')),
              '[%s] no loader log is created in the game root' % name)
        check(r.returncode == 0, '[%s] host ran (exit %s)' % (name, r.returncode))
        check('version size : 0' not in r.stdout and 'version size :' in r.stdout,
              '[%s] forwarded version API answered' % name)
        check('loading [1/1] zz_test_mod.dll ... ok' in log,
              '[%s] the test DLL was loaded' % name)
        check(mod_log.count('zz_test_mod loaded') == 1,
              '[%s] its DllMain ran exactly once' % name)
        check('1 of 1 DLL(s) loaded' in log, '[%s] summary line' % name)
        check('[skip] bad_32bit.dll: not an x64 DLL' in log,
              '[%s] 32-bit DLL rejected' % name)
        check('[skip] broken.dll: not a PE image' in log,
              '[%s] non-PE file rejected' % name)
        check('[skip] bad_header.dll: not a PE image (bad header offset)' in log,
              '[%s] a file with a broken header offset is rejected' % name)
        check('nested.dll' not in log and '5 candidate(s), 1 to load' in log,
              '[%s] a directory inside a mod folder is not a candidate' % name)
        check('[skip] %s.dll: this is the loader itself' % name in log,
              '[%s] a copy of the loader inside a mod folder is skipped' % name)
        # The delay_ms of the ini is echoed by the worker; the start-up gate is
        # what the timing now hangs on, so both are checked here. delay_ms=300 is
        # below the 700 ms a host without a CRT probe waits for, so the start
        # line has to show the process age the wake-up arrived at.
        check('stellaris_mod_injector.ini (delay_ms=300)' in log,
              '[%s] the log names the delay_ms it used' % name)
        check(read(os.path.join(mods, 'stellaris_mod_injector.ini')) == 'delay_ms=300\n',
              '[%s] an existing ini is not rewritten to add defaults' % name)
        started = re.search(r'start : the message loop started \((\d+) ms into the process\)', log)
        check(started is not None,
              '[%s] the loader thread was started by the message-loop wake-up' % name)
        check(started is not None and int(started.group(1)) >= 300,
              '[%s] it was not started before delay_ms (%s ms)'
              % (name, started.group(1) if started else '-'))
        check('start : the game\'s CRT was already up at attach' not in log
              and 'start : no user32' not in log,
              '[%s] no thread was started at attach' % name)
        if name == 'dxgi':
            print('       ---- %s log ----\n%s' % (name, '\n'.join(
                '       ' + line for line in log.strip().splitlines())))
            print('       ---- host output ----\n%s' % '\n'.join(
                '       ' + line for line in r.stdout.strip().splitlines()))


def test_scan_depth():
    print('== loader: only immediate child directories ==')
    for name in NAMES:
        case = setup_case('t10_depth_' + name, [name], ini='delay_ms=300\n')
        mods = os.path.join(case, 'injected_mods')
        deep = os.path.join(mods, 'test_mod', 'deeper', 'deepest')
        sibling = os.path.join(mods, 'package.dll')
        os.makedirs(deep)
        os.makedirs(sibling)
        source = os.path.join(WORK, 'mod', 'zz_test_mod.dll')
        shutil.copy(source, os.path.join(mods, 'ignored_root.dll'))
        shutil.copy(source, os.path.join(os.path.dirname(deep), 'ignored_deep.dll'))
        shutil.copy(source, os.path.join(deep, 'ignored_deepest.dll'))
        shutil.copy(source, os.path.join(sibling, 'aa_sibling.DLL'))
        shutil.copy(source, os.path.join(sibling, 'ignored_text.txt'))
        r = run_host(case)
        log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
        check(r.returncode == 0, '[%s] depth-test host ran' % name)
        check('2 candidate(s), 2 to load' in log and '2 of 2 DLL(s) loaded' in log,
              '[%s] exactly the two immediate-child DLLs are loaded' % name)
        check('loading [1/2] aa_sibling.DLL ... ok' in log
              and 'loading [2/2] zz_test_mod.dll ... ok' in log,
              '[%s] sibling folders and uppercase extensions use file-name order' % name)
        check(read(os.path.join(sibling, 'zz_test_mod.log')).count('zz_test_mod loaded') == 1
              and read(os.path.join(mods, 'test_mod', 'zz_test_mod.log')).count(
                  'zz_test_mod loaded') == 1,
              '[%s] both child-folder DLLs ran exactly once' % name)
        check('ignored_root.dll' not in log
              and not os.path.exists(os.path.join(mods, 'zz_test_mod.log')),
              '[%s] injected_mods root DLLs are ignored' % name)
        check('ignored_deep.dll' not in log and 'ignored_deepest.dll' not in log
              and not os.path.exists(os.path.join(os.path.dirname(deep), 'zz_test_mod.log'))
              and not os.path.exists(os.path.join(deep, 'zz_test_mod.log')),
              '[%s] deeper DLLs are ignored' % name)
        check('ignored_text.txt' not in log,
              '[%s] non-DLL files in child folders are ignored' % name)


def test_foreign_host():
    print('== loader: a host that is not the game ==')
    case = setup_case('t2_foreign', ['dxgi'], host_name='not_the_game.exe')
    r = run_host(case, exe='not_the_game.exe')
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check(r.returncode == 0, 'the foreign host still ran (exit %s)' % r.returncode)
    check('host is not stellaris.exe; nothing to do' in log,
          'the loader refused to act outside the game')
    check(not os.path.exists(os.path.join(case, 'injected_mods', 'test_mod', 'zz_test_mod.log')),
          'no mod was loaded into the foreign host')
    case = setup_case('t2_foreign_nomods', ['dxgi'], host_name='not_the_game.exe',
                      with_mods=False)
    r = run_host(case, exe='not_the_game.exe')
    check(r.returncode == 0 and not os.path.exists(os.path.join(case, 'injected_mods')),
          'the foreign host does not create a mods directory')


def test_several_proxies():
    print('== loader: three proxies at once ==')
    case = setup_case('t3_multi', ['dxgi', 'version', 'd3d11'], ini='delay_ms=300\n')
    r = run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    mod_log = read(os.path.join(case, 'injected_mods', 'test_mod', 'zz_test_mod.log'))
    check(r.returncode == 0, 'host ran (exit %s)' % r.returncode)
    check(log.count('loading [1/1] zz_test_mod.dll ... ok (module') == 1,
          'exactly one proxy loaded the mods')
    check(log.count('was already loaded in the process') >= 2,
          'the other proxies reported the module as already loaded')
    check(mod_log.count('zz_test_mod loaded') == 1, 'DllMain still ran exactly once')


def test_probe_mode():
    print('== loader: ini probe setting and switching it off ==')
    case = setup_case('t4_probe', ['dxgi'], ini='delay_ms=300\nprobe=1\n')
    mods = os.path.join(case, 'injected_mods')
    flag = os.path.join(mods, 'test_mod', 'diplo_action_hook_probe_only.txt')
    mod_log = os.path.join(mods, 'test_mod', 'zz_test_mod.log')
    run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check('probe mode: creating diplo_action_hook_probe_only.txt' in log,
          'probe=1 enables probe mode without the old marker')
    check(os.path.exists(flag),
          'diplo_action_hook_probe_only.txt was created next to the mod')
    check('probe flag: present' in read(mod_log), 'the probe flag exists before mod DllMain runs')
    ini = 'delay_ms=300\nprobe=0\n'
    with open(os.path.join(mods, 'stellaris_mod_injector.ini'), 'w', encoding='ascii') as fh:
        fh.write(ini)
    with open(os.path.join(mods, 'stellaris_mod_loader_probe.txt'), 'w', encoding='ascii') as fh:
        fh.write('legacy marker\n')
    run_host(case)
    log = read(os.path.join(mods, 'stellaris_mod_injector.log'))
    check('ini   : probe=0' in log and 'probe mode:' not in log,
          'probe=0 disables probe mode even with an old marker')
    check(not os.path.exists(flag), 'switching probe off removes the previous probe flag')
    check(read(mod_log).splitlines()[-1] == 'probe flag: absent',
          'the stale probe flag is removed before mod DllMain runs')
    check(read(os.path.join(mods, 'stellaris_mod_injector.ini')) == ini,
          'existing player settings are preserved')


def test_missing_mods_dir():
    print('== loader: no injected_mods ==')
    for proxies in [[name] for name in NAMES] + [['dxgi', 'version', 'd3d11']]:
        label = '_'.join(proxies)
        case = setup_case('t5_nomods_' + label, proxies, with_mods=False)
        r = run_host(case)
        mods = os.path.join(case, 'injected_mods')
        log_path = os.path.join(mods, 'stellaris_mod_injector.log')
        log = read(log_path)
        check(r.returncode == 0, '[%s] host ran without a mods folder' % label)
        check(os.path.isdir(mods), '[%s] the missing folder was created' % label)
        check(os.path.isfile(log_path) and "created 'injected_mods' next to stellaris.exe" in log,
              '[%s] the log was opened after creating the folder' % label)
        check('0 candidate(s), 0 to load' in log and 'nothing to load' in log,
              '[%s] the new empty folder has no DLL candidates' % label)
        check(not os.path.exists(os.path.join(case, 'stellaris_mod_injector.log')),
              '[%s] no log was written to the game root' % label)
        ini_path = os.path.join(mods, 'stellaris_mod_injector.ini')
        check(os.path.isfile(ini_path) and read(ini_path) == read(os.path.join(
                  OUT, 'injected_mods', 'stellaris_mod_injector.ini')),
              '[%s] the default ini is created after the missing directory' % label)
        check('stellaris_mod_injector.ini (delay_ms=700)' in log and 'ini   : probe=0' in log,
              '[%s] the auto-created defaults are read' % label)


def test_missing_ini():
    print('== loader: existing mods directory with no ini ==')
    for proxies in [[name] for name in NAMES] + [['dxgi', 'version', 'd3d11']]:
        label = '_'.join(proxies)
        case = setup_case('t14_missing_ini_' + label, proxies)
        mods = os.path.join(case, 'injected_mods')
        r = run_host(case)
        log = read(os.path.join(mods, 'stellaris_mod_injector.log'))
        check(r.returncode == 0 and '1 of 1 DLL(s) loaded' in log,
              '[%s] mods still load after default ini creation' % label)
        check(read(os.path.join(mods, 'stellaris_mod_injector.ini')) == read(os.path.join(
                  OUT, 'injected_mods', 'stellaris_mod_injector.ini')),
              '[%s] runtime defaults match the packaged ini' % label)
        check(not os.path.exists(os.path.join(mods, 'test_mod', 'diplo_action_hook_probe_only.txt')),
              '[%s] the generated defaults disable probe mode' % label)


def test_mods_path_is_file():
    print('== loader: injected_mods path is an existing file ==')
    case = setup_case('t11_mods_file', ['dxgi'], with_mods=False)
    mods = os.path.join(case, 'injected_mods')
    with open(mods, 'w', encoding='ascii') as fh:
        fh.write('keep this file\n')
    r = run_host(case)
    check(r.returncode == 0, 'a blocked mods directory does not prevent the host from running')
    check(read(mods) == 'keep this file\n', 'the existing file is preserved')
    check(not os.path.exists(os.path.join(case, 'stellaris_mod_injector.log')),
          'directory creation failure does not write a log in the game root')


def test_ini_path_is_directory():
    print('== loader: ini path is an existing directory ==')
    case = setup_case('t15_ini_directory', ['dxgi'])
    ini_path = os.path.join(case, 'injected_mods', 'stellaris_mod_injector.ini')
    os.makedirs(ini_path)
    r = run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check(r.returncode == 0 and '1 of 1 DLL(s) loaded' in log,
          'ini creation failure still allows mods to load with defaults')
    check(os.path.isdir(ini_path), 'an existing directory at the ini path is preserved')
    check('ini   : could not create default settings:' in log,
          'ini creation failure is reported in the log')


def test_default_ini():
    print('== loader: packaged default ini ==')
    ini_path = os.path.join(OUT, 'injected_mods', 'stellaris_mod_injector.ini')
    check(os.path.isfile(ini_path), 'the build includes injected_mods/stellaris_mod_injector.ini')
    case = setup_case('t12_default_ini', ['dxgi'], ini=read(ini_path))
    r = run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check(r.returncode == 0 and 'stellaris_mod_injector.ini (delay_ms=700)' in log,
          'the packaged ini uses the default delay of 700 ms')
    check('1 of 1 DLL(s) loaded' in log, 'the default configuration loads the mod')
    check('ini   : probe=0' in log and 'probe mode:' not in log,
          'the packaged ini disables probe mode by default')


def test_old_ini_name_is_ignored():
    print('== loader: renamed ini ==')
    case = setup_case('t13_ini_name', ['dxgi'], ini='delay_ms=300\n')
    with open(os.path.join(case, 'injected_mods', 'stellaris_mod_loader.ini'), 'w',
              encoding='ascii') as fh:
        fh.write('delay_ms=1500\n')
    run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check('stellaris_mod_injector.ini (delay_ms=300)' in log and 'delay_ms=1500' not in log,
          'the new ini is used even when an old-name ini exists')


# The shapes an editor can leave behind. "UTF-8" in older Notepad was UTF-8 with
# a BOM and "Unicode" is UTF-16LE, so a BOM in front of delay_ms is exactly what
# a player gets without doing anything unusual -- a BOM used to make the key
# unrecognised, which silently left the default in place.
INI_CASES = [
    ('lf', b'delay_ms=1500\nprobe=1\n'),
    ('crlf', b'delay_ms=1500\r\nprobe=1\r\n'),
    ('utf8_bom', b'\xef\xbb\xbfdelay_ms=1500\nprobe=1\n'),
    ('utf16_le', b'\xff\xfe' + 'delay_ms=1500\r\nprobe=1\r\n'.encode('utf-16-le')),
    ('utf16_be', b'\xfe\xff' + 'delay_ms=1500\nprobe=1\n'.encode('utf-16-be')),
    ('comments_spaces', b'# wait a bit longer\r\n  delay_ms  =  1500  \r\n  PrObE = 1 \r\n'),
]


def test_launcher_style_wakeup():
    print('== loader: the launcher shape (no message loop, a late thread) ==')
    # Through the launcher the game is walked on a thread of its own and its own
    # message loop comes much later, so the timer the proxy arms is useless
    # there. What has to get the mods loaded is the thread attach -- and it has
    # to wait for the host to be past its start-up before it acts on it.
    case = setup_case('t8_nopump', ['dxgi'], ini='delay_ms=300\n')
    r = run_host(case, args=('--no-pump',))
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check(r.returncode == 0, 'host ran (exit %s)' % r.returncode)
    check('start : a thread attached after the CRT was up' in log,
          'the thread attach is what started the loader thread')
    check('loading [1/1] zz_test_mod.dll ... ok' in log, 'the mod was loaded')


def test_log_is_replaced():
    print('== loader: each start replaces the log ==')
    # A file an append-mode loader would have left behind, with a marker line
    # that must not survive the next start.
    case = setup_case('t9_replace', ['dxgi'], ini='delay_ms=300\n')
    with open(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'), 'w', encoding='ascii') as fh:
        fh.write("[     0.000] stellaris_mod_loader 1.0 -- proxy 'dxgi.dll', pid 1234\n")
        fh.write('MARKER FROM AN EARLIER RUN\n')
    run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check('MARKER FROM AN EARLIER RUN' not in log, 'the earlier run is gone')
    check(log.count('stellaris_mod_loader 1.1 -- proxy') == 1,
          'the file holds exactly the run that just happened')
    check(log.splitlines() and 'stellaris_mod_loader 1.1' in log.splitlines()[0],
          'and it starts at its own header line')


def test_delay_is_waited_out():
    print('== loader: the worker still waits out delay_ms ==')
    # With a delay longer than the 700 ms a host without a CRT probe waits for,
    # the worker has to sleep the rest off before it loads anything -- the same
    # line the old build wrote.
    case = setup_case('t9_longdelay', ['dxgi'], ini='delay_ms=1500\n')
    run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check('waiting' in log and '(delay_ms=1500,' in log,
          'the worker waited the rest of delay_ms out')
    check('1 of 1 DLL(s) loaded' in log, 'the mod was loaded after the wait')


def test_ini_encodings():
    print('== loader: the ini as editors write it ==')
    for label, blob in INI_CASES:
        case = setup_case('t6_ini_' + label, ['dxgi'], ini=blob)
        mods = os.path.join(case, 'injected_mods')
        run_host(case)
        log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
        check('(delay_ms=1500,' in log,
              '[%s] delay_ms was read and used, not the 700 default' % label)
        check('stellaris_mod_injector.ini (delay_ms=1500)' in log,
              '[%s] the log names the ini and the value it produced' % label)
        check('ini   : probe=1' in log and os.path.exists(os.path.join(
                  mods, 'test_mod', 'diplo_action_hook_probe_only.txt')),
              '[%s] probe is read correctly in this encoding' % label)


def test_ini_rejections():
    print('== loader: ini values that cannot be used ==')
    ini = ('delay_ms=0\n'
           'delay=700\n'
           'this line has no equals sign\n'
           'delay_ms=1000000\n'
           'probe=-1\nprobe=2\nprobe=1garbage\n')
    case = setup_case('t7_ini_bad', ['dxgi'], ini=ini)
    mods = os.path.join(case, 'injected_mods')
    run_host(case)
    log = read(os.path.join(case, 'injected_mods', 'stellaris_mod_injector.log'))
    check("ini   : delay_ms '0' is not 1..599999" in log,
          'a value below the range is reported')
    check("ini   : delay_ms '1000000' is not 1..599999" in log,
          'a value above the range is reported')
    check("ini   : unknown key 'delay' ignored" in log,
          'a misspelled key is reported instead of silently ignored')
    check("ini   : line without '=' ignored" in log, 'a junk line is reported')
    check('delay_ms=700' in log, 'the default was used instead')
    check('1 of 1 DLL(s) loaded' in log, 'the mod was still loaded')
    check(all("ini   : probe '%s' is not 0 or 1; using 0" % value in log
              for value in ['-1', '2', '1garbage']),
          'invalid probe values are reported and leave the default disabled')
    check(not os.path.exists(os.path.join(mods, 'test_mod', 'diplo_action_hook_probe_only.txt')),
          'invalid probe values do not create a flag')


def main():
    if GXX is None:
        sys.exit('g++ not found')
    if '--out' in sys.argv:
        i = sys.argv.index('--out')
        if i + 1 >= len(sys.argv):
            sys.exit('--out needs a directory')
        globals()['OUT'] = os.path.abspath(sys.argv[i + 1])
    print('== build ==')
    if '--no-build' not in sys.argv:
        r = run(['cmd', '/c', 'build.bat'], cwd=SRC, timeout=600,
                env=dict(os.environ, OUTDIR=OUT))
        print(r.stdout.strip()[-400:])
        if r.returncode:
            print(r.stderr)
            sys.exit('build failed')
    else:
        print('  (--no-build: testing the DLLs already in %s)' % OUT)
    for name in NAMES:
        if not os.path.exists(os.path.join(OUT, name + '.dll')):
            sys.exit('%s has no %s.dll -- build first (or pass another --out)' % (OUT, name))
    print('  testing: %s' % OUT)
    warn_if_stale()
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK, exist_ok=True)
    check_exports()
    report_shipped()
    build_tools()
    test_each_proxy()
    test_scan_depth()
    test_noinject_markers()
    test_foreign_host()
    test_several_proxies()
    test_probe_mode()
    test_missing_mods_dir()
    test_missing_ini()
    test_mods_path_is_file()
    test_ini_path_is_directory()
    test_default_ini()
    test_old_ini_name_is_ignored()
    test_launcher_style_wakeup()
    test_log_is_replaced()
    test_delay_is_waited_out()
    test_ini_encodings()
    test_ini_rejections()
    print()
    print('%d checks, %d failure(s)' % (CHECKS[0], len(FAILURES)))
    for failure in FAILURES:
        print('  FAILED: %s' % failure)
    return 1 if FAILURES else 0


if __name__ == '__main__':
    sys.exit(main())
