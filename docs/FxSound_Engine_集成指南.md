# FxSound Engine v2.0 集成指南

> **版本**：v2.0 | **更新**：2026-04-23 | **许可证**：AGPL-3.0

---

## 1. 项目概述

FxSound Engine 从开源音频增强软件 FxSound 改造而来，是**无头命令行音频处理引擎**。剥离 GUI、注册表依赖和外部工具链，保留核心 DSP 处理能力和 WASAPI 管线，通过进程隔离模型对外提供服务。

**核心定位**：被前端应用通过 `Process.start()` 启动，stdout JSON 通信，进程终止完成清理。

| 特性 | FxSound 原版 | FxSound Engine |
|------|-------------|----------------|
| 界面 | GUI (Win32/WPF) | 无（命令行） |
| 通信方式 | 共享内存/窗口消息 | stdout JSON |
| 注册表依赖 | 必需 | 移除（NO_REGISTRY） |
| 驱动安装 | 外部 fxdevcon.exe | 内嵌 SetupDi API |
| 错误处理 | MessageBox/DebugBreak | stderr 输出 |
| 部署体积 | ~30MB | ~537KB |
| 许可证 | AGPL-3.0 | AGPL-3.0（进程隔离有利合规） |

---

## 2. 系统架构

### 2.1 音频管线

```
游戏音频输出
    │
    ▼
┌──────────────────────┐
│  FxSound Audio       │  ← 系统默认播放设备
│  Enhancer (虚拟设备)  │    fxvad.sys
└──────────┬───────────┘
           │ WASAPI Loopback Capture
           ▼
┌──────────────────────┐
│  DfxDsp 音频增强      │  EQ(10段) + Fidelity + Surround
│  - 均衡器 / 环绕声    │  + Ambience + DynamicBoost + Bass
│  - 动态压缩 / 低音    │  + 耳机模式 / 音乐模式
└──────────┬───────────┘
           │ WASAPI Render
           ▼
┌──────────────────────┐
│  真实播放设备          │  ← 用户耳机/扬声器
└──────────────────────┘
```

### 2.2 进程生命周期

```
Flutter App                     fxsound_engine.exe
    │                                │
    │── Process.start() ───────────>│ ① 启动进程
    │                                │ ② 解析参数 + 检查驱动
    │<── {"type":"startup",...} ────│ ③ 输出启动信息
    │                                │ ④ 保存当前默认设备
    │<── {"type":"device_saved"} ───│
    │                                │ ⑤ 加载预设 + 初始化管线
    │<── {"type":"ready"} ──────────│ ⑥ 就绪
    │                                │
    │    (游戏运行，音频持续处理)      │ ⑦ processTimer() 循环
    │<── {"type":"device_change"} ──│ ⑧ 设备变化通知
    │                                │
    │── kill(SIGINT) ─────────────>│ ⑨ 终止（优先 SIGINT）
    │                                │ ⑩ 恢复默认设备
    │<── {"type":"shutdown"} ───────│ ⑪ 输出关闭状态
```

### 2.3 关键设计决策

| 决策 | 方案 | 理由 |
|------|------|------|
| 进程隔离 vs DLL | Process.start() | 避免 FFI 复杂性；AGPL 合规更有利 |
| 虚拟设备 vs 同设备Loopback | fxvad.sys | 消除正反馈杂音 |
| JSON vs IPC | stdout JSON | 跨语言解析简单 |
| 恢复文件 vs 仅信号 | .fxsound_recovery | Process.kill()/TerminateProcess() 无法拦截，需持久化兜底 |
| 注册表 | NO_REGISTRY 宏移除 | 免安装、便携部署 |

---

## 3. 命令行参数完整参考

### 3.1 语法

```bash
fxsound_engine.exe --preset <路径> [选项]     # 音频处理模式
fxsound_engine.exe --install-driver [目录]     # 安装驱动（管理员）
fxsound_engine.exe --uninstall-driver          # 卸载驱动（管理员）
fxsound_engine.exe --check-driver              # 检查驱动
fxsound_engine.exe --list-devices              # 列出设备
fxsound_engine.exe --restore-default           # 恢复默认设备
fxsound_engine.exe --help                      # 帮助
```

### 3.2 音频处理参数

