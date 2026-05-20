# Listening Wind FxSound Engine

这是听风者项目使用的 FxSound 音频引擎适配层。它把 FxSound 的 DSP 与 AudioPassthru 能力封装为独立命令行程序，供上层客户端启动、加载 FAC 预设、选择输出设备、安装/卸载虚拟声卡驱动和恢复系统默认音频设备。

## 开源说明

- 本目录代码基于 FxSound AGPL-3.0 源码进行适配，许可证继承为 AGPL-3.0-only。
- 如果分发包含本引擎的二进制程序，需要同时向用户提供对应源码和修改说明。
- 当前仓库已带上构建所需的 `fxsound-app` 源码；如果本地另有同级目录 `../fxsound-app`，CMake 也可以兼容使用。
- 不要提交本机编译产物、驱动二进制、日志、缓存或本机路径配置。

## 目录结构

```text
fxsound-engine/
  CMakeLists.txt          CMake 构建入口
  src/                    听风者引擎适配代码
  overrides/              编译覆盖头文件
  docs/                   集成说明和产品/技术文档
  fxsound-app/            FxSound AGPL-3.0 上游源码
  build/                  本机生成目录，不提交
```

## 构建要求

- Windows 10/11 x64
- Visual Studio 2022 Build Tools，包含 MSVC C++ 工具链
- CMake 3.20+
- Ninja，或 Visual Studio CMake 生成器
- 仓库内置 `fxsound-app` 源码目录，或通过 `-DFXSOUND_ROOT=<path>` 指定 FxSound 源码目录

## 构建命令

在 `fxsound-engine` 目录执行：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target fxsound_engine
```

如果使用 Visual Studio 生成器：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target fxsound_engine
```

输出程序通常位于：

```text
build/Release/fxsound_engine.exe
```

## 常用命令

查看帮助：

```powershell
.\build\Release\fxsound_engine.exe --help
```

列出真实输出设备：

```powershell
.\build\Release\fxsound_engine.exe --list-devices
```

安装驱动：

```powershell
.\build\Release\fxsound_engine.exe --install-driver
```

卸载驱动：

```powershell
.\build\Release\fxsound_engine.exe --uninstall-driver
```

恢复系统默认输出设备：

```powershell
.\build\Release\fxsound_engine.exe --restore-default
```

启动引擎并加载 FAC：

```powershell
.\build\Release\fxsound_engine.exe --preset "path\to\preset.fac"
```

## 研发注意事项

- `CMakeLists.txt` 优先使用仓库内置 `fxsound-app`，也支持通过 `-DFXSOUND_ROOT=<path>` 指定外部 FxSound 源码；不要写死本机绝对路径。
- 中文路径下构建和运行必须保持兼容，新增脚本不要依赖固定盘符。
- 修改驱动安装/卸载逻辑前，必须验证管理员权限、外部 FxSound 冲突、卸载所有权和系统默认设备恢复。
- 修改设备枚举逻辑前，必须确认不会把 FxSound 虚拟设备暴露为可选输出设备。
- 修改启动参数解析时，注意 PowerShell `-Command` 对非 ASCII 路径的解析风险。

## 发布前检查

```powershell
cmake --build build --config Release --target fxsound_engine
.\build\Release\fxsound_engine.exe --list-devices
.\build\Release\fxsound_engine.exe --help
```

如果要随听风者客户端发布，请使用上层项目的 release 脚本统一打包，不要手工拼 release 目录。

## 发布 GitHub Release

完整 Windows 客户端 release 依赖上层 Flutter app 构建产物，因此当前仓库不使用 GitHub Actions 编译完整客户端。研发在本机执行上层构建脚本生成 `release/` 后，使用本仓库脚本发布 GitHub Release：

```powershell
.\scripts\publish_release.ps1
```

默认读取仓库同级目录的 `..\release`。如果 release 目录在其他位置：

```powershell
.\scripts\publish_release.ps1 -ReleaseDir "D:\path\to\release"
```

预检查但不创建 Release：

```powershell
.\scripts\publish_release.ps1 -DryRun
```

指定 tag：

```powershell
.\scripts\publish_release.ps1 -Tag "v2026.05.20-build2"
```

脚本会：

- 校验 `listening_wind_app.exe`、`engine\fxsound_engine.exe`、FAC、驱动和卸载脚本是否存在。
- 排除 `*.log`、`*.dmp`、`crash_logs/`。
- 生成 `out\listening_wind_release_<tag>.zip`。
- 如果 Release 不存在则创建；如果已存在则覆盖上传同名 zip。

## 下载免编译包

如果只需要运行程序，请从 GitHub Releases 下载打包好的 release 压缩包。该包包含客户端 exe、引擎 exe、FAC 预设、驱动文件和辅助脚本。

源码仓库不直接提交 release 二进制，避免每次构建都污染 Git 历史。
