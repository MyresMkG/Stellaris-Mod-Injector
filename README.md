# stellaris_mod_injector —— injected_mods 注入器

把 `injected_mods` 目录里的**所有 DLL** 注入群星主程序的注入器。

**它不修改游戏的任何文件**，DLL 只存在于进程内存里；关掉游戏就干净了。

（注：使用deepseek-v4.1-flash编写，harness为Kimi Code）

---

## 1. 文件

```
 stellaris_mod_injector.cpp   全部源码（单文件，无第三方依赖）
 build.bat           用 MinGW-w64 编译，产物直接写到 ..\stellaris_mod_injector_bin\
 README.md           本文件
```

编译（需要 PATH 里有 MinGW-w64 的 `g++`，或设 `MINGW_BIN`）：

```
build.bat                 rem 产物写到 ..\stellaris_mod_injector_bin\stellaris_mod_injector.exe
build.bat D:\somewhere    rem 也可以指定别的输出目录
```

编译参数： `-std=c++17 -O2 -static -static-libgcc -static-libstdc++` ，
产物不依赖 MinGW 运行库。`-Wall -Wextra` 下无警告。

---

## 2. 命令行

```
stellaris_mod_injector.exe [options] [-- <游戏参数>]
```

| 参数 | 作用 |
| --- | --- |
| `--exe <路径>` | 指定游戏主程序（默认：自己旁边的 `stellaris.exe` → Steam 库里已安装的那份） |
| `--mods-dir <路径>` | 指定要扫描的目录（默认：自己旁边的 `injected_mods` → 游戏目录下的 `injected_mods`） |
| `--dll <路径>` | 额外注入一个指定 DLL，可重复；这些排在被扫出来的 DLL **前面** |
| `--attach <名字或PID>` | 注入到已经在跑的进程，不启动新的 |
| `--delay <毫秒>` | 启动后等多久再注入（默认 700，上限 599999） |
| `--new-instance` | 即使游戏已经在跑也**再开一个**（默认是附加到已在跑的那个） |
| `--list` | 只打印会注入哪些、哪些被跳过，然后退出；不碰任何进程 |
| `--probe` | 在每个被注入 DLL 旁边建 `diplo_action_hook_probe_only.txt`，让 diplo 钩子只解析地址不装钩子 |
| `--wait` | 等目标进程退出 |
| `--suspend` | ⚠️ 不推荐：挂起启动再注入。实测会让本机 4.5.1 立刻崩在 `0xC0000005`，默认关闭 |

### 路径解析规则

1. **游戏**：`--exe` > 自己旁边的 `stellaris.exe` > 注册表 `HKCU\Software\Valve\Steam\SteamPath`
   加各库的 `libraryfolders.vdf`（`steamapps\libraryfolders.vdf` 与 `config\libraryfolders.vdf`）
   里能找到的 `steamapps\common\Stellaris\stellaris.exe`。
2. **DLL 目录**：`--mods-dir` > 自己旁边的 `injected_mods`（存在时）> 游戏目录下的 `injected_mods`。
   自己推出来的目录如果落在 Windows 系统目录里（`Windows`、`System32`、`SysWOW64`），**直接拒绝**并报错。

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 全部注进去了（`--list` 时表示"有东西可注"） |
| 1 | 参数/路径错误、没有可注入的 DLL、有 DLL 注入失败，或目标进程在注入前就退出了 |

---

## 3. 实现要点

- **扫描**：`FindFirstFileW(mods\*)` 取所有 `*.dll`（大小写不敏感），按文件名排序，
  所以每次注入顺序都一样；`--dll` 指定的排在前面。同名路径按整路径去重。
- **路径先转绝对**：`--dll` / `--mods-dir` / `--exe` 的相对写法先用 `GetFullPathNameW`
  转成绝对路径，再校验、再注入。本工具校验文件用的是自己的工作目录，而被注入进程
  解析 `LoadLibraryW` 用的是它自己的工作目录（启动模式下是游戏目录），两者不一定
  相同——不转绝对就会出现"校验的是 A、加载的是 B"。
- **参数原样转发**：`--` 后面的参数按 `CommandLineToArgvW` 的规则重新加引号后再拼进
  命令行，所以带空格、带引号、结尾带反斜杠的单个参数，到达被启动程序时仍是同一个参数。
- **校验**：读每个文件的 DOS/NT 头，要求有 `MZ`、`PE\0\0`、`IMAGE_FILE_DLL`，
  且 `Machine == 0x8664`。不合格的直接列成 `[skip] <名字>: <原因>`，不参与注入，
  也不会让整批失败。这一步在**启动游戏之前**跑完，所以目录里全是坏 DLL 时不会白开一次游戏。
