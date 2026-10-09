# stellaris_mod_injector_dll_src

`stellaris_mod_injector.exe` 的**代理 DLL 版**源码：把"启动游戏后注入 injected_mods"
搬进游戏进程内部 —— 靠顶替一个游戏启动时一定会加载的系统 DLL（`dxgi.dll` / `winmm.dll` /
`version.dll` / `d3d11.dll` / `d3dcompiler_47.dll` 五种），加载器就在游戏进程里跑，
不需要外部工具。

（初版由deepseek-v4.1-flash编写，harness为kimi code）
（新版由gpt-6.1-sol编写，harness为codex）

---

## 1. 目录

```
stellaris_mod_injector_dll_src/
├── build.bat                     一键编译五种，产物写到 ..\stellaris_mod_injector_dll\
├── README.md                     本文件
├── stellaris_mod_injector.ini     全默认配置模板（delay_ms=700、probe=0）
├── src/
│   ├── dllmain.cpp               DllMain（只登记 + 查标志位）+ 延迟启动 + 加载流程
│   ├── crtprobe.h / crtprobe.cpp 在主 exe 里找"游戏 CRT 是否已经初始化"的标志位
│   ├── mods.h / mods.cpp         injected_mods 扫描、PE 检查、加载、probe 标记
│   ├── config.h / config.cpp     默认 ini 自动创建、读取（delay_ms、probe）
│   ├── log.h / log.cpp           覆盖式日志（每次启动重写）+ OutputDebugString
│   └── util.h / util.cpp         路径、UTF-8、Win32 错误文本
├── def/                          每种名字的导出清单（生成物，别手改，见第 4 节）
│   ├── dxgi.def  d3d11.def  version.def  winmm.def  d3dcompiler_47.def
└── tools/
    ├── gen_proxy_def.py          从本机 System32 生成 def\<name>.def
    ├── run_tests.py              编译 + 自动检查（导出表、宿主桩、扫描层级、加载流程、ini、负例）
    ├── host_stub.cpp             测试宿主：导入游戏实际导入的那些函数，起一条线程并跑消息循环
    ├── test_mod.cpp              测试 mod：记录自己的 DllMain 跑了几次
    ├── check_crt_probe.cpp       拿一个真实的游戏 exe 验证 crtprobe 还能找到标志位
    └── suspended_start_check.cpp 复现启动器的启动方式（进程挂起 + 由别的线程走导入表）
├── build_out/                    测试编译的产物（run_tests.py 生成，可删）
└── test_out/                     测试宿主与各用例目录（同上，可删）
```

## 2. 工作原理