| 参数 | 缩写 | 类型 | 必需 | 默认值 | 说明 |
|------|------|------|------|--------|------|
| `--preset <path>` | `-p` | 路径 | **是** | — | .fac 预设文件路径 |
| `--device <name>` | `-d` | 字符串 | 否 | 自动 | 目标播放设备名称（子串匹配） |
| `--device-index <n>` | — | 整数 | 否 | 自动 | 设备索引（对应 --list-devices 编号） |
| `--fidelity <0-10>` | `-f` | 浮点 | 否 | 预设值 | 保真度，0=关 10=最大 |
| `--ambience <0-10>` | `-a` | 浮点 | 否 | 预设值 | 氛围感 |
| `--surround <0-10>` | `-s` | 浮点 | 否 | 预设值 | 环绕声 |
| `--dynamic-boost <0-10>` | `-b` | 浮点 | 否 | 预设值 | 动态增强 |
| `--bass <0-10>` | — | 浮点 | 否 | 预设值 | 低音增强 |
| `--buffer <ms>` | — | 整数 | 否 | 40 | 缓冲区大小（毫秒），越小延迟越低 |
| `--analyze` | — | 布尔 | 否 | false | 仅捕获模式（静音，调试用） |

### 3.3 驱动管理参数

| 参数 | 说明 |
|------|------|
| `--install-driver [dir]` | 安装 fxvad.sys。需要**管理员权限**。可选指定驱动目录 |
| `--uninstall-driver` | 卸载虚拟设备。需要**管理员权限** |
| `--check-driver` | 检查驱动是否已安装且启用 |
| `--restore-default` | 从恢复文件还原默认播放设备 |

### 3.4 使用示例

```bash
# 基础：使用预设启动
fxsound_engine.exe --preset pubg.fac

# 指定播放设备
fxsound_engine.exe --preset pubg.fac --device "HyperX"

# 覆盖预设参数
fxsound_engine.exe --preset pubg.fac --bass 0 --surround 10

# 减少延迟（竞技场景）
fxsound_engine.exe --preset pubg.fac --buffer 20

# 安装驱动
fxsound_engine.exe --install-driver

# 列出设备
fxsound_engine.exe --list-devices
```

---

## 4. JSON 通信协议

### 4.1 规则

- **stdout**：状态消息，每行一个 JSON 对象
- **stderr**：日志信息，格式 `[FxSound Engine] ...`
- 前端逐行读取 stdout 解析 JSON

### 4.2 消息类型

#### startup — 启动通知
```json
{"type": "startup", "version": "2.0.0", "pid": 12345, "mode": "virtual_device"}
```

#### device_saved — 原始设备保存
```json
{"type": "device_saved", "original_device": "HyperX Cloud II"}
```

#### crash_recovery — 崩溃恢复
```json
{"type": "crash_recovery", "status": "restored"}
{"type": "crash_recovery", "status": "skipped", "reason": "dfx_device"}
```

#### ready — 就绪
```json
{"type": "ready", "status": "processing", "mode": "virtual_device"}
```

#### settings — 当前效果参数
```json
{"type": "settings", "fidelity": 9.1, "ambience": 1.2, "surround": 8.3, "dynamic_boost": 5.1, "bass": 2.0}
```
> 浮点范围 0-10，由 FAC 预设值(0-127)映射而来

#### device_change — 设备变化（1秒防抖）
```json
{"type": "device_change"}
{"type": "device_change", "devices": 5}
```

#### error — 错误
```json
{"type": "error", "code": "driver_not_found", "message": "..."}
```

#### shutdown — 关闭
```json
{"type": "shutdown", "status": "ok", "device_restored": true, "restored_device": "HyperX Cloud II"}
```

#### 驱动管理
```json
{"type": "driver_install", "result": "OK", "message": "Driver installed successfully."}
{"type": "driver_uninstall", "result": "OK", "message": "..."}
{"type": "driver_status", "result": "OK"}
```

#### 设备恢复
```json
{"type": "restore_default", "status": "ok", "device": "HyperX Cloud II"}
{"type": "restore_default", "status": "no_recovery_file"}
```

DriverResult 枚举：`OK` | `Reboot required` | `Failed` | `Already installed` | `Not installed` | `Administrator required` | `INF not found` | `Architecture unsupported`

---

## 5. Flutter 产品集成指南

### 5.1 集成架构

