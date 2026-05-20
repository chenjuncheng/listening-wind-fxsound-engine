# AI游戏音效优化大师 — 方案C实施计划

> **当前状态**：Flutter 集成已完成，进入质量打磨阶段
> **最后更新**：2026-04-24

---

## 一、方案演进背景

### 1.1 方案对比

| 方案 | 描述 | 状态 | 关键问题 |
|------|------|------|----------|
| 方案A | Flutter 控制 FxSound.exe 进程 | 已否决 | 无法程序化修改参数 |
| 方案B | FxSound 源码编译为 DLL，FFI 嵌入 | 备选 | AGPL-3.0 传染风险 |
| 方案C | 独立 CLI 引擎 + Flutter 进程管理 | **当前** | 进程隔离合规性最佳 |

### 1.2 方案C两代迭代

| 版本 | 模式 | 状态 | 问题 |
|------|------|------|------|
| v1.0 | Loopback 模式（同一设备 capture=playback） | 已废弃 | 正反馈导致严重杂音 |
| v2.0 | 虚拟设备模式（fxvad.sys 驱动） | **当前** | 零反馈、零杂音，生产可用 |

---

## 二、当前完成状态

### 2.1 引擎侧（已完成）

- fxsound_engine.exe 编译成功（VS2022 x64 Release, 549KB）
- 虚拟设备驱动集成（fxvad.sys Root-enumerated 驱动）
- 驱动管理 API（SetupDi 安装/卸载/检查，嵌入 exe 无需外部工具）
- 设备变更自动重连（WASAPI 回调 + processTimer 循环，引擎自愈）
- 环境恢复机制（启动保存默认设备，退出自动恢复，崩溃恢复文件兜底）
- JSON 状态输出（stdout 实时输出设备变更、崩溃恢复、退出状态）
- .fac 预设读写（完整支持，已生成 valorant.fac 特调预设）
- 渠道分发准备（NOT_OKAY 宏改为 stderr 输出，无弹窗；移除注册表依赖）

### 2.2 文档资产（已完成）

- `FxSound_Engine_集成指南.md`（10 章：架构、CLI、Flutter 集成、源码改造、FAQ）
- `AI游戏音效优化大师_PRD.md`（11 章：面向 FPS 玩家的 AI 音频增强产品定义）

### 2.3 Flutter 侧（已完成）

- Process.start() 启动引擎 + Process.kill() 停止 ✅
- stdout JSON 解析（EngineMessage 模型 + switch 字符串映射）✅
- Riverpod 状态管理（EngineNotifier + EngineState）✅
- 参数配置 UI（EffectSlider + AudioPreset）✅
- 驱动安装向导（UAC PowerShell + 结果文件读取）✅
- 预设卡片选择（PresetCard + 游戏主题背景图）✅
- KOOK 风格 UI（毛玻璃 GlassCard + 发光按钮 + 渐变文字）✅
- 崩溃转储检测与展示（CrashLogService + EngineService.findCrashDumps）✅
- 环境恢复机制（--restore-default + 恢复文件）✅
- 实时参数更新（stdin JSON 命令 + sendCommand）✅
- 状态机一致性（_withLock 串行化 + _disposed 全路径检查）✅

### 2.4 UI 品牌系统

- TT 品牌色彩系统（#1AC2FF 主色 + 辅助色环 + 渐变预设）
- KOOK 风格深邃暗色（#0D0E12 基底 + 毛玻璃 + 发光渐变 + 大圆角）
- 自定义 Widget：GlassCard / GlowGradientButton / PulseDot / GradientText / _GlowThumbShape
- 游戏主题背景素材（logo.png / bg_pubg.png / bg_cs2.png / bg_apex.png）

### 2.5 已修复的 Bug 列表