- **注入**：`VirtualAllocEx` 写入 UTF-16 全路径 → `CreateRemoteThread` 跑目标进程里的
  `LoadLibraryW` → 等线程结束 → 读返回值（模块句柄）→ 释放远端内存。
  `LoadLibraryW` 在系统 DLL 共享 ASLR 基址的前提下，本地地址就等于目标进程里的地址。
  返回值是 32 位的（`GetExitCodeThread` 只给 DWORD），所以读到 0 时还会用
  `TH32CS_SNAPMODULE` 快照按完整路径复查一次，避免把"其实注进去了"误报成失败。
- **已加载**：注入前会根据模块快照报告"was already loaded in the target"——已加载的模块
  `LoadLibraryW` 直接返回旧句柄、`DllMain` **不会**再跑一次，这一点在日志里说清楚，
  免得看到"注进去了但没效果"无从下手。
- **目标已退出**：任何 `VirtualAllocEx`/`CreateRemoteThread` 失败都用 `GetExitCodeProcess`
  复查一次；进程其实已经死了就报告它的退出码并停止注入剩下的 DLL，而不是抛出费解的
  `error 5 (拒绝访问)`。
- **已经在跑的游戏**：用 `TH32CS_SNAPPROCESS` 按 exe 名找（如 `stellaris.exe`），
  找到就 `OpenProcess` 附加，并提示"数据可能已经加载过，早期钩子可能错过"。
  `OpenProcess` 的权限里包含 `SYNCHRONIZE`，否则 `--wait` 会立刻返回、
  `GetExitCodeProcess` 只能读到 `259 (STILL_ACTIVE)`。
- **出错或跳过了 DLL 时不会闪退，并且把原因再列一遍**：全部注入成功、也没有 DLL 被跳过
  时照旧立刻退出、不等任何按键（`--wait` 也仍然等目标进程退出）。但只要 ① 退出码非 0，
  或 ② 有 DLL 被 PE 校验筛掉（32 位、不是 PE、文件不存在……），并且本进程有自己的控制台
  窗口、标准输入又确实是这个控制台（双击 exe 的典型情况），就会先在末尾把原因汇总重印
  一遍——退出码非 0 时标题是 `1 problem:` / `N problems:`，只是跳过了 DLL 时是
  `1 warning:` / `N warnings:`，下面每条写成 `  * <原因>`——再打印
  `press Enter to close this window (exit code N)` 停下等回车。被跳过的 DLL 意味着那个
  mod 根本没进游戏，一闪而过的窗口最容易漏掉它。标准输入是管道或文件时（脚本、重定向）
  不重印也不等，免得把自动化卡住。
- **UTF-16 全程**：路径、命令行、注册表读取、`_wfopen` 都是宽字符，中文目录可用。

---

## 4. 验证记录（2026-09-25）