Flutter 应用作为前端，通过 `Process.start()` 启动引擎子进程，职责划分如下：

| 层 | 负责方 | 职责 |
|---|---|---|
| UI 交互 | Flutter | 预设选择、效果滑块、设备选择、开关按钮 |
| 驱动管理 | Flutter (管理员提权) | 首次安装驱动、检查驱动状态 |
| 音频处理 | fxsound_engine.exe | 加载预设、音频管线、效果处理 |
| 环境恢复 | fxsound_engine.exe | 保存/恢复默认设备、崩溃恢复 |
| 状态同步 | JSON 协议 | Flutter 解析 stdout，映射到 UI 状态 |

### 5.2 Flutter 代码示例

#### 启动引擎

```dart
import 'dart:convert';
import 'dart:io';

class AudioEngine {
  Process? _process;
  bool isRunning = false;
  String? originalDevice;

  // 效果参数（由 settings 消息更新）
  double fidelity = 0, ambience = 0, surround = 0;
  double dynamicBoost = 0, bass = 0;

  Future<bool> start(String presetPath) async {
    _process = await Process.start(
      'fxsound_engine.exe',
      ['--preset', presetPath],
      mode: ProcessStartMode.normal,
    );

    _process!.stdout
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen(_handleMessage);

    _process!.stderr
        .transform(utf8.decoder)
        .transform(const LineSplitter())
        .listen((line) => debugPrint('[Engine] $line'));

    return true;
  }

  void _handleMessage(String line) {
    if (line.trim().isEmpty) return;
    try {
      final msg = jsonDecode(line);
      switch (msg['type']) {
        case 'startup':
          debugPrint('Engine PID: ${msg['pid']}');
          break;
        case 'device_saved':
          originalDevice = msg['original_device'];
          break;
        case 'ready':
          isRunning = true;
          break;
        case 'settings':
          fidelity = msg['fidelity'];
          surround = msg['surround'];
          dynamicBoost = msg['dynamic_boost'];
          bass = msg['bass'];
          ambience = msg['ambience'];
          // 通知 UI 更新
          onSettingsChanged?.call();
          break;
        case 'device_change':
          onDeviceChanged?.call();
          break;
        case 'shutdown':
          isRunning = false;
          onShutdown?.call(msg['device_restored'] == true);
          break;
        case 'error':
          onError?.call(msg['message']);
          break;
      }
    } catch (_) {}
  }

  void stop() {
    if (_process == null) return;

    // 优先发送 SIGINT，让引擎执行完整清理（恢复默认设备 + 删除恢复文件）
    // SIGINT → SetConsoleCtrlHandler → graceful shutdown
    _process!.kill(ProcessSignal.sigint);

    // 3 秒超时后强杀（TerminateProcess 不可拦截，依赖崩溃恢复文件兜底）
    Future.delayed(const Duration(seconds: 3), () {
      if (_process != null) {
        _process!.kill();
        _process = null;
      }
    });
  }

  // 回调
  VoidCallback? onSettingsChanged;
  VoidCallback? onDeviceChanged;
  void Function(bool restored)? onShutdown;
  void Function(String message)? onError;
}
```

#### 驱动管理（需管理员权限）

```dart
Future<bool> installDriver() async {
  // 方案1: 使用 runas 提权
  final result = await Process.run('runas', [
    '/user:Administrator',
    'fxsound_engine.exe',
    '--install-driver',
  ]);

  // 方案2: Flutter 应用自身以管理员权限启动
  // 或使用 Windows UAC manifest
  return result.exitCode == 0;
}

Future<String> checkDriver() async {
  final result = await Process.run(
    'fxsound_engine.exe', ['--check-driver']);
  return result.stdout.trim(); // JSON
}

Future<String> listDevices() async {
  final result = await Process.run(
    'fxsound_engine.exe', ['--list-devices']);
  return result.stdout; // 纯文本
}
```

### 5.3 退出处理要点

```
优先级从高到低：
1. SIGINT（推荐）：kill(ProcessSignal.sigint) → SetConsoleCtrlHandler 捕获 → 恢复设备 → shutdown JSON
2. Ctrl+C：同上，控制台手动 Ctrl+C
3. 系统关机：CTRL_SHUTDOWN_EVENT → 立即恢复设备 → 退出
4. 异常崩溃 / Process.kill()：不可拦截信号，恢复文件兜底 → 下次启动时自动恢复
```