- ✅ **Pipe Buffer Deadlock**：PowerShell UAC 安装驱动时 UI 卡死 → drain stdout/stderr + `.then()` 替代 `await`
- ✅ **CMD 弹窗**：C++ 引擎入口 `ShowWindow(GetConsoleWindow(), SW_HIDE)` 隐藏控制台
- ✅ **设备恢复**：`stopEngine()` kill 后调用 `--restore-default` 恢复原始设备
- ✅ **缺失 .fac 文件**：创建 cs2.fac / apex.fac / general.fac，EQ 曲线按游戏特性调优
- ✅ **PresetCard 图标映射**：id 含后缀导致匹配失败 → `.contains()` 模糊匹配
- ✅ **isInstallingDriver 未重置**：驱动安装成功但 loading 状态不消失 → `_checkDriver()` 中重置
- ✅ **EffectSlider 自动重启**：滑动调节参数时频繁重启引擎 → 只在切换预设卡片时重启
- ✅ **`_restartEngine()` 返回值未检查**：引擎重启失败无提示 → exitCode 判断和错误提示
- ✅ **引擎意外退出状态不更新**：引擎崩溃后 UI 仍显示"运行中" → onDone 发送 synthetic shutdown
- ✅ **并发重启 Race Condition**：`_withLock()` 串行化 + Process.start 存局部变量
- ✅ **crash_log_service String? 错误**：局部变量提取修复
- ✅ **切换预设后启动状态丢失 + 死锁**：补全 state 更新 + switch 字符串映射 + stopEngine 移到锁外

### 2.6 代码审查报告（2026-04-24）

**Flutter 侧审查**：8 个问题已修复

1. ✅ `_withLock` 重入 bool 绕过锁 → 回退纯 Completer 链排队
2. ✅ `toggleEngine` 停止时 `error:null` 吞崩溃信息 → 不覆盖 error
3. ✅ `stopEngine` 期间 subscription 仍活跃 → 先 cancel 再 stop
4. ✅ `EffectSlider` value 未 clamp → `value.toDouble().clamp(0.0, 10.0)`
5. ✅ `preset_provider` `.first` 无空列表保护 → fallback 默认预设
6. ✅ 120s Timer 未随 dispose 取消 → `_driverOpTimer` 存储 + onDispose cancel
7. ✅ `bgGradient` 未使用变量 → 删除
8. ✅ `resetToDefault` null 检查 → getPresetById 有 orElse 兜底

**C++ 引擎审查**：20 个问题（4 严重 + 5 高 + 6 中 + 5 低）

严重问题：
1. `console_ctrl_handler` 中调 COM 可能死锁（Ctrl Handler 线程 vs 主线程 COM 调用）
2. `EngineCallback` static 变量数据竞争（多线程读写无 atomic/mutex）
3. DSP 在音频线程活跃时先于 AudioPassthru 被 powerOff（use-after-close）
4. `g_running` volatile bool 非 atomic（信号 handler 写 + 主线程读，UB）

---

## 三、架构设计

### 3.1 技术架构

```
Flutter Windows App
  ├── UI Layer（预设选择、效果调节、设备切换）
  ├── 配置管理（.fac 文件读写）
  └── 状态监听（stdout JSON 解析）
          │
          │  Process.start() / Process.kill()
          ▼
  fxsound_engine.exe (CLI)
  ├── 参数解析 -> 驱动检测 -> 设备枚举 -> 音频管线启动
  ├── DfxDsp（DSP 处理：Fidelity/Surround/Bass...）
  └── AudioPassthru（WASAPI I/O：Loopback 捕获 + 播放）
          │
          ▼
  Windows Audio System
  ├── fxvad.sys（虚拟播放设备，设为系统默认）
  └── 真实播放设备（耳机/音箱）
```

### 3.2 音频数据流

```
系统音频 -> fxvad.sys（虚拟设备） -> Loopback 捕获 -> DfxDsp 处理 -> 真实播放设备
```

### 3.3 进程生命周期

```
Flutter                        fxsound_engine.exe
  │                                    │
  ├─ Process.start() ────────────────>│ saveDefaultPlaybackDevice()
  │                                    │ checkDriverStatus()
  │<── device_saved JSON ─────────────┤ 初始化音频管线
  │                                    │
  │<── device_change JSON ────────────┤ （设备变更自动重连）
  │                                    │
  ├─ kill(sigint) ──────────────────>│ restoreDefaultPlaybackDevice()
  │<── shutdown JSON ────────────────┤ 删除恢复文件, exit(0)
```

