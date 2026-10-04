# stellaris 4.5 主程序导入的 DLL 清单

分析对象：`stellaris.exe`（x64，群星 4.5.1）。本机 Steam 版与 `D:\学习\stellaris_4.5.exe`
两份文件的导入表**逐项一致**（23 个模块、函数个数全同，仅文件大小差 3072 字节）。

数据来自三处，互相印证：

- **PE 导入表**：本机解析（`analysis\pe_dll_check.py`）；
- **本机 `\KnownDlls` 对象目录**（43 项，`analysis\known_dlls_enum.ps1`）——这是加载器
  实际使用的名单，注意**注册表键里看不到全部**（`bcrypt.dll`、`cfgmgr32.dll`、`shcore.dll`
  在对象目录里，但注册表视图没有）；
- **实机抢占测试**：把"只导出一个函数的假 DLL"和"按名字逐项导入的 stub 程序"放同一目录
  运行，看加载器加载的是哪一份（`analysis\hijack_test\`）。

## 0. 为什么"同名文件放游戏目录就能被加载"

- 该 exe 的导入**全部是按文件名**（无完整路径、无延迟导入、无绑定导入，清单里也没有任何
  DLL 重定向）；
- Windows 默认的 DLL 搜索顺序（本机 SafeDllSearchMode 未改）：
  **exe 所在目录 → System32 → 16 位系统目录 → Windows 目录 → 当前目录 → PATH**；
- 所以：名字**不在 KnownDLLs** 里 → 游戏根目录放同名 x64 DLL 就会被优先加载；
  名字**在 KnownDLLs** 里 → 加载器直接去 System32，放什么都被忽略。

## 1. 主表（按导入表顺序，也就是启动时的解析顺序）

| # | 模块 | 导入函数数 | 能否被游戏目录同名 DLL 顶替 | 备注 |
| ---: | --- | ---: | --- | --- |
| 0 | `SETUPAPI.dll` | 9 | ✗ 受保护 | KnownDLL |
| 1 | `steam_api64.dll` | 16 | ✓ | 游戏自带文件（顶替 = 替换游戏文件本身） |
| 2 | `PDXSDK.dll` | 146 | ✓ | 游戏自带文件 |
| 3 | `D3DCOMPILER_47.dll` | 1 | ✓ | 实测可从游戏目录加载 |
| 4 | `d3d11.dll` | 1 | ✓ | 实测 |
| 5 | `dxgi.dll` | 1 | ✓ | 实测 |
| 6 | `d3dx9_43.dll` | 7 | ✓ | 实测 |
| 7 | `d3d9.dll` | 2 | ✓ | 实测 |
| 8 | `OPENGL32.dll` | 44 | ✓ | 实测 |
| 9 | `KERNEL32.dll` | 184 | ✗ 受保护 | KnownDLL |
| 10 | `USER32.dll` | 123 | ✗ 受保护 | KnownDLL |
| 11 | `GDI32.dll` | 27 | ✗ 受保护 | KnownDLL |
| 12 | `SHELL32.dll` | 10 | ✗ 受保护 | KnownDLL |
| 13 | `ole32.dll` | 7 | ✗ 受保护 | KnownDLL |
| 14 | `OLEAUT32.dll` | 1 | ✗ 受保护 | KnownDLL；**唯一按序号导入**的（`ord#6`） |
| 15 | `ADVAPI32.dll` | 8 | ✗ 受保护 | KnownDLL |
| 16 | `bcrypt.dll` | 3 | ✗ 受保护 | KnownDLL，但**注册表里看不到**（实测被系统抢先，别用） |
| 17 | `WINMM.dll` | 21 | ✓ | 实测 |
| 18 | `WS2_32.dll` | 31 | ✗ 受保护 | KnownDLL |
| 19 | `SHLWAPI.dll` | 2 | ✗ 受保护 | KnownDLL |
| 20 | `IMM32.dll` | 10 | ✗ 受保护 | KnownDLL |
| 21 | `VERSION.dll` | 3 | ✓ | 实测 |
| 22 | `nakama-sdk.dll` | 1 | ✓ | 游戏自带文件 |

**小结**：23 个模块里，**11 个**可以被游戏目录里的同名 x64 文件顶替：

- **8 个系统 DLL**：`D3DCOMPILER_47.dll`、`d3d11.dll`、`dxgi.dll`、`d3dx9_43.dll`、
  `d3d9.dll`、`OPENGL32.dll`、`WINMM.dll`、`VERSION.dll`；
- **3 个游戏自带文件**：`steam_api64.dll`、`PDXSDK.dll`、`nakama-sdk.dll`
  （顶替它们等于替换游戏自己的文件，需要把原文件的导出全部转发）。

其余 12 个名字（`SETUPAPI`、`KERNEL32`、`USER32`、`GDI32`、`SHELL32`、`ole32`、
`OLEAUT32`、`ADVAPI32`、`bcrypt`、`WS2_32`、`SHLWAPI`、`IMM32`）都在 KnownDLLs 里，
放同名文件会被忽略。

## 2. 实测结果（本机 Windows 10 19045）

方法：假 DLL（每个只导出一个函数）+ 逐名导入的 stub exe，同目录运行，
再看 `GetModuleFileName` 指向游戏目录还是 System32。

| 结果 | 名字 |
| --- | --- |
| **从游戏目录加载成功（APP-DIR）** | `version` `dxgi` `d3d9` `d3d11` `d3dx9_43` `D3DCOMPILER_47` `opengl32` `winmm` `vulkan-1` `xinput1_4` `userenv` `dsound` `dbghelp` `dinput8` `wininet` `msvcp140` `vcruntime140` `vcruntime140_1` |
| **被系统拿走（受保护）** | `bcrypt` `cfgmgr32` `ws2_32` `imm32` `shell32` `shlwapi` `ole32` `setupapi` `shcore` |