> **重要**：`Process.kill()` 在 Windows 上调用 `TerminateProcess()`，**无法被任何信号处理器拦截**。
> Flutter 侧必须优先使用 `kill(ProcessSignal.sigint)` 发送 Ctrl+C 信号，确保引擎有时间执行清理。
> 3 秒超时后仍未退出才回退到 `kill()` 强杀，此时依赖 `.fxsound_recovery` 文件兜底。

**Flutter 侧建议**：在 `dispose()` 中调用 `stop()`；监听窗口关闭事件确保引擎退出。

---

## 6. 游戏脚步声增强场景

### 6.1 场景分析

FPS 玩家的核心需求是**在枪声和爆炸声中清晰辨识敌人脚步声的方向和距离**。

| 要素 | 技术要求 | 对应引擎参数 |
|------|----------|-------------|
| 脚步声放大 | 提升 1-4kHz 中高频段（脚步核心频段） | Fidelity + EQ 1k/2kHz Boost |
| 枪声抑制 | 削减 125-500Hz 低频冲击 + 3.5kHz 爆裂峰 | Bass ↓ + EQ 3.5kHz Cut |
| 方位辨识 | 提升双耳差异（ITD/ILD），6-9kHz 方向线索 | Surround + EQ 6k/9kHz Boost |
| 环境噪声控制 | 压制风声/雨声/环境混响 | Ambience ↓ + EQ 低频 Cut |
| 听感自然 | 避免过度压缩，保留动态差异 | Dynamic Boost 适中 |

### 6.2 不同游戏的预设策略

| 游戏 | 地图特征 | 交战距离 | Fidelity | Surround | Ambience | Dynamic Boost | Bass |
|------|----------|----------|----------|----------|----------|---------------|------|
| PUBG | 8km×8km 大地图 | 0-800m | 极高 | 极高 | 极低 | 高 | 中低 |
| Valorant | 室内 CQB | 0-50m | 很高 | 中 | 低 | 中 | 低 |
| CS2 | 中等地图 | 0-100m | 很高 | 高 | 低 | 中 | 低 |
| Apex Legends | 大地图 + 高机动 | 0-300m | 高 | 高 | 低 | 高 | 中 |
| COD Warzone | 大地图 | 0-200m | 高 | 高 | 低 | 高 | 中 |

### 6.3 PUBG 特调预设详解（pubg.fac v4）

以当前 v4 版本为例，展示参数与游戏特性的映射关系：

```
主参数设计思路：
┌────────────┬──────┬──────┬────────────────────────────────┐
│ 效果        │ 值   │ /127 │ 设计理由                       │
├────────────┼──────┼──────┼────────────────────────────────┤
│ Fidelity   │ 115  │ 90%  │ 脚步更远更微弱，拉满保真度       │
│ Surround   │ 105  │ 83%  │ 8km地图方向辨识是核心生存技能   │
│ Ambience   │ 15   │ 12%  │ 大地图环境音丰富，压低避免掩蔽   │
│ DynBoost   │ 65   │ 51%  │ 适中，保留左右耳响度差异         │
│ Bass       │ 25   │ 20%  │ 载具引擎/轰炸区需低频但不盖过脚步│
└────────────┴──────┴──────┴────────────────────────────────┘

EQ 曲线设计（山丘形，听感自然）：
      dB
       │          ╱╲
   +12 │         ╱  ╲
    +9 │        ╱    ╲
    +6 │       ╱  4k  ╲
    +3 │   ╱╲ ╱         ╲
     0 │──╱──╲────────────╲──────
    -3 │╱      9k           14k
    -6 │
    -9 │62Hz  125Hz
       └──┬──┬──┬──┬──┬──┬──┬──┬──┬──> Hz
         62 125 250 500 1k 2k 3.5k 6k 9k 14k

频段映射：
  62.5Hz  -7dB │ 爆炸/轰炸区超低频，压低避免掩蔽
  125Hz   -5dB │ 枪声低频分量
  250Hz   -1dB │ 轻微压低
  500Hz   +4dB │ 脚步踩草地/木板的中低频摩擦
  1kHz    +9dB │ 脚步金属/水泥地面
  2kHz   +12dB │ 脚步核心频段
  3.5kHz  +7dB │ 远距离枪声定位（不过度衰减）
  6kHz    +5dB │ 近场脚步细节
  9kHz    +3dB │ 方位定位高频泛音
  14kHz   -1dB │ 温和衰减避免高频疲劳

开关设置：
  Integer[0]=1  Fidelity 开启
  Integer[1]=1  Surround 开启
  Integer[2]=0  Ambience 关闭
  Integer[3]=1  Dynamic Boost 开启
  Integer[4]=1  Bass 开启
  Integer[5]=1  耳机模式（听声辨位必须）
  Integer[6]=2  音乐模式默认
```