### 3.4 Flutter 侧架构

```
EngineService（进程管理 + Completer 链排队锁）
        │
        ▼
EngineNotifier（Riverpod Notifier 状态机）
   ├── EngineState（isRunning/isStarting/isDriverInstalled/error/...）
   ├── _withLock 串行化 start/stop 生命周期
   ├── _disposed 全路径检查防止 dispose 后更新
   ├── _messageSubscription 监听 stdout JSON
   └── _timer 计时运行时间
        │
        ▼
UI Layer
   ├── HomeScreen（品牌标题 + 预设选择 + 效果参数 + 操作按钮）
   ├── PresetCard（游戏主题背景 + 选中态发光 + 悬停动效）
   ├── EffectSlider（渐变轨道 + 发光 thumb + clamp 保护）
   ├── SettingsScreen（驱动管理 + 设备列表 + 关于）
   └── DriverWarningBanner（驱动状态横幅）
```

---

## 四、CLI 参数规范

### 4.1 音频处理参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--preset <path>` | string | 必需 | .fac 预设文件路径 |
| `--device <name>` | string | 默认设备 | 真实播放设备名称（模糊匹配） |
| `--device-index <n>` | int | 0 | 设备索引 |
| `--fidelity <0-10>` | float | 预设值 | 保真度 |
| `--surround <0-10>` | float | 预设值 | 环绕声 |
| `--ambience <0-10>` | float | 预设值 | 氛围/空间感 |
| `--dynamic-boost <0-10>` | float | 预设值 | 动态增强 |
| `--bass <0-10>` | float | 预设值 | 低音增强 |
| `--buffer <ms>` | int | 40 | 缓冲区大小 |

### 4.2 驱动管理参数（需管理员权限）

| 参数 | 说明 |
|------|------|
| `--install-driver [dir]` | 安装 fxvad.sys |
| `--uninstall-driver` | 卸载 fxvad.sys |
| `--check-driver` | 检查驱动状态 |

### 4.3 工具参数

| 参数 | 说明 |
|------|------|
| `--list-devices` | 列出所有播放设备 |
| `--restore-default` | 从崩溃恢复文件还原默认设备 |
| `--analyze` | 仅捕获模式（测试用） |

---

## 五、JSON 输出协议

### 5.1 启动阶段

```json
{"type":"device_saved","original_device":"扬声器 (Realtek Audio)"}
```

### 5.2 运行阶段

```json
{"type":"device_change","device_count":2,"status":"reconnected"}
{"type":"crash_recovery","status":"restored"}
```

### 5.3 退出阶段

```json
{"type":"shutdown","status":"ok","device_restored":true,"restored_device":"扬声器 (Realtek Audio)"}
```

### 5.4 错误输出

```json
{"type":"error","code":"DRIVER_NOT_INSTALLED","message":"请使用 --install-driver 安装虚拟设备驱动"}
{"type":"error","code":"DEVICE_NOT_FOUND","message":"找不到指定播放设备"}
```

---

## 六、引擎自动重连机制

引擎内部已实现自动重连（WASAPI 回调 -> stopAudio -> processTimer 重试 -> sndDevicesReInit -> CreateThread），大部分设备变更场景无需 Flutter 重启进程。

| 场景 | 引擎自愈 | Flutter 操作 |
|------|---------|--------------|
| 拔掉耳机，插回同一个 | 是 | 仅 UI 提示 |
| 换另一个播放设备 | 是 | 仅 UI 提示 |
| 系统切换默认设备 | 是 | 仅 UI 提示 |
| 设备彻底丢失（无播放设备） | 否 | 需要重启引擎 |
| 虚拟设备被卸载 | 否 | 提示安装驱动 |
| reInit 反复失败 | 否 | 需要重启引擎 |

**Flutter 建议**：监听 `device_change` 计数，超过阈值（5 次）自动重启引擎防止死循环。