1. **冒充**：每种构建都链接 `def/<name>.def`。这个 def 里列着同名系统 DLL 的**全部
   命名导出和它们的原始序号**，每个都是转发器（forwarder），指向
   `C:/Windows/System32/<name>.<Func>`。所以进程里任何模块从 `dxgi.dll` 等名字上
   拿到的东西和真文件一一对应，连序号都对得上（`tools/run_tests.py` 会逐个核对）。
   转发用正斜杠是因为 GNU ld 的 .def 词法把 `\` 当转义（反斜杠写法会编译失败）。
2. **DllMain**：只记句柄、查一次标志位、登记一个回调定时器，然后立刻返回。
   它在 loader lock 里、而且是在游戏自己的导入还没解析完的时候被调用，
   所以这里不能建线程（1.1 起也不再建，原因见下），也不能干别的。
3. **延迟启动（1.1）**：`src/crtprobe.cpp` 在主 exe 里找那个"-1 表示还没分配"的
   TLS 索引变量 —— 它的值是 `-1` 时，游戏自己的 CRT 还处在启动过程中，
   这时候任何新线程都会被游戏的 TLS 回调送进 `abort()`（0xC0000409 / BEX64）。
   所以线程只在两处创建：
   - `DllMain(DLL_THREAD_ATTACH)`：游戏自己起了一条新线程，且标志位说 CRT 好了；
   - 回调定时器：加载线程进入消息循环（游戏自己的主循环），且标志位说 CRT 好了。

   两条路都晚于游戏自己的启动。真要是都等不到（30 秒），就在当时那条**已经初始化**
   的线程上直接加载（`start : the CRT stayed unready for ...`）——建线程才危险，
   在已有线程上跑不危险。找不到标志位（别的宿主、别的 CRT）时退回按时间等
   （`kNoProbeSafeMs` = 700 ms）那一套。
4. 线程起来之后：检查游戏根目录的 `injected_mods`，不存在时先新建这个目录，
   然后打开 `injected_mods\stellaris_mod_injector.log`，检查
   `injected_mods\stellaris_mod_injector.ini`：缺失时新建全默认配置，已有文件保留，
   再读取配置。等满 `delay_ms`（默认 700，与注入器
   `--delay 700` 同值，起点是进程
   创建时刻）→ 取一个按 pid 命名的互斥量（玩家装多个名字时只有一个负责加载，
   其余会看到 "was already loaded in the process"）→ 只扫 `injected_mods` 的**一层子目录**
   中的 DLL，例如 `injected_mods\my_mod\hook.dll`；忽略 `injected_mods\hook.dll`，
   也不进入 `injected_mods\my_mod\deeper\`。汇总后按**文件名顺序**（同名时按完整路径，
   保证动态关键字注册顺序稳定）→ 检查每个 DLL 同目录是否有对应的
   `<DLL文件名>noinject` 文件（例如 `hook.dllnoinject`），有则跳过该 DLL → 与注入器**同一套**
   PE 检查（是 DLL、x64），坏文件跳过并写明原因 → `LoadLibraryW` 全路径逐个加载，
   每个都写一行 `ok (module ...)` 或 `FAILED: ...` → 汇总 `N of M DLL(s) loaded`。
   日志写在**游戏根目录下**的 `injected_mods\stellaris_mod_injector.log`，
   打开日志前已经完成目录检查和创建。旧文件可能仍在游戏根目录或 `injected_mods\`
   里，新版不会使用它们。**每次启动覆盖**，文件里只有这一次
   运行：玩家看的永远是刚才那次，昨天的失败不会再混进来。各个 mod 自己的日志
   仍写在它们自己旁边（那些是追加的）。
   **禁用单个 DLL**：在 `injected_mods\my_mod\hook.dll` 旁放一个空文件
   `hook.dllnoinject`。标记内容不限，只影响同目录同名 DLL；同名目录不算标记。
   原来的 `hook.dllnoload` 不再识别。
   检查在打开 DLL、PE 检查和 probe 标记处理之前完成，日志会写
   `[skip] hook.dll: disabled by hook.dllnoinject`。删除标记后，下次启动恢复加载；
   不会卸载已经加载的 DLL，也不能阻止其他模块自行加载它。

5. 配置放在 `injected_mods\stellaris_mod_injector.ini`，旧名
   `stellaris_mod_loader.ini` 不再读取。当前所有 ini 参数及默认值为：

   ```ini
   delay_ms=700
   probe=0
   ```

   `delay_ms` 表示从进程创建到加载 mod 至少等待的毫秒数，有效范围为 1～599999。
   `probe=0` 默认关闭探测模式，`probe=1` 开启（= 注入器的 `--probe`）。开启时，
   在每个可加载 DLL 旁生成 `diplo_action_hook_probe_only.txt`，支持这个标记的 mod
   只解析地址、不安装钩子。关闭时，在加载 DLL 之前删除它旁边已有的这个标记，
   让上次开启 probe 留下的文件不再生效。
   `injected_mods\stellaris_mod_loader_probe.txt` 不再用于控制探测模式。
   缺少参数或参数值无效时使用默认值；`probe` 只接受 `0` 或 `1`。
   编译会附带全默认 ini。游戏运行时先检查并尝试创建缺失的 `injected_mods`，
   随后为缺失的 ini 创建默认配置，不改写已有 ini。

### 为什么 DllMain 里不再创建线程（1.1）

1.0 在 DllMain 里立刻 `CreateThread`。当游戏是**直接**启动时，主线程几乎总能先跑到
CRT 初始化，所以看不出来；但**从启动器启动**时不是这样：Paradox Launcher 拉起的
进程被创建成挂起状态，`gameoverlayrenderer64.dll`（Steam 覆盖层）挂进去的线程
替游戏走完导入表 —— 我们那条线程这时也被创建了，而游戏的主线程还没被恢复，
CRT 初始化根本没机会跑。于是我们线程的 TLS 回调先执行，发现 CRT 的索引还是 `-1`，
按 CRT 自己的规矩 `abort()`。本机两份崩溃转储里出错线程的起始地址都是
`dxgi.dll+0x14b0`（加载器线程函数），这是定位的决定性证据。

`tools/suspended_start_check.cpp` 就是把这条启动方式复现成一个工具：进程挂起、
在它里面起一条线程（让导入表在那条线程上走完），然后看进程还在不在。
1.0 的产物在这个工具下于主线程被恢复之前就死了，1.1 的产物活着。
（工具恢复主线程之后的访问违例不算数：那一步**没有任何代理**时也会发生，
是"导入表由别的线程走完、那条线程又走了"这件事本身带来的，工具的注释里写了。）

### 为什么没有"等加载器空闲"的探测

`LoadLibraryW` 自己会阻塞在 loader lock 上：如果游戏镜像还没初始化完，加载会自然推迟到
初始化结束，不需要（也不应该）额外探测。这不是理论 —— 试过并测出问题：

本机实验（Windows 10 19045，宿主 = 只导入 dxgi 的最小 exe，命名为 `stellaris.exe`，
代理用真的 def，只换 DllMain 的行为）：

| 变体 | 结果 |
| --- | --- |
| 纯转发，DllMain 不起线程 | 正常 |
| DllMain 起线程，只写日志 | 直接启动正常；**挂起启动崩溃**（见上） |
| 上面 + `LdrLockLoaderLock(TRY_ONLY)` 轮询探测 | **卡死**：宿主停在 `CreateDXGIFactory1`，探测一直报"锁忙" |
| 上面改为延迟后 `LoadLibraryW`（不探测） | 正常 |

也就是说，从"进程初始化期间创建的线程"里去 TRY_ONLY 探测加载器锁，会把加载器锁搞成
永远拿不到的状态。所以最终版**只保留 delay + 让 LoadLibrary 自己阻塞 + 等 CRT 标志位**，
并在 `src/dllmain.cpp`、`src/crtprobe.h` 里写明了原因。

## 3. 编译

```
build.bat
```

需要 MinGW-w64 的 `g++` 在 PATH 里，或设置 `MINGW_BIN`。产物（五种 DLL）写到
`..\stellaris_mod_injector_dll\`，同时将默认模板复制到产物目录下的
`injected_mods\stellaris_mod_injector.ini`（每次编译覆盖产物目录中的这份配置）。
安装时将选用的代理 DLL 放在游戏根目录，并将附带的 `injected_mods` 目录合并到
游戏根目录；mod DLL 放进 `injected_mods` 的一层子目录中。全部静态链接：产物的导入表只有 `KERNEL32.dll` 和
UCRT 的 api-ms 集（`ucrtbase.dll`，Windows 10 自带），不依赖 MinGW 运行库。

设 `OUTDIR` 可以编译到别处（`tools\run_tests.py` 就是这么做的，所以跑测试不会
覆盖已经实机验证过的产物）：

```
set OUTDIR=D:\tmp\proxy && build.bat
```

## 4. 重新生成导出清单

System32 里的这些 DLL 换了版本（或系统目录不是 `C:\Windows`）时：

```
py -3 tools\gen_proxy_def.py
```

它读每个系统 DLL 的导出表，写出 `def\<name>.def`（名字 + 原序号 + 转发目标），
并报告"只有序号、没有名字"的导出（这些**无法**转发，会跳过；本机 winmm 有 1 个，
游戏不按序号用它）。

## 5. 测试

```
py -3 tools\run_tests.py            （编译到 build_out\，再跑全部检查）
py -3 tools\run_tests.py --no-build （只跑检查，用 build_out\ 里现成的产物）
py -3 tools\run_tests.py --out ..\stellaris_mod_injector_dll --no-build
                                    （检查已经发布的那些 DLL）