### 6.4 Flutter 产品侧集成流程

```
用户打开游戏增强功能
    │
    ▼
检查驱动是否安装 (--check-driver)
    │
    ├─ 未安装 → 提示用户以管理员权限安装
    │            └→ --install-driver
    │
    ▼
选择游戏预设（pubg/valorant/cs2...）
    │
    ▼
启动引擎 --preset <game>.fac
    │
    ├─ 收到 ready → UI 显示"已开启"
    │               效果参数同步到 UI 滑块
    │
    ├─ 用户调整滑块 → 重启引擎带覆盖参数
    │    （或未来支持运行时参数调整）
    │
    ▼
用户关闭功能 → Process.kill()
    │
    ▼
收到 shutdown → UI 恢复初始状态
    │
    ▼
设备自动恢复（引擎内部完成）
```

---

## 7. FAC 预设文件格式与调参指南

### 7.1 文件格式

FAC 是 FxSound 的预设文件格式，纯文本，键值对结构。

```
CLASS1 : Effect Type          ← 固定值 "1"
9: Version                    ← 版本号
PUBG                          ← 预设名称（自由文本）
0: Double Params Flag         ← 固定值 "0"
1: Total number of elements   ← 固定值 "1"
115: Main 0                   ← Fidelity (0-127)
105: Main 1                   ← Surround (0-127)
0: Main 2                     ← 保留
15: Main 3                    ← Ambience (0-127)
65: Main 4                    ← Dynamic Boost (0-127)
25: Main 5                    ← Bass (0-127)
0: Element Number             ← 固定值 "0"
   0: Param 0~6               ← 保留参数
7: Number of App Integers     ← 固定值 "7"
0: Number of App Reals        ← 固定值 "0"
0: Number of App Strings      ← 固定值 "0"
1: Integer[0]                 ← Fidelity 开关 (0/1)
1: Integer[1]                 ← Surround 开关
0: Integer[2]                 ← Ambience 开关
1: Integer[3]                 ← Dynamic Boost 开关
1: Integer[4]                 ← Bass 开关
1: Integer[5]                 ← 耳机模式 (0=扬声器, 1=耳机)
2: Integer[6]                 ← 音乐模式 (2=默认)
10: Number of EQ Bands        ← 固定值 "10"
1: On/Off Flag                ← EQ 总开关
Band 1~10                     ← 10段参数均衡器
   62.5: CF                   ← 中心频率 (Hz)
   -7: Boost/Cut              ← 增益 (-12 ~ +12 dB)
```

### 7.2 调参要点

**主参数**（0-127，影响 DSP 引擎内部算法权重）：
- Fidelity：全频段细节增强，越高声音越"透"
- Surround：空间宽度展开，越高声场越宽
- Ambience：环境混响渲染，越高越"空灵"（游戏场景建议关）
- Dynamic Boost：动态范围压缩，越高小声越响但可能发闷
- Bass：低频增强，越高低音越重

**EQ 参数**（-12 ~ +12 dB，直接影响频率响应）：
- 低频(62.5-250Hz)：枪声/爆炸/载具能量区，游戏场景建议压低
- 中频(500Hz-2kHz)：脚步声核心频段，游戏场景建议提升
- 中高频(3.5kHz)：枪声爆裂峰值，适度控制避免刺耳
- 高频(6-14kHz)：方位辨识线索，适度提升辅助方向感

**调参原则**：
1. **少即是多**：每次只调 1-2 个参数，对比试听后再继续
2. **主参数为主、EQ 为辅**：先定主参数方向，再用 EQ 精修
3. **避免极端值**：EQ 单段不超过 ±10dB，主参数不超过 120/127
4. **保持曲线连贯**：相邻频段差异不超过 5dB，避免听感断层