| 用例 | 做法 | 结果 |
| --- | --- | --- |
| 扫描与校验 | `--list`，目录里放 3 个好 DLL + 1 个文本冒充的 `broken.dll` + 1 个 32 位 `version.dll` | 3 个列出；两个坏的分辨报 `file is too small to be a PE image` / `not an x64 DLL (machine 0x014c...)`；退出码 0 |
| 批量注入（启动模式） | `--exe notepad.exe --mods-dir <测试目录> --new-instance` | `3 of 3 DLL(s) injected`；两个探针 DLL 的加载标记都生成，diplo 钩子日志生成 |
| 批量注入（附加模式） | `--attach notepad.exe` | `3 of 3` 成功，并正确标注三个都是 "was already loaded in the target" |
| 目标提前退出 | 注入一个开局即退出的目标 | 报 `the process had already exited with code ...`，不再是 `error 5` |
| `--probe` | `--attach notepad.exe --probe` | 在 DLL 所在目录建出 `diplo_action_hook_probe_only.txt` |
| 游戏目录默认解析 | 注入器放在游戏根目录，直接 `--list` | `game` = 同级 `stellaris.exe`，`mods` = 同级 `injected_mods`，列出 `diplo_action_hook.dll` |
| 远离游戏目录 | 在源码树外的目录里 `--list` | 经 Steam 注册表找到 `D:\SteamLibrary\...\stellaris.exe`，DLL 目录回落到游戏目录下的 `injected_mods` |
| 真实游戏（关键） | 注入器放游戏根目录、`injected_mods\diplo_action_hook.dll`，无参数运行 | 钩子在 t=1.0 s 装好；t≈63 s `registered keyword 'action_plnmg_test' -> token 66908`；`catch-up: scanned 69 action types`；`self-test: OK`；游戏存活 |
| 原始症状消失 | 同一局结束后的 `Documents\...\Stellaris\logs\error.log` | `Diplomatic action is missing token` 出现 **0** 次 |
| 中文路径 + 大写扩展名 | 目录 `...\Temp\注入测试\injected_mods\`，放 `probe_c.dll` 与 `UPPER.DLL` | 两个都列出并注入成功，两个标记文件都生成 |
| 游戏参数转发 | `--exe cmd.exe --new-instance -- //c md <新目录>` | 目录被创建，说明 `--` 后面的参数原样传给了被启动的程序 |
| `--delay` | `--delay 1500` | 打印 `waiting 1500 ms before injecting` |
| 参数错误 | 给一个不存在的参数 | 打印 `unknown argument: ...` 与完整用法，退出码 1 |
| 可重复编译 | 从本目录 `build.bat` 重新编译后与已发布的 exe 逐字节比较 | 734969 字节里只有 3 个字节不同（链接器写入的时间戳/Rich 头），代码段完全一致 |
| 改名后重跑 | 工具改名为 `stellaris_mod_injector.exe` 后重新编译，游戏根目录再跑一次 | 钩子 t=1.1 s 装好、`registered keyword` / `catch-up: scanned 69` / `self-test: OK` 全部如前，游戏存活 |
| **注入后自动关闭** | 无参数运行，stdin 指向一条 300 秒不关闭也不发数据的管道（若在等回车就会一直卡着） | 探针目标 **968 ms** 自行退出、退出码 0，探针 DLL 的加载标记已生成；游戏根目录同样 **982 ms** 退出（成功路径；出错时的行为见下一行） |
| **出错或跳过了 DLL 时不再闪退，并列出原因**（2026-09-26 改动） | 五种组合：成功/出错/只是跳过 × 有自己的控制台窗口/标准输入是管道。做法：`CreateProcess(..., CREATE_NEW_CONSOLE)` 起它，再用 `cmd /c "... > out.txt"` 在它自己那个控制台里跑一次，把屏幕内容读回来 | 出错+控制台 → 末尾把原因汇总重印一遍（`1 problem:`），再停在 `press Enter to close this window (exit code 1)`；**跳过 DLL+控制台 → 同样停下，标题写 `1 warning:`、退出码仍是 0**；跳过+管道 → **131 ms** 自行退出、退出码 0，输出里没有汇总；干净运行（无错无跳过）→ 控制台/管道两种情况都自行退出（**128 ms**，退出码 0）；出错+管道 → **133 ms** 自行退出、退出码 1。汇总覆盖：参数错、找不到游戏、目录里没有可注入 DLL、被跳过的 DLL、逐个 DLL 注入失败、目标提前退出、`--wait`/`ResumeThread` 失败 |

| **`Narrow()` 的越界写法**（2026-09-29 改动） | `std::string out(n - 1, '\0')` 之后又按 `n` 字节转换，最后一个 `'\0'` 落在 `data()[size()]` 上——C++11 起规定这个字节不得被修改（技术上 UB，各家实现上都还没出事）。改成先按 `n` 建串、转换完 `resize(n - 1)` | 行为不变（写进去的本来就是 `'\0'`）。改完重新编译：把这一处还原再编一份，还原版的 `.text` 与发布版 exe **逐字节相同**——说明发布版确实来自这份源码，这次改动也只动了这一处代码 |

出错时屏幕末尾实际长这样（一次批量注入里有 1 个 DLL 加载失败）：

```
injecting [1/2] bad_init.dll ... FAILED: error 1114 (动态链接库(DLL)初始化例程失败。)
injecting [2/2] diplo_action_hook.dll ... ok (module 00000000e6ad0000)

1 of 2 DLL(s) injected into pid 68848
failed:
  bad_init.dll: error 1114 (动态链接库(DLL)初始化例程失败。)

1 problem:
  * bad_init.dll: error 1114 (动态链接库(DLL)初始化例程失败。)

press Enter to close this window (exit code 1)
```

只是有 DLL 被跳过、其它都成功时（退出码 0）长这样：

```
1 of 1 DLL(s) injected into pid 65328

1 warning:
  * skipped broken.dll: file is too small to be a PE image

press Enter to close this window (exit code 0)
```

游戏内的完整日志见发行版的 `使用说明.md` 第 4 节。

---

## 5. 已知限制

| 限制 | 说明 |
| --- | --- |
| 依赖 `LoadLibraryW` 的地址跨进程一致 | 与 x64 Windows 上系统 DLL 共享基址的既有做法一致；同一个会话内成立 |
| 不解析 DLL 的依赖目录 | 被注入 DLL 的同目录兄弟依赖，靠"**同目录所有 DLL 都会被注入**"来满足，顺序按文件名；对顺序敏感的场景用 `--dll` 明确排前面 |
| 无签名/兼容性检查 | 只校验 PE 头与机器类型，不校验版本或签名；钩子本身都是失败即放弃 |
| 不保证宿主进程的 DLL 顺序 | 注入顺序确定，但每个 DLL 自己的初始化线程由它自己决定 |