```

测试自己编译到 `build_out\`，`build.bat` 的产物（`..\stellaris_mod_injector_dll\`）
不会被碰；两边都存在时会比对 `.text` 段，并注明"发布的 DLL 就是刚测过的这份代码"
还是"和刚测的这份不是同一份代码"。

自动检查全部离线（不碰游戏）：

1. **导出表核对**（每种名字）：名字集合一致、无多余名字、序号一致、每个导出都转发回
   System32 的真文件；
2. **宿主桩**：一个导入了"游戏实际导入的那些函数"的 exe，真调用 `GetFileVersionInfoSizeA`
   （必须拿到真实大小）、`timeGetTime`、`CreateDXGIFactory1`、`D3D11CreateDevice`，
   并用 `D3DCompile` 真编译一段 HLSL（编译失败即判失败）；
   转发器解析不了的话它在启动阶段就会失败；
3. **加载流程**（五种名字各一遍）：测试 mod 被加载且 DllMain 只跑一次、32 位 DLL / 非 PE
   文件 / MZ 合法但头偏移坏掉的文件被跳过并写明原因、mod 子目录里叫 `*.dll` 的**目录**
   不算候选、loader 自己那份副本被跳过、ini 延迟生效、日志与汇总行正确，
   且日志写在 `injected_mods\stellaris_mod_injector.log`、不在游戏根目录生成日志；
   另对五种代理分别验证只加载一层子目录的 DLL、忽略根层及更深层 DLL，
   多个子目录一起扫描、大小写扩展名识别和文件名排序正确；
   另外每种名字都要求 `start :` 行说加载线程是**被消息循环叫醒**的（而不是在挂接时
   就创建），并且叫醒时刻不早于 ini 里的 `delay_ms`；
4. **目录和配置**：五种代理及三个代理同时装时，缺 `injected_mods` 会先创建目录、
   再写日志并补齐默认 ini；已有目录但没有 ini 时同样自动生成，已有配置保留；
   同名路径被文件占用时保留文件且宿主继续运行；运行时生成的默认 ini 与编译附带
   的 ini 一致（700 ms、probe 关闭），新 ini 存在时旧名配置不参与读取；
   **负例**：宿主不是 `stellaris.exe` 时拒绝加载、三个代理同时装时只加载一遍、
   `probe=1` 在 mod 的 DllMain 运行前生成标记，切回 `probe=0` 在加载前清理标记，
   旧的 loader probe 标记不再生效；
5. **启动器那种启动方式**：宿主不跑消息循环、只在 1.2 秒后起一条线程 —— 这时
   只有 `DLL_THREAD_ATTACH` 能叫醒代理，要求日志写出
   `start : a thread attached after the CRT was up` 且 mod 照常加载；
6. **ini 编码**（6 种）：LF / CRLF / 带 BOM 的 UTF-8 / UTF-16LE / UTF-16BE / 带注释与
   空格，都要求 `delay_ms` 和 `probe` 真的生效（2026-09-29 之前，带 BOM 的文件会被静默忽略）；
7. **ini 里不能用的值**：越界值、拼错的键、没有 `=` 的行，各自要求在日志里写明被忽略、
   并说明用的是哪个值；
8. **等满 delay_ms**：一个 `delay_ms=1500` 的用例，要求日志出现 `waiting ... ms`，
   证明 worker 仍然按玩家配置等待。

### 另外两个不进自动测试的工具

游戏相关的这两件事没法离线做，各配了一个小工具（都在 `tools\` 下，都可以单独编译）：

- `check_crt_probe.cpp` —— 拿一个真实的游戏 exe，验证 `src/crtprobe.cpp` 还能找到
  那个 CRT 标志位（`found at rva 0x...`；找不到不影响使用，只是退回按时间等的路径）。
  用法：

  ```
  g++ -std=c++17 -O2 -o check_crt_probe.exe tools\check_crt_probe.cpp src\crtprobe.cpp
  check_crt_probe.exe "D:\SteamLibrary\steamapps\common\Stellaris\stellaris.exe"
  ```

- `suspended_start_check.cpp` —— 复现启动器的启动方式（进程挂起，导入表由另一条线程
  走完），看代理会不会在那之前就创建线程而把游戏弄死。判定看
  `process still alive after N s` 这一行；工具里也写明了恢复主线程之后的访问违例
  与代理无关（没有任何代理时同样发生）。用法：

  ```
  g++ -std=c++17 -O2 -o suspended_start_check.exe tools\suspended_start_check.cpp
  suspended_start_check.exe "D:\SteamLibrary\steamapps\common\Stellaris\stellaris.exe" 8
  ```

### 实机验证

真游戏 4.5.1（本机 Steam 版）+ `injected_mods` 里的 3 个 mod
（`achievement_unlocker.dll` / `diplo_action_hook.dll` / `sound_ogg_hook.dll`）：

- **1.0**：`dxgi.dll` 与 `d3d11.dll` 各跑一遍，两种都是**直接**启动：
  代理在 704 / 641 ms 内把 3 个 DLL 全部加载，`diplo_action_hook` 在 1.8 s 装好钩子，
  60 s 数据加载完后自检全过。原始日志：`..\stellaris_mod_injector_dll\验证日志_实机_loader.log`。
  **从启动器启动时同一份代码必定崩**（两份 0xc0000409 崩溃转储，出错线程是加载器自己的
  线程函数 `dxgi.dll+0x14b0`；Steam 日志显示那两次是 `dowser.exe` 拉起的游戏）。
- **1.1**：`dxgi.dll` 直接启动跑通（`验证日志_实机_1.1_延迟启动.log`）：
  `crt   : ... ready`、`start : a thread attached after the CRT was up (650 ms)`、
  满 700 ms 后加载 3 个 mod，游戏正常到主菜单、三个 mod 的日志都正常
  （diplo 的自检全过、拿到新 token）。
  启动器那条路用 `suspended_start_check.cpp` 验证：1.0 的产物在导入表走完之前就死，
  1.1 的产物活着（`process still alive after 8 s`）。

### 没有验证的部分

- `version.dll` / `winmm.dll` 没有在实机上装载过：这两个名字在本机游戏目录里被整合版的
  Steam 模拟器占着，装上去会顶掉它，所以只做了离线验证（导出表 + 宿主桩 + 加载流程）；
- `d3dcompiler_47.dll` 也还没有在实机上装载过：它的离线验证全过了（导出表 29 个名字、
  宿主桩真编译一段 HLSL、加载流程按名字各跑一遍），导入表分析与抢占测试里它排在第 4 个、
  可以从游戏目录顶替，但还没有在真游戏里装载过一遍；
- 真实启动器（Paradox Launcher 点 Play）下的 1.1 端到端跑一遍还没做：那条路只能用
  `suspended_start_check.cpp` 复现到"导入表走完时进程还活着"这一步（见上），
  恢复主线程之后这一步是工具自己的构造限制，换真正启动器不会有；
- 多人游戏（动态 token 的联机一致性是 diplo 钩子自己的问题，与加载方式无关）；
- Windows 11 与"系统目录不是 `C:\Windows`"的机器（转发目标写死在 def 里）；
- 其它版本的游戏（代理机制与版本无关，但"哪个名字、多早加载"、以及 CRT 标志位的
  形状是按 4.5.1 核的；换了 CRT 就用 `check_crt_probe.cpp` 重新确认）。

## 6. 与注入器的行为差异

| 注入器（外部） | 本 DLL 版（进程内） |
| --- | --- |
| `CreateProcess` 后等 700 ms，再远程 `LoadLibraryW` | 进程创建后等 700 ms，进程内 `LoadLibraryW` |
| `--delay` / `--probe` / `--attach` / `--wait` / `--new-instance` / `--list` | ini `delay_ms` / ini `probe` / 不适用 / 不适用 / 不适用 / 日志里的 `[skip]`+`loading` 行 |
| 注入失败会打印错误并保留窗口 | 写日志，游戏继续跑 |
| 需要玩家每次双击 | 随游戏启动自动生效 |

两者可以共存：`LoadLibrary` 对已加载模块返回旧句柄，`DllMain` 不会跑第二次。