---

## 8. 源码改造全流程：fxsound-app → fxsound_engine

本节面向研发和产品经理，系统介绍如何从 FxSound 开源项目改造出无头命令行引擎。产品经理可据此评估改造工作量和风险，研发可据此进行独立复现或二次开发。

### 8.1 改造全景图

```
┌─────────────────────────────────────────────────────────────┐
│               FxSound 原版 (fxsound-app)                     │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────────┐  │
│  │ fxsound/ │ │ fxdiag/  │ │Installer/│ │audiopassthru/ │  │
│  │ GUI(WPF) │ │ 诊断工具  │ │ NSIS安装 │ │ WASAPI 音频管线│  │
│  │ 187 files│ │ 11 files │ │ 154 files│ │   73 files    │  │
│  └────┬─────┘ └────┬─────┘ └────┬─────┘ └──────┬────────┘  │
│       └────────────┴────────────┘              │            │
│                 完全移除（不编译）               │  保留+修改 │
│                                              ▼            │
│  ┌───────────────┐ ┌──────────────┐ ┌─────────────────┐   │
│  │  dsp/ (245f)  │ │audiopassthru │ │   新增代码       │   │
│  │ DfxDsp 核心    │ │ WASAPI 管线  │ │ engine_main.cpp │   │
│  │ ptechDsp 算法  │ │ 设备管理     │ │ driver_manager  │   │
│  │ ptutil 工具    │ │ (注册表已移除)│ │ codedefs.h覆盖 │   │
│  └───────┬───────┘ └──────┬───────┘ └───────┬─────────┘   │
│          └────────────────┴─────────────────┘              │
│                      编译链接                                │
│          ┌──────────────────────────────┐                   │
│          │  fxsound_engine.exe (549KB)  │                   │
│          └──────────────────────────────┘                   │
└─────────────────────────────────────────────────────────────┘
```

### 8.2 源码裁剪：移除的部分

| 模块 | 路径 | 文件数 | 移除原因 |
|------|------|--------|----------|
| GUI 应用 | `fxsound/` | 187 | 无头模式无需 UI（Win32/WPF 窗口、托盘图标、设置面板） |
| 诊断工具 | `fxdiag/` | 11 | 面向用户的独立诊断程序 |
| 安装包 | `Installer/` | 154 | NSIS 脚本、驱动分发，自行管理 |
| 外部驱动工具 | `DfxInstall/` | — | 原版 fxdevcon.exe，用内嵌 SetupDi API 替代 |

**保留编译的核心模块**：`dsp/`（DfxDsp + ptechDsp + ptutil，约 85 文件）和 `audiopassthru/`（约 48 文件）。

### 8.3 源码修改

#### 8.3.1 codedefs.h — NOT_OKAY 宏替换

**文件**：`fxsound-engine/overrides/codedefs.h`（通过 include path 优先级覆盖原版）

**问题**：原版 Debug 弹 `MessageBox` 或 `DebugBreak()`，Release 弹 `MessageBox`，无头进程会无限阻塞。

```
原版：  NOT_OKAY → MessageBoxW(filename, line, MB_OK) 或 DebugBreak()
改造：  NOT_OKAY → fprintf(stderr, ...) + return NOT_OKAY_NO_BREAK
```

影响范围约 200+ 处调用，全部从弹窗阻塞变为 stderr 日志。

#### 8.3.2 FXSOUND_ENGINE_MVP 宏（已移除）

v1.0 MVP 版本使用此宏区分 Loopback 模式和虚拟设备模式，涉及 15 处条件编译。v2.0 移除该宏，统一使用虚拟设备模式，代码路径更清晰。

#### 8.3.3 NO_REGISTRY 宏

通过 CMake `target_compile_definitions` 添加 `-DNO_REGISTRY`，所有 target 均定义。使 `sndDevicesSaveDefaultDevice/RestoreDefaultDevice` 和注册表读写被条件编译跳过。

**副作用**：原版依赖注册表记住"原始默认设备"，禁用后需自行实现（见 8.4.2）。

#### 8.3.4 静态 CRT + SDL 禁用

- 所有 target 设置 `MSVC_RUNTIME_LIBRARY = MultiThreaded`，不依赖 MSVC DLL
- Play16.c/Play32.c 设置 `/SDL-` 禁用安全检查（原版代码使用 sprintf 等不安全函数）