`dinput8.dll` 的"成功"只说明**名字本身**可被顶替；游戏进程里没有任何模块加载它，
所以放进游戏目录也不会被加载（`winhttp.dll` 同理，这个 exe 根本不用）。

## 3. 附 A：写代理 DLL 时要转发的导出

（"谁导入它"以外，推荐把真 DLL 的全部导出都转发一遍；同一进程里别的模块也可能导入。）

| 名字 | 谁导入它 | 必须能提供的导出 |
| --- | --- | --- |
| `dxgi.dll` | exe | `CreateDXGIFactory1`（另：`d3d11.dll` 还导入 `CreateDXGIFactory2`） |
| `D3DCOMPILER_47.dll` | exe | `D3DCompile` |
| `d3d11.dll` | exe | `D3D11CreateDevice` |
| `d3d9.dll` | exe | `Direct3DCreate9`、`Direct3DCreate9Ex` |
| `d3dx9_43.dll` | exe | `D3DXSaveSurfaceToFileInMemory`、`D3DXLoadSurfaceFromSurface`、`D3DXCreateLine`、`D3DXCompileShader`、`D3DXCreateTexture`、`D3DXCreateCubeTexture`、`D3DXLoadSurfaceFromMemory` |
| `winmm.dll` | exe（SDL 也会在运行时加载） | `waveInAddBuffer`、`waveInStart`、`waveInReset`、`timeGetTime`、`waveOutGetDevCapsW`、`waveInUnprepareHeader`、`waveInPrepareHeader`、`waveInClose`、`waveInOpen`、`waveInGetDevCapsW`、`waveInGetNumDevs`、`timeBeginPeriod`、`timeEndPeriod`、`waveOutGetNumDevs`、`waveOutWrite`、`waveOutGetErrorTextW`、`waveOutOpen`、`waveOutClose`、`waveOutPrepareHeader`、`waveOutUnprepareHeader`、`waveOutReset` |
| `version.dll` | exe | `GetFileVersionInfoSizeA`、`GetFileVersionInfoA`、`VerQueryValueA` |
| `opengl32.dll` | exe | 44 个（`glReadPixels`、`glTexParameterf` … `wglGetProcAddress`，完整清单见 `analysis\exe_imports_full.txt`） |
| `steam_api64.dll` | exe | 16 个（`SteamAPI_Init`、`SteamAPI_RunCallbacks` …） |
| `PDXSDK.dll` | exe | 146 个（C++ 修饰名，完整清单见 `analysis\exe_imports_full.txt`） |
| `nakama-sdk.dll` | exe | `?createDefaultClient@Nakama@@…`（1 个） |
| ~~`bcrypt.dll`~~ | exe | `BCryptGenRandom`、`BCryptCloseAlgorithmProvider`、`BCryptOpenAlgorithmProvider`（名字受保护，列出来仅供参考） |

## 4. 附 B：不在导入表里、进程里同样会加载的名字

| 名字 | 谁带来 | 实测 |
| --- | --- | --- |
| `WININET.dll` | `PDXSDK.dll` 的静态依赖（启动第 3 个模块） | 可从游戏目录顶替 |
| `MSVCP140.dll` / `VCRUNTIME140.dll` / `VCRUNTIME140_1.dll` | `PDXSDK.dll`、`nakama-sdk.dll` 的依赖 | 同上（转发量 123 / 16 / 1 个 C++ 符号） |
| `vulkan-1.dll` | SDL（用 Vulkan 渲染器时） | 同上 |
| `xinput1_4.dll`（及 `xinput1_3/1_2/1_1/9_1_0.dll`） | SDL（手柄） | 同上 |
| `dsound.dll` | SDL（DSound 音频后端） | 同上 |
| `userenv.dll` | PhysicsFS（用户目录） | 同上 |
| `dbghelp.dll` | 崩溃处理器，但用**完整路径**找它（`…\Debugging Tools for Windows…`） | 名字可顶替，游戏不用裸名加载 |
| `shcore.dll` | SDL（DPI） | **受保护**（KnownDLL） |

## 5. 附 C：本机 64 位 `\KnownDlls` 名单（43 项）

```
kernel32.dll      ucrtbase.dll     MSCTF.dll        SHLWAPI.dll      WS2_32.dll
kernelbase.dll    wow64.dll        msvcp_win.dll    gdiplus.dll      user32.dll
bcrypt.dll        COMCTL32.dll     cfgmgr32.dll     IMM32.dll        combase.dll
rpcrt4.dll        ntdll.dll        bcryptPrimitives.dll              coml2.dll
win32u.dll        wow64cpu.dll     COMDLG32.dll     gdi32full.dll    IMAGEHLP.dll
SHELL32.dll       sechost.dll      WINTRUST.dll     NORMALIZ.dll     difxapi.dll
Setupapi.dll      CRYPT32.dll      gdi32.dll        MSVCRT.dll       wow64win.dll
advapi32.dll      PSAPI.DLL        NSI.dll          OLEAUT32.dll     WLDAP32.dll
SHCORE.dll        ole32.dll        clbcatq.dll      （另有 KnownDllPath：路径项，不是 DLL）
```

判定方法：`analysis\known_dlls_enum.ps1`（`NtOpenDirectoryObject` + `NtQueryDirectoryObject`
枚举对象管理器里的 `\KnownDlls`）。**不要只看注册表**
`HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs`——注册表视图不含
`bcrypt.dll`、`cfgmgr32.dll`、`shcore.dll` 等，照它判断会得出错误结论。