---

## 七、Flutter 集成要点

### 7.1 优雅退出

优先使用 `ProcessSignal.sigint` 发送 Ctrl+C，引擎可执行完整清理逻辑（恢复默认设备 + 删除恢复文件）。3 秒超时后强杀，依赖崩溃恢复文件兜底。

### 7.2 首次启动流程

```
启动引擎 -> checkDriverStatus 失败
  -> 输出 DRIVER_NOT_INSTALLED 错误 JSON
  -> Flutter 弹出安装向导
  -> 请求管理员权限执行 --install-driver
  -> 安装成功后重新启动引擎
```

---

## 八、FAC 预设文件映射

| FAC 字段 | 效果参数 | 范围 |
|----------|---------|------|
| Main[0] | Fidelity（保真度） | 0-127 |
| Main[1] | Surround（环绕声） | 0-127 |
| Main[2] | （保留） | 0 |
| Main[3] | Ambience（氛围/空间感） | 0-127 |
| Main[4] | Dynamic Boost（动态增强） | 0-127 |
| Main[5] | Bass（低音增强） | 0-127 |
| Integer[0-4] | 各效果 on/off 开关 | 0/1 |
| Integer[5] | 耳机模式（0=扬声器, 1=耳机） | 0/1 |
| Integer[6] | 音乐模式（2=默认） | 2 |

CLI 参数 0-10 线性映射到 FAC 的 0-127。

**预设文件位置**：

| 类别 | 路径 |
|------|------|
| 内置预设 | `fxsound-app/Installer/Resources/Factsoft/ (0.fac-9.fac)` |
| 奖励预设 | `fxsound-app/bin/BonusPresets/` |
| 游戏特调 | `fxsound-engine/build/Release/valorant.fac` |

---

## 九、驱动部署

驱动文件从 `fxsound-app/Installer/Drivers/Version14/win10/x64/` 复制到 `fxsound_engine.exe` 同目录的 `drivers/win10/x64/` 子目录。

---

## 十、环境恢复机制

### 正常流程

1. 启动：保存当前默认播放设备 -> 写入 `.fxsound_recovery` 文件 -> 输出 `device_saved`
2. 运行：音频管线正常处理
3. 退出：恢复默认设备 -> 删除恢复文件 -> 输出 `shutdown`

### 崩溃恢复

异常退出（Process.kill / 崩溃 / 断电）后恢复文件残留。下次启动时：

- 设备名**不含** "FxSound" -> 恢复设备，删除文件，输出 `crash_recovery:restored`
- 设备名**含** "FxSound" -> 跳过（避免恢复到虚拟设备），输出 `crash_recovery:skipped`

### 手动恢复

`fxsound_engine.exe --restore-default` 可随时手动还原。

---

## 十一、风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| fxvad.sys 驱动签名 | 用户无法安装 | MVP 用测试签名，正式版申请 WHQL |
| 反作弊软件拦截 | FPS 玩家无法使用 | Root-enumerated 风险极低，提供白名单指南 |
| 崩溃后设备未恢复 | 用户失去音频 | 崩溃恢复文件 + --restore-default 兜底 |
| AGPL-3.0 传染 | 商业化风险 | 进程隔离模型，源码不混合 |

---

## 十二、里程碑

| 版本 | 目标 | 预计时间 | 状态 |
|------|------|---------|------|
| v0.1 | 引擎编译通过 | 2026-04-21 | 已完成 |
| v0.2 | 虚拟设备模式，零杂音 | 2026-04-22 | 已完成 |
| v0.3 | 环境恢复机制 | 2026-04-23 | 已完成 |
| v0.5 | Flutter MVP 集成 + KOOK UI | 2026-04-24 | 已完成 |
| v0.6 | C++ 引擎安全加固（atomic/RAII/清理顺序） | 2026-04-25 | 待开始 |
| v1.0 | 公测版本 | 2026-04-28 | 待开始 |

---

**文档维护者**：技术文档工程师 Agent  
**版本**：v1.0  
**创建日期**：2026-04-23