### 8.4 新增代码

#### 8.4.1 fxsound_engine_main.cpp（1015 行）

从零编写的引擎入口，替代原版 GUI 主程序。

| 功能模块 | 代码行 | 说明 |
|----------|--------|------|
| CLI 参数解析 | ~70 | `parse_args()` — 预设、设备、效果覆盖、驱动管理等 |
| 信号处理 | ~30 | SIGINT/SIGTERM + SetConsoleCtrlHandler（7 种事件） |
| 设备保存/恢复 | ~140 | WASAPI 枚举 + IPolicyConfigVista 切换 |
| 崩溃恢复 | ~60 | .fxsound_recovery 文件读写 |
| 驱动管理命令 | ~50 | install/uninstall/check 包装 |
| JSON 通信层 | 分散 | 所有 stdout 输出均为 JSON |
| 主循环 | ~30 | processTimer() + 设备变化检查 |

#### 8.4.2 环境恢复机制（新增设计）

这是改造中最关键的新增逻辑，补偿 NO_REGISTRY 导致的设备恢复能力缺失：

```
启动：
  ① saveDefaultPlaybackDevice() — 保存当前默认设备
     - 若默认设备已是 FxSound 虚拟设备（上次崩溃残留），自动枚举第一个真实设备
  ② 检查恢复文件 → 存在则恢复上次设备 + 删除文件
  ③ 写入恢复文件（设备ID + 设备名，两行文本）

退出：
  ④ restoreDefaultPlaybackDevice() — IPolicyConfigVista 切回 + 删除恢复文件
```

- `IPolicyConfigVista` 是未公开 COM 接口（复用原版 PolicyConfig.h）
- `Process.kill()` 无法拦截信号，恢复文件是唯一兜底
- 恢复文件中设备名含 "FxSound" 则跳过，避免循环恢复

#### 8.4.3 driver_manager.cpp/h（595+74 行）

从原版 `DfxInstall` 移植并重新封装为内嵌 C++ API：

| API | 说明 |
|-----|------|
| `installDriver(dir, log)` | 8 步 SetupDi + 自动启用 + 电源覆盖 |
| `uninstallDriver(log)` | 遍历删除所有匹配设备（含幽灵设备） |
| `checkDriverStatus()` | 检查 "FxSound Audio Enhancer" 设备 |
| `isAdmin()` | 管理员权限检查 |

相比原版新增：InstallDiag 诊断结构、CPU/OS 自动检测、自动启用、powercfg 电源覆盖。

### 8.5 构建系统

**三个编译目标**：

| Target | 类型 | 源码 | 依赖 |
|--------|------|------|------|
| `dfxdsp` | 静态库 | fxsound-app/dsp/ (~85 文件) | 无 |
| `audiopassthru` | 静态库 | fxsound-app/audiopassthru/ (~48 文件) | dfxdsp |
| `fxsound_engine` | 可执行文件 | fxsound-engine/src/ (3 新文件) | 两者 |

**关键配置**：`OVERRIDE_DIR` 在 include path 中优先于原始目录，实现"不改原版源码"的 patch 方式。

### 8.6 工作量评估（供产品经理参考）

| 阶段 | 工作内容 | 预估 | 风险 |
|------|----------|------|------|
| 源码分析 | 理解架构、识别核心模块 | 2-3 天 | 低 |
| 基础裁剪 | CMake + NO_REGISTRY + 去掉 GUI | 2-3 天 | 中 |
| 引擎主程序 | CLI + JSON + DSP 初始化 + 主循环 | 3-5 天 | 中 |
| 设备恢复 | 保存/恢复 + 恢复文件 + 信号处理 | 2-3 天 | 高 |
| 驱动管理内嵌 | SetupDi 移植 + 封装 | 2-3 天 | 高 |
| 联调测试 | FAC 预设、设备热插拔、崩溃恢复 | 2-3 天 | 中 |
| **合计** | | **13-20 天** | |

---

## 9. 编译与构建

### 9.1 环境要求

| 依赖 | 版本要求 | 说明 |
|------|----------|------|
| Visual Studio 2022 | 17.x | 需含 C++ 桌面开发工作负载 |
| CMake | ≥ 3.20 | VS 2022 自带 3.29+ |
| Windows SDK | 10.0+ | WASAPI / SetupDi / Newdev API |
| fxvad.sys 驱动文件 | — | 来自 `fxsound-app/Installer/Drivers/` |

