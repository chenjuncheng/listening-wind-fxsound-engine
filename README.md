# Listening Wind FxSound Engine

这是听风者项目使用的 FxSound 音频引擎适配层。它把 FxSound 的 DSP 与 AudioPassthru 能力封装为独立命令行程序，供上层客户端启动、加载 FAC 预设、选择输出设备、安装/卸载虚拟声卡驱动和恢复系统默认音频设备。

## 开源说明

- 本目录代码基于 FxSound AGPL-3.0 源码进行适配，许可证继承为 AGPL-3.0-only。
- 如果分发包含本引擎的二进制程序，需要同时向用户提供对应源码和修改说明。
- 当前目录不是完整 FxSound 上游源码镜像；构建时需要同级目录 `../fxsound-app` 提供 FxSound 原始源码。
- 不要提交本机编译产物、驱动二进制、日志、缓存或本机路径配置。

## 目录结构

```text
fxsound-engine/
  CMakeLists.txt          CMake 构建入口
  src/                    听风者引擎适配代码
  overrides/              编译覆盖头文件
  docs/                   集成说明和产品/技术文档
  build/                  本机生成目录，不提交
```

依赖目录：

```text
listening_wind/
  fxsound-app/            FxSound AGPL-3.0 原始源码
  fxsound-engine/         当前仓库目录
```

## 构建要求

- Windows 10/11 x64
- Visual Studio 2022 Build Tools，包含 MSVC C++ 工具链
- CMake 3.20+
- Ninja，或 Visual Studio CMake 生成器
- 同级 `../fxsound-app` 源码目录

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

- `CMakeLists.txt` 使用相对路径 `../fxsound-app`，不要写死本机绝对路径。
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