### 9.2 编译步骤

```bash
# 1. 打开 VS 2022 x64 Native Tools Command Prompt

# 2. 进入引擎目录
cd c:\path\to\fxsound-engine

# 3. 配置（首次）
cmake -B build -G "Visual Studio 17 2022" -A x64

# 4. 编译 Release
cmake --build build --config Release

# 5. 输出位置
#    build/Release/fxsound_engine.exe
```

### 9.3 驱动文件部署

```bash
# 编译后需手动部署驱动文件到 exe 同级目录
mkdir build\Release\drivers\win10\x64
copy ..\fxsound-app\Installer\Drivers\Version14\win10\x64\fxvad.inf build\Release\drivers\win10\x64\
copy ..\fxsound-app\Installer\Drivers\Version14\win10\x64\fxvad.sys build\Release\drivers\win10\x64\
copy ..\fxsound-app\Installer\Drivers\Version14\win10\x64\fxvadntamd64.cat build\Release\drivers\win10\x64\
```

### 9.4 预设文件

CMake 的 POST_BUILD 步骤会自动复制 `fxsound-app/dsp/ptComSftDfx/` 下的内置预设（0.fac - 9.fac）到 `build/Release/presets/`。

游戏特调预设（pubg.fac / valorant.fac）需手动复制到 exe 同级目录。

---

## 10. 常见问题与排查

### Q1: 启动后无声音

**排查步骤**：
1. 检查驱动：`fxsound_engine.exe --check-driver`，应返回 `"result":"OK"`
2. 检查设备列表：`fxsound_engine.exe --list-devices`，确认有真实播放设备
3. 检查 stderr 输出：是否有 `NOT_OKAY` 或 `Failed to init AudioPassthru`
4. 检查 Windows 声音设置：默认设备是否被切换到 "FxSound Audio Enhancer"

### Q2: 退出后无声音

**原因**：默认播放设备仍指向虚拟设备，未恢复。

**解决方案**：
1. 立即修复：`fxsound_engine.exe --restore-default`
2. 再次启动引擎会自动检测恢复文件并恢复
3. 手动修复：Windows 设置 → 系统 → 声音 → 将输出设备切回真实设备

### Q3: 驱动安装失败

**常见原因及解决**：

| 失败结果 | 原因 | 解决 |
|----------|------|------|
| Administrator required | 未以管理员运行 | 右键 → 以管理员身份运行 |
| INF not found | 驱动文件不在正确路径 | 确认 `drivers/win10/x64/fxvad.inf` 存在 |
| Step 3 failed | INF 文件无效或未签名 | 检查 INF 文件完整性 |
| Step 8 failed | 驱动未签名，Windows 拒绝安装 | 启用测试签名模式或使用 WHQL 签名驱动 |

### Q4: 进程杀掉后系统音频异常

**原因**：`Process.kill()` / `TerminateProcess()` 无法被拦截，引擎来不及恢复设备。

**机制**：引擎在启动时写入 `.fxsound_recovery` 文件，下次启动自动恢复。Flutter 侧也可调用 `--restore-default` 手动恢复。

### Q5: 设备变化风暴（大量 device_change JSON）

**已修复**：v2.0 引擎内置 1 秒防抖机制，设备变化回调最多每秒输出一次。若仍出现，检查是否有应用频繁切换默认设备。

### Q6: FAC 预设加载失败

**排查**：
1. 确认文件路径正确（支持绝对路径和相对路径）
2. 确认文件格式：纯文本、行首无 BOM
3. 确认 Main 参数数量正确（6 个）、Integer 数量正确（7 个）
4. 确认 EQ Band 数量 = 10

### Q7: AGPL-3.0 合规性

FxSound 源码采用 AGPL-3.0 许可证。引擎采用**进程隔离模型**（独立可执行文件 + 进程间通信），从合规角度：
- 引擎作为独立进程运行，不被链接到 Flutter 应用
- AGPL 的网络传染性限制在引擎进程内
- **建议**：分发时附带 FxSound 源码或提供获取方式，满足 AGPL 要求

---

> **文档版本**：v1.1 | **最后更新**：2026-04-23 | **维护者**：FxSound Engine Team
