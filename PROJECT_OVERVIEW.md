# rorrerror 项目全景简介（AI 上下文导航文档）

> 本文档是为 AI 助手上下文初始化设计的项目导航说明。阅读完本文档，你应能立刻定位到"改哪个文件、调哪个类、走哪条数据流"。

---

## 1. 项目概述

### 1.1 项目定位
- **产品名**：`rorrerror`（版本：`1.2.0`）
- **产品形态**：一款 **破坏性失真 / 故障（Glitch）效果插件**，UI 为 **三列老虎机（Slot Machine）交互**：拖动列 = 搓碟（Scratch），底部横条 = 参数控制。视觉主题为暗红做旧背景 + 循环 glitch 动画，走地下/亚文化美学路线。
- **发行形态**（在 [CMakeLists.txt](/I:/rorrerror/CMakeLists.txt) 中通过 `juce_add_plugin` 定义）：
  - **Windows**：`VST3` + `Standalone` 独立应用
  - **BundleID / Plug ID**：插件代码 `Grdb`，厂商代码 `Wfsd`，公司名 `iisaacbeats.cn`
  - 通道布局：单声道 / 立体声（输入输出必须一致）
- **代码仓库**：`https://github.com/sweetorange1/rorrerror.git`
- **代码骨架来源**：`grid_plugin` 模块骨架来自 The WolfSound 的 JUCE CMake 模板（模块头保留其 MIT 声明），业务代码（DSP/UI/网络）均为本项目原创。

### 1.2 主要功能一览
- **三列老虎机选择器**（核心交互）：
  - **第一列（4 档）**：输出削波 —— `1_1` 软削波（tanh）/ `1_2` 硬削波（±1 限幅）/ `1_3` 折返削波（foldback）/ `1_4` 非对称削波（正负半周不对称）
  - **第二列（15 档）**：Waveshaper 波形整形器 `2_1` ~ `2_15`（详见 §5.1）
  - **第三列（10 档）**：节拍同步 Glitch `3_1` ~ `3_10`（详见 §5.1）
- **三列底部横向控制条**：输入增益（0~24dB）/ Waveshaper 干湿比（0~100%）/ Glitch 触发周期（7 点离散）
- **搓碟（Scratch）**：拖动任意列时，从 0.6s 环形缓冲中以拖动速度正放/倒放
- **随机按钮（rand）**：三列老虎机滚动 + 全链路"倒带"音效（scratch rate = -2.8）
- **About 按钮**：关于弹窗
- **循环 glitch 背景**：60 帧（10×6 sprite sheet）@20fps，每轮随机替换 3 帧为 glitch 效果帧（每帧 3 个版本，共 180 张）
- **自动更新检查** + 自定义风格更新弹窗（§5.4）
- **每日一次匿名遥测**（§5.5）

### 1.3 技术栈
| 项目 | 版本 / 说明 |
| --- | --- |
| 语言 | **C++23**（`CMAKE_CXX_STANDARD 23`，`CXX_EXTENSIONS OFF`） |
| 框架 | **JUCE 8.0.12**（通过 `FetchContent` 自动拉取，首次配置需联网） |
| 构建 | CMake ≥ 3.22 |
| Windows CRT | 强制静态 CRT（`MultiThreaded`，避免依赖 VC_redist） |
| 编码 | `/utf-8`（MSVC，便于中文注释） |
| 安装器 | Inno Setup 6（[rorrerror_installer.iss](/I:/rorrerror/rorrerror_installer.iss) + [build_installer.bat](/I:/rorrerror/build_installer.bat)） |
| 资源打包 | `juce_add_binary_data`（背景图集 + 29 张功能贴图 + 180 张 glitch 帧） |
| 裁剪宏 | `JUCE_WEB_BROWSER=0`、`JUCE_VST3_CAN_REPLACE_VST2=0`；Debug 下 `JUCE_DISABLE_ASSERTIONS=1` |

---

## 2. 核心分层架构

```
┌────────────────────────────────────────────────────────────────┐
│  构建层（CMakeLists.txt）                                         │
│    GridButtonsPlugin（juce_add_plugin：VST3 + Standalone）        │
│    GridButtonsPlugin_BinaryData（juce_add_binary_data：全部贴图）  │
├────────────────────────────────────────────────────────────────┤
│  Plugin 层（grid_plugin 模块）                                    │
│    grid::PluginProcessor  (source/PluginProcessor.cpp)          │
│      ↑ 音频线程 processBlock：完整 DSP 链路                       │
│    grid::PluginEditor     (source/PluginEditor.cpp)             │
│      ↑ UI 线程：老虎机绘制/交互/背景动画/遥测 Session             │
├────────────────────────────────────────────────────────────────┤
│  Network 层（grid_plugin/source/network）                        │
│    Version.cpp        —— SemVer 解析与比较                       │
│    UpdateChecker.cpp  —— 异步 GET 更新检查（进程级去重）           │
├────────────────────────────────────────────────────────────────┤
│  UI Dialog 层（grid_plugin/source/ui）                           │
│    UpdateDialog.cpp   —— rorrerror 风格原生更新弹窗（480×340）     │
├────────────────────────────────────────────────────────────────┤
│  Shared 层（shared/）                                            │
│    IisaacTelemetry.h  —— header-only 每日遥测（跨项目共享）        │
└────────────────────────────────────────────────────────────────┘
```

### 2.1 关键调用关系
1. **音频链路**：`PluginProcessor::processBlock` 按序执行：输入增益（平滑）→ Scratch 混音 → Column2 Waveshaper（干湿混合）→ Column3 Glitch（拍同步）→ Column1 削波 → 输出（详见 §5.1）。
2. **UI → 音频线程通信**：全部走 `std::atomic`（无锁）。Editor 在 `mouseDown/Drag/Up` 中调用 `setColumnXSelectedIndex / setSliderXNorm / setScratch*`，Processor 内部平滑后生效。
3. **音频 → UI 反馈**：Processor 在 block 末尾把 glitch 相位/强度/事件计数写入原子（`column3Ui*`），Editor 以 60Hz `Timer` 读取，用于第三列闪光与进度显示。
4. **头文件合并**：`grid_plugin.cpp` 直接 `#include` 全部 `.cpp`（Editor/Processor/网络/弹窗），模块以单翻译单元方式编译（见 §6.2）。

---

## 3. 代码结构说明

### 3.1 根目录关键文件
| 文件 | 作用 |
| --- | --- |
| [CMakeLists.txt](/I:/rorrerror/CMakeLists.txt) | CMake 主构建脚本：FetchContent 拉 JUCE、插件目标、贴图打包、静态 CRT |
| [rorrerror_installer.iss](/I:/rorrerror/rorrerror_installer.iss) | Inno Setup 安装器：安装到 `{commoncf}\VST3\iisaacbeats.cn`，需管理员权限，非默认目录时弹 DAW 重扫描提示 |
| [build_installer.bat](/I:/rorrerror/build_installer.bat) | 一键查找 ISCC.exe 并打包，产物输出 `dist/` |
| [main.cpp](/I:/rorrerror/main.cpp) | 占位文件（Hello World，未参与构建，可忽略） |
| [assets/](/I:/rorrerror/assets) | 全部视觉资源（见 §3.3） |
| [grid_plugin/](/I:/rorrerror/grid_plugin) | JUCE 模块：Processor + Editor + 网络 + 弹窗 |
| [shared/IisaacTelemetry.h](/I:/rorrerror/shared/IisaacTelemetry.h) | header-only 匿名遥测（与 iisaacbeats 旗下其他产品共享同一份） |

### 3.2 `grid_plugin/`（插件核心模块）
| 路径 | 作用 |
| --- | --- |
| `grid_plugin.h/.cpp` | 模块声明与单翻译单元 include 汇总（**所有 .cpp 在这里合并**） |
| `include/Grid/PluginProcessor.h` | Processor 头：模式枚举、原子参数、Biquad/DcBlocker、scratch/glitch 状态 |
| `include/Grid/PluginEditor.h` | Editor 头：老虎机动画状态、slot spin、glitch 帧覆盖 |
| `source/PluginProcessor.cpp` | **DSP 核心**：15 种 waveshaper 公式、4 种输出削波、10 种 glitch 算法、scratch 环形缓冲、响度均衡、状态序列化 |
| `source/PluginEditor.cpp` | **UI 核心**：背景动画、透视轮盘绘制、拖拽/吸附/滚动、搓碟速率估算、遥测 Session 持有 |
| `source/network/Version.h/.cpp` | SemVer（`X.Y.Z[-prerelease]`）解析与比较 |
| `source/network/UpdateChecker.h/.cpp` | 异步更新检查：GET `iisaacbeats.cn/api/update/check`，5s 超时，进程级去重 |
| `source/ui/UpdateDialog.h/.cpp` | 原生置顶小窗（480×340），Download / Remind Me Later，`force_update` 时仅有 Download 且不可关闭 |

### 3.3 `assets/`（视觉资源）
| 路径 | 内容 |
| --- | --- |
| `background_pnglist.png` | **22MB 背景 sprite sheet**：10 列 × 6 行 = 60 帧，@20fps 循环播放 |
| `background.png` | 背景静态兜底帧（sprite sheet 未就绪时显示） |
| `png/1_1.png` ~ `1_4.png` | 第一列：软削波 / 硬削波 / 折返 / 非对称（`*old.png` 为旧版贴图，**未打包**） |
| `png/2_1.png` ~ `2_15.png` | 第二列：15 种 waveshaper 贴图 |
| `png/3_1.png` ~ `3_10.png` | 第三列：10 种 glitch 模式贴图 |
| `png/rand.png` | 随机按钮图标（双箭头） |
| `png/about.png` | About 按钮图标（"P" 字母） |
| `glitched_frames/` | 180 张 glitch 帧：`frame_{000..059}_v{1,2,3}.png`，每轮动画随机挑 3 帧替换 |

### 3.4 `shared/IisaacTelemetry.h`（遥测）
- header-only，无授权弹窗、无自动更新，只做**每日一次** `ui_open_daily` 事件 POST 到 `https://iisaacbeats.cn/api/telemetry/ping`。
- 客户端 UUID 首次生成后持久化在 `%APPDATA%\iisaacbeats\Telemetry\<productId>.xml`。
- 停用方式：编译宏 `IISAAC_TELEMETRY_DISABLED`，或启动前设环境变量 `IISAAC_TELEMETRY_DISABLED=1`。
- 生命周期铁律：**构造/析构必须在消息线程**；只允许 Editor 持有 Session，禁止放进 Processor、音频回调或静态全局对象（详见头文件注释）。

---

## 4. 关键类 / 接口清单

### 4.1 `grid::PluginProcessor`（[PluginProcessor.h](/I:/rorrerror/grid_plugin/include/Grid/PluginProcessor.h)）
- 模式枚举：`Column1Mode { SoftClip, HardClip, FoldClip, AsymClip }`、`Column2Mode { Shape_2_1 .. Shape_2_15 }`。
- UI 选择索引与控制条：`setColumn{1,2,3}SelectedIndex`、`setSlider{0,1,2}Norm`（内部做 clamp 并同步派生音频参数，**恢复状态时只需调这 6 个 setter**）。
- 搓碟：`setScratchEnabled(bool)` + `setScratchRate(float)`（rate 限幅 ±4，|rate|<0.02 死区归零；正放 / 倒放 / 停住）。
- UI 反馈：`getColumn3GlitchEventCounter()`（事件计数，捕捉极短触发）、`getColumn3GlitchUiAmount01()`（强度）、`getColumn3GlitchUiPhase01()`（周期相位）。
- 曲线预览：`evaluateColumn2Waveshaper(x)` —— 纯公式映射，不含 wet/滤波/后续级，供 Editor 画函数曲线。
- 状态持久化：`getStateInformation / setStateInformation` —— 自定义二进制（magic `'GRID'` + version 1）：3 个列索引 + 3 个控制条值。**没有用 JUCE AudioProcessorValueTreeState**，加新参数时需手动扩展该序列化格式（记得升 version）。

### 4.2 `grid::PluginEditor`（[PluginEditor.h](/I:/rorrerror/grid_plugin/include/Grid/PluginEditor.h)）
- 老虎机透视轮盘：中心 scale 1.0 → 远端 0.48，横向圆弧位移 18px，深度曲线 pow 1.25，远端 alpha 0.55。
- 背景动画：60 帧 sheet 由 `BackgroundAnimCache`（**进程级常驻、有意泄漏**的静态缓存）在后台线程解码一次，避免每次开关 Editor 重复解码 22MB 图集（见 §6.3）。
- glitch 帧覆盖：每轮动画随机替换 3 帧，每帧有 v1/v2/v3 三个版本，由独立解码线程加载，`std::atomic<bool>` 就绪标记。
- Slot spin（随机滚动）：采用"已知终点"的缓动插值，保证停在随机目标上不跳变；起手触发 scratch 倒带（rate=-2.8）。
- 拖拽 = 搓碟：按下即 `setScratchEnabled(true)`，rate 由 Y 方向拖动速度估算（10px/16ms ≈ 1x，限幅 ±3.5），松手关闭。
- 窗口缩放：`recenterColumnsToProcessorSelection` 保证缩放后当前居中选中项不变。

### 4.3 `grid::network::UpdateChecker`（[UpdateChecker.h](/I:/rorrerror/grid_plugin/source/network/UpdateChecker.h)）
- `CheckForUpdatesAsync(product, current_version, platform, callback)`：后台线程 HTTP，回调经 `MessageManager::callAsync` 切回主线程；失败/超时静默返回 `has_update=false`。
- 进程级去重：构造函数里 `static std::atomic<bool>` 保证**多实例只检查一次**，启动后延迟 5s 触发。
- 平台标识格式 `<os>-<arch>`（如 `win-x64`），与遥测口径一致。

### 4.4 `grid::ui::UpdateDialog`（[UpdateDialog.h](/I:/rorrerror/grid_plugin/source/ui/UpdateDialog.h)）
- 通过 `addToDesktop` 创建独立原生窗口（非宿主子窗口），`setAlwaysOnTop` 置顶，标题栏可拖拽。
- `force_update=true` 时只显示 Download 且不可关闭。

---

## 5. 业务逻辑流程

### 5.1 音频 DSP 链路（processBlock）

```
输入 x
  │
  ├─① 输入增益：0~24dB，20ms 一阶平滑（防阶跃爆音）
  │
  ├─② Scratch 混音：0.6s 交织环形缓冲（录"增益后"信号）
  │     · 按下列任一列触发；rate>0 正放 / rate<0 倒放 / 0=停住
  │     · 线性插值读取；开启瞬间 playPos 对齐到"20ms 前刚录到的位置"
  │     · 15ms mix 平滑淡入淡出
  │
  ├─③ Column2 Waveshaper（15 选 1，干湿混合 wet 默认 50%）：
  │     2_1 锯齿：round 误差整形 —— (round(a)-a)*2，a=(-x/2)*3
  │     2_2 梯形量化：round(q*x)/q，q≈10.1
  │     2_3 三角折叠：(1-|wrap(x+1,2)-1|)*2-1（模式内另挂 55Hz/12dB·oct 高通）
  │     2_4 相位扭曲+全波整流：tanh(1.8*(2|sin(2π(x+0.25sin7x))|-1))
  │     2_5 折返失真：foldback(2.8x, 1)
  │     2_6 自调制正弦+幂次+量化：round(11·sign(s)|s|^0.25)/11
  │     2_7 多项式扩展+波折叠+自FM：sin(π/2·x(1+1.6x²))+0.35sin(6y)
  │     2_8 环调/AM 金属感：sin(2πx(1+0.85|x|))·(1-0.35cos2πx)
  │     2_9 粗量化 bitcrush：round(16x)/16
  │     2_10 级联过驱 fuzz：三级 tanh 级联（tanh(3x)→tanh(4(y+0.6x))→tanh(5y)）
  │     2_11 混沌波折叠：迭代正弦映射（sin 三次迭代，密集不可预测谐波）
  │     2_12 正弦波折叠：sin(π·x)
  │     2_13 全波整流：2|x|-1（频率翻倍，八度感）
  │     2_14 交越/死区失真：|x|≤0.15 静音，否则 sign(x)·(|x|-t)/(1-t)
  │     2_15 Chebyshev 5次：16x⁵-20x³+5x（强五次谐波）
  │     · 每种 shape 输出经 ~12Hz 一阶 DC blocker 去直流
  │     · 响度均衡：prepareToPlay 用 997Hz/A=0.5 测试正弦离线测各模式
  │       RMS，得静态归一化增益；运行时再按干/湿短时功率做动态
  │       auto-trim（30ms attack / 150ms release，限幅 0.1~4）
  │     · 模式切换：新旧 shape 输出 20ms 交叉淡化（各自独立 DC blocker），
  │       防函数跳变爆音
  │
  ├─④ Column3 Glitch（10 选 1，拍同步）：
  │     · 维护"上一拍 / 当前拍"两个交织缓冲，tempo 变化时重建
  │     · 触发周期 = 控制条 7 点离散：持续 / 每2·4·8·16·32拍 / 永不
  │       （≥1 拍窗口=周期末尾 1 拍；<1 拍窗口=周期末尾 1/4）
  │     · 3_1 缓冲重复：上一拍开头 1/4 段循环 ×4
  │     · 3_2 消音错误：输出静音
  │     · 3_3 反向切片：上一拍前半拍倒放循环
  │     · 3_4 Sample&Hold/bitcrush：按 1/48 拍保持 + xorshift32 符号翻转 + 1.15x 过驱
  │     · 3_5 随机切片跳读：上一拍切 16 片，xorshift32 随机跳片
  │     · 3_6 颗粒重排：上一拍切 64 颗粒，随机挑颗粒 + 随机正放/倒放
  │     · 3_7 磁带停止：播放头先快后慢指数减速，最后几乎停住
  │     · 3_8 硬核降采样：1/4 拍保持 + 1-bit 量化（只留 ±1）
  │     · 3_9 比特翻转：随机翻转采样值符号/尾数位，产生爆音/数字噪声
  │     · 3_10 扫频环调：正弦载波从低扫到高，金属环调/射频干扰
  │     · glitchMix 3ms 平滑；无宿主 playhead 时用 fallback BPM/PPQ
  │
  ├─⑤ Column1 削波（4 选 1）：软削波 tanh(x) / 硬削波 clamp(±1) / 折返 foldback(x,1) /
  │     非对称 tanh(正 1.3x/负 2x)，切换时 20ms 交叉淡化；输出级另挂 DC blocker
  │     （去除非对称削波的直流偏置）
  │
  └─→ 输出
```

**全链路位置约定**：Scratch 在最前（搓碟信号也会走完整效果链），Glitch 在 Waveshaper 之后、削波之前。

### 5.2 UI 交互 → 音频参数流
1. 拖动列 → 松手吸附最近居中项 → `setColumnXSelectedIndex(i)` → 原子写 `columnXMode` → 音频线程下一 block 生效（配 20ms 交叉淡化）。
2. 拖底部横条 → `setSliderXNorm(n)` → 映射：列1 `n*24dB` 输入增益；列2 `n`=wet；列3 离散映射到 7 档 glitch 周期。
3. 按下随机按钮 → `startRandomSlotSpin()`：三列缓动滚动到随机目标 + scratch 倒带，结束时统一推 `pushUiStateToProcessor`。
4. Processor 每块末尾写 `column3Ui*` 原子 → Editor 60Hz Timer 读取 → 第三列闪光/进度反馈。

### 5.3 状态持久化流程
宿主保存工程 → `getStateInformation` 写 `'GRID'` v1 二进制 → 宿主回放 → `setStateInformation` 校验 magic/version → 6 个 setter 恢复（内部 clamp 并同步派生参数）。UI 侧重开 Editor 时按原子值重放居中选中。

### 5.4 更新检查流程
Processor 构造 →（进程级仅一次）延迟 5s → `CheckForUpdatesAsync("rorrerror", 版本, "win-x64")` → 后台 GET → 主线程回调 → `has_update` 则弹 `UpdateDialog` → Download 开浏览器 / Remind Me Later 关闭。`force_update` 时弹窗强制。

### 5.5 遥测流程
Editor 构造 → 创建 `iisaac::telemetry::Session`（消息线程）→ 5s 后起工作线程 → 读/建 `%APPDATA%\iisaacbeats\Telemetry\rorrerror.xml`（进程锁 + flock）→ 当 UTC 日未上报则 POST ping → 成功记 `last_success_day`，失败留 15min 冷却。跨 UTC 日自动再报，每日至多一次。

---

## 6. 特殊约定与注意事项

### 6.1 音频线程约束
- UI→音频全部走 `std::atomic`（`memory_order_relaxed`）；所有可听参数在音频线程内**再做一阶平滑**（增益 20ms / wet 25ms / mix 15ms / 交叉淡化 20ms / glitch 3ms），不要在 setter 里直接写 DSP 状态。
- `prepareToPlay` 完成所有离线测量（响度均衡增益、滤波器系数、缓冲分配）；tempo 变化只在 processBlock 内重建拍缓冲。

### 6.2 头文件合并（**极其重要**）
- `grid_plugin.cpp` 直接 `#include` 全部 `.cpp`（含网络/弹窗），整个模块是单翻译单元。**新增 .cpp 必须加进 grid_plugin.cpp**，否则链接报 undefined。
- CMake 侧 `juce_add_module(grid_plugin ALIAS_NAMESPACE grid)`，以 `grid::grid_plugin` 引用；插件名宏 `GRID_PLUGIN_NAME` 通过 `target_compile_definitions` 注入。

### 6.3 背景缓存与 DLL 卸载
- `BackgroundAnimCache` 是**有意泄漏**的进程级常驻单例：某些宿主持锁卸载 DLL 时，若析构释放 22MB Image 或 join 线程会卡死。解码线程不 detach，"加载完成后尽快 join"（`tryJoinBackgroundAnimWorkerIfFinished`）。
- 同理，任何新增的线程/大资源都应避免在 DLL 卸载阶段做重型析构。

### 6.4 2_3 模式专用高通
- 55Hz / 12dB·oct（RBJ cookbook，Butterworth Q）Biquad，仅在 2_3 生效；进入/退出 2_3 时 reset 滤波器状态，避免切换"砰声"。
- 注意：波形切换时**不要**重置动态响度补偿（`column2AutoTrim` / `column2DryPower` / `column2WetPower`），否则会因增益瞬时跳变产生电流声；应让其按 attack/release 自然平滑适应新模式。

### 6.5 搓碟细节
- 环形缓冲录的是"输入增益之后"的信号；开启瞬间 playPos 回跳 0.2s，听感是"刮刚放过的那段"。
- rate 死区 0.02、限幅 ±4（UI 估算限 ±3.5）；随机按钮倒带固定 -2.8。
- 兜底路径：缓冲未就绪时跳过 scratch 但保留其余链路。

### 6.6 版本号一致性
改版本号需**三处同步**：`CMakeLists.txt`（project VERSION + juce_add_plugin VERSION）、`rorrerror_installer.iss`（MyAppVersion）、更新检查用 `JucePlugin_VersionString`（自动跟随 CMake）。漏改 iss 会导致安装包版本错位。

### 6.7 安装器与构建目录
- `.iss` 从 `cmake-build-release\GridButtonsPlugin_artefacts\Release\VST3\*` 取产物 —— 构建目录必须命名为 `cmake-build-release`（或自行改 iss 路径）；本机 VS 生成的 `cmake-build-release-visual-studio` **不会**被打包脚本识别。
- 构建后 `COPY_PLUGIN_AFTER_BUILD=TRUE` 会把 VST3 复制到 `%LOCALAPPDATA%\Programs\Common\VST3`（免管理员）；安装器则装到系统级 `{commoncf}\VST3\iisaacbeats.cn`（需管理员）。
- 打包：`build_installer.bat`（自动找 Inno Setup 6 的 ISCC.exe），产物在 `dist/`。

### 6.8 遥测/更新检查的隐私口径
- 两者均为匿名、每日/每进程至多一次、失败静默；遥测可用环境变量 `IISAAC_TELEMETRY_DISABLED=1` 关闭。
- 遥测状态文件损坏时**不会**当作新安装重置 UUID（防串号）；修改 `shared/IisaacTelemetry.h` 时注意它是多产品共享文件，改动需兼容其他产品。

### 6.9 存在但未使用的文件
- `main.cpp`（Hello World 占位，不参与构建）、`assets/png/*old.png`（旧版贴图，未打包进 BinaryData）、`1_1old.png`/`1_2old.png`。清理时先确认无引用。

### 6.10 常见修改场景速查
| 场景 | 改哪里 |
| --- | --- |
| 新增 waveshaper | `Column2Mode` 枚举 + `waveshapeColumn2()` 新分支 + `kColumn2MaxIndex`/`kColumn2ModeCount` + 贴图 `2_N.png` + CMake BinaryData 列表 |
| 新增 Column1 削波模式 | `Column1Mode` 枚举 + `clipColumn1()` 新分支 + `kColumn1MaxIndex` + 贴图 `1_N.png` + CMake BinaryData 列表 |
| 新增 glitch 模式 | `applyColumn3Glitch()` 新分支 + `kColumn3MaxIndex` + 贴图 |
| 调 glitch 触发档位 | `mapSlider2ToGlitchPeriodBeats()`（7 点离散表） |
| 改 UI 布局/透视参数 | `PluginEditor.cpp` 顶部 `kColumnGap / kSliderBandH / kWheel*` 常量 |
| 换背景动画 | 重做 `background_pnglist.png`（10×6=60 帧）；帧数变了需同步 `kBackgroundSheetCols/Rows` |
| 新增可保存参数 | Processor 原子 + setter/getter + `getState/StateInformation` 序列化（升 kVersion）+ Editor `pushUiStateToProcessor` |

---

## 7. 附：目录树（简化版）

```
I:\rorrerror\
├── CMakeLists.txt              # 主构建脚本（JUCE 8.0.12 FetchContent）
├── rorrerror_installer.iss     # Inno Setup 安装器脚本
├── build_installer.bat         # 一键打包
├── main.cpp                    # 占位，未参与构建
├── assets\
│   ├── background.png          # 背景兜底帧
│   ├── background_pnglist.png  # 22MB 背景 sprite sheet（60 帧）
│   ├── png\                    # 1_1~1_4 / 2_1~2_15 / 3_1~3_10 / rand / about
│   └── glitched_frames\        # 180 张 glitch 帧（60×3 版本）
├── grid_plugin\                # JUCE 模块（单翻译单元）
│   ├── grid_plugin.h/.cpp      # 模块声明 + .cpp include 汇总
│   ├── include\Grid\           # PluginProcessor.h / PluginEditor.h
│   └── source\
│       ├── PluginProcessor.cpp # DSP 核心
│       ├── PluginEditor.cpp    # UI 核心
│       ├── network\            # Version / UpdateChecker
│       └── ui\                 # UpdateDialog
├── shared\
│   └── IisaacTelemetry.h       # header-only 每日遥测（多产品共享）
├── dist\                       # 安装包输出（gitignore）
└── cmake-build-release-visual-studio\  # 本机构建目录（gitignore）
```

---

## 8. 开发记录

> 本轮围绕"可听参数/模式切换的爆音与响度均衡"做了四轮迭代，目标是在保留地下/故障音色风味的前提下，消除拖拽与随机切换时的电流声，并平衡各 waveshaper 的响度。

### 8.1 问题一：调节第二列 Waveshaper 湿音时产生电流声
- **根因**：湿音系数虽有平滑但仅 10ms 偏短；输入增益 `g` 完全没有平滑，滑块阶跃直接作用到音频。
- **修复**（[PluginProcessor.cpp](/I:/rorrerror/grid_plugin/source/PluginProcessor.cpp)）：
  - 湿音平滑时间 `10ms → 25ms`（`wetSmoothCoeff`）。
  - 新增输入增益平滑（`20ms`，`inputGainSmoothed`），每采样平滑 `g`，兜底与 Scratch 两条路径均生效。

### 8.2 问题二：2_4 / 2_6 的响度被放大特别多
- **根因**：原响度均衡在 `prepareToPlay` 用固定幅度（A=0.5）正弦离线测得**固定增益**，但 2_4（整流+tanh）与 2_6（`|s|^0.25` 小信号扩展）的增益随输入电平剧烈变化，典型音乐电平下会比其他模式响 2.5~5 倍。
- **修复**：在固定基准增益基础上增加**动态响度补偿**（`column2AutoTrim`）：
  - 逐采样跟踪干/湿短时功率（单声道合成，避免破坏立体声像）。
  - 目标补偿 `sqrt(dryPower / wetPower) / column2ModeGain[mode]`。
  - 非对称平滑（attack 30ms / release 150ms），限幅 `0.1~4.0`。
  - 最终 `yGain = column2ModeGain[mode] * column2AutoTrim`，整体 clamp `0.05~8.0`。
  - 只做线性增益缩放，不改变波形谐波结构/风味。

### 8.3 问题三：拖动第二列 / 点击随机按钮过渡期电流声
- **根因**：`wsMode` 与第一列软/硬削波在音频线程内离散切换，处理函数输出瞬间跳变。
- **修复**：引入**交叉淡化（crossfade）**：
  - 第二列 waveshaper：新增 `column2ActiveMode` / `column2ModeCrossfade`，约 20ms 在新旧 `waveshapeColumn2` 输出间线性过渡；响度基准增益 `column2BaseGain` 随 crossfade 一起混合；2_3 高通启用条件改用 `column2ActiveMode`。
  - 第一列削波：新增 `column1ActiveMode` / `column1ModeCrossfade`，软/硬削波之间约 20ms 平滑过渡。

### 8.4 问题四：静音输入下切换仍有电流声
- **根因**：即使输入静音，部分 waveshaper 在 `x=0` 时输出恒定直流（如 2_3 输出 +1、2_4 输出约 -0.95）。原先在 DC blocker **之前**混合 `yRaw`，导致 DC blocker 输入从旧直流阶跃到新直流，高通滤波器将其放大为 10~20ms 瞬态脉冲；同时模式切换时把 `column2AutoTrim` 突然重置为 1.0 也造成增益跳变。
- **修复**：
  - 新增 `column2DcBlockersNew`：切换期间旧模式走 `column2DcBlockers`、新模式走 `column2DcBlockersNew`，**各自独立去直流后再按 `column2Mix` 交叉淡化**。
  - 交叉淡化推进逻辑：检测到目标模式变化且上次过渡完成时，重置 `column2DcBlockersNew` 并将 `column2ModeCrossfade` 归零重新开始；过渡完成后把 `column2DcBlockersNew` 交接给 `column2DcBlockers`。
  - **移除** `column2AutoTrim/DryPower/WetPower` 的突然重置，让动态响度补偿按 attack/release 自然适应。

### 8.5 涉及文件与关键成员
| 文件 | 本轮改动 |
| --- | --- |
| [PluginProcessor.h](/I:/rorrerror/grid_plugin/include/Grid/PluginProcessor.h) | 新增 `inputGainSmoothed`、`column2ActiveMode/ModeCrossfade`、`column1ActiveMode/ModeCrossfade`、`column2DcBlockersNew` 等状态成员 |
| [PluginProcessor.cpp](/I:/rorrerror/grid_plugin/source/PluginProcessor.cpp) | `prepareToPlay` 初始化交叉淡化/新增 DC blocker；两条处理路径同步实现输入增益平滑、交叉淡化、分路去直流、动态响度补偿 |

> 交叉淡化统一约 20ms，湿音 25ms、输入增益 20ms、glitch mix 3ms、scratch mix 15ms。新增可平滑参数时，务必在音频线程内做一阶平滑，不要在 UI setter 中直接写 DSP 状态。

---

### 第二轮：三列扩展（第一列 4 档 / 第二列 15 档 / 第三列 10 档 / 输入增益 24dB）

> 本轮扩展了三列的可选档位（第一列 2→4 档、第二列 9→15 档、第三列 5→10 档），并把输入增益范围从 0~8dB 扩大到 0~24dB，以配合上一轮加入的动态响度补偿。

#### 8.6 第一列：输出削波 2 档 → 4 档
- **原状态**：`Column1Mode { SoftClip, HardClip }`，仅 tanh / clamp。
- **扩展**：新增 `FoldClip`（折返 `foldback(x,1)`）与 `AsymClip`（非对称二极管式 tanh，正半周 1.3x、负半周 2x）。
- **统一分发**：新增 `clipColumn1(mode, x)` 函数，`processBlock` 两处（兜底路径 + scratch 路径）由三目判断改为 `clipColumn1()`。
- **DC 处理**：非对称削波会产生直流偏置，新增输出级 `column1DcBlockers` / `column1DcR`，在交叉淡化混合之后统一去直流（复用 12Hz 一阶高通系数）。
- **兼容性**：`getStateInformation` 存的是列索引（int），0~3 天然兼容，`kVersion` 无需升级。

#### 8.7 输入增益：0~8dB → 0~24dB
- `kInputGainDbMax` 16→24；`setSlider0Norm` 与 Editor `setSliderNormValue` 的 `*8.0f`→`*24.0f`；`inputGainDb` 默认值 4→12（对应 0.5 位置）。

#### 8.8 第二列：Waveshaper 9 档 → 15 档
- 新增 `2_10`~`2_15` 六种波形整形（`Column2Mode` 枚举 + `waveshapeColumn2()` 分支）：
  - `2_10` 级联过驱 fuzz（多级 tanh）
  - `2_11` 混沌波折叠（迭代正弦映射）
  - `2_12` 正弦波折叠 `sin(πx)`
  - `2_13` 全波整流 `2|x|-1`
  - `2_14` 交越/死区失真（`t=0.15`）
  - `2_15` Chebyshev 5次 `16x⁵-20x³+5x`
- `kColumn2ModeCount` 9→15、`kColumn2MaxIndex` 8→14；响度均衡的离线测量循环自动覆盖 15 档，无需额外改动。
- **2_10/2_11 的迭代**：最初实现为 cubic / 指数二极管，听感偏弱，改为级联过驱与混沌折叠以更贴合"地下故障"美学。

#### 8.9 第三列：Glitch 5 档 → 10 档
- 新增 `3_6`~`3_10` 五种故障效果（`applyColumn3Glitch()` 分支）：
  - `3_6` 颗粒重排（上一拍切 64 颗粒，随机挑颗粒 + 随机正放/倒放）
  - `3_7` 磁带停止（播放头先快后慢指数减速）
  - `3_8` 硬核降采样（1/4 拍保持 + 1-bit 量化）
  - `3_9` 比特翻转（随机翻转 IEEE754 符号/尾数位）
  - `3_10` 扫频环调（正弦载波扫频）
- `kColumn3MaxIndex` 4→9；`applyColumn3Glitch` 与 `processBlock` 的 `col3ModeIdx` 上限同步。
- **UI 可视化同步**：Editor `paint` 里第三列故障波形的 `switch(modeIdx)` 新增 5 个 case（3_6~3_10）。
- **3_9 安全边界**：只翻转符号位 + 尾数位（不碰指数位），`isfinite` 兜底 + clamp ±1.5，避免 `inf/NaN`。

#### 8.10 本轮涉及文件
| 文件 | 本轮改动 |
| --- | --- |
| [PluginProcessor.h](/I:/rorrerror/grid_plugin/include/Grid/PluginProcessor.h) | `Column1Mode`/`Column2Mode` 枚举扩展；新增 `column1DcBlockers`/`column1DcR`；`kColumn2ModeCount`=15 |
| [PluginProcessor.cpp](/I:/rorrerror/grid_plugin/source/PluginProcessor.cpp) | 新增 `asymClip`/`clipColumn1`；`waveshapeColumn2` 加 6 分支；`applyColumn3Glitch` 加 5 分支；`kColumn1/2/3MaxIndex` 与增益上限更新 |
| [PluginEditor.cpp](/I:/rorrerror/grid_plugin/source/PluginEditor.cpp) | 加载 `1_3`/`1_4`/`2_10`~`2_15`/`3_6`~`3_10` 贴图；三列映射与增益上限同步；第三列可视化 switch 加 5 case |
| [CMakeLists.txt](/I:/rorrerror/CMakeLists.txt) | BinaryData 新增 `1_3`/`1_4`/`2_10`~`2_15`/`3_6`~`3_10` |

---

### 第三轮：宿主参数暴露（自动化 / MIDI CC）+ 窗口大小持久化（v1.2.0）

> 本轮把三列底部横向控制条通过 `AudioProcessorValueTreeState` 暴露给宿主，使 DAW 能对其做自动化或 MIDI CC 映射；并用 `PropertiesFile` 持久化插件窗口大小，解决关闭再打开被重置的问题。版本号 `1.1.0` → `1.2.0`。

#### 8.11 需求一：三列底部控制条暴露给宿主（自动化 / MIDI CC）
- **背景**：原三个底部控制条（输入增益 / Waveshaper 干湿比 / Glitch 周期）只能打开界面手动调整，未暴露给宿主，DAW 无法做自动化或 MIDI Learn 映射。
- **方案**：改用 JUCE 标准 `AudioProcessorValueTreeState`（APVTS）注册 3 个宿主参数：
  - `inputGain`（Input Gain，0~24 dB）
  - `waveshaperWet`（Waveshaper Wet，0~1）
  - `glitchPeriod`（Glitch Period，0~1，底层仍由 `mapSlider2ToGlitchPeriodBeats()` 映射到 7 点离散）
- **双向同步闭环**：
  - 宿主 → 插件：`parameters.addParameterListener(paramID, this)`（JUCE 8.0.12 新 API，替代旧 `addListener`）→ `parameterChanged` 收到**实际值（denormalised）**，转回归一化写入内部 `std::atomic` → 音频线程下一 block 生效（保留原有一阶平滑，不爆音）。
  - 插件 → 宿主：Editor 拖动控制条后调 `setParameterValueFromUi()` → `setValueNotifyingHost()`（接收 **0~1 归一化值**）→ 宿主参数面板 / 自动化实时更新。
- **值语义要点**（易错）：`AudioProcessorParameter::setValue` / `setValueNotifyingHost` 接收 0~1 归一化值；`Listener::parameterChanged` 的 `newValue` 是实际值。
- **状态持久化**：`setStateInformation` 恢复工程时用 `setValue()` **静默**同步 APVTS（避免触发宿主回调）。
- **UI 跟随**：Editor `timerCallback` 里轮询 processor 原子值同步到 `sliderNorm`，宿主侧改动也能实时反映到界面。

#### 8.12 需求二：窗口大小持久化
- **背景**：宿主中关闭再打开插件会重置窗口大小。
- **方案**：`juce::PropertiesFile`（进程级有意泄漏单例，遵循 §6.3）保存到 `%APPDATA%\iisaacbeats\rorrerror.settings`；构造时 `loadWindowSize()` 恢复、`resized()` 里 `saveWindowSize()`、析构时 `saveIfNeeded()` 落盘。
- **踩坑与修复**：
  - `AudioProcessorEditor::setResizeLimits()` 内部会 `setBoundsConstrained(getBounds())`。若在 `setSize` 之前调用（组件还是 0×0），会被强行拉到最小值，并把错误尺寸写进设置文件，导致窗口"特别小"且"保存不了"。
  - 修复：先 `setSize` 再 `setResizeLimits`；新增 `windowSizeSavingEnabled` 开关，构造期间禁止保存，构造完成后才置 `true`；最小尺寸设 400（默认 720×540 = `background.png` 1440×1080 减半）。

#### 8.13 版本号升级 1.1.0 → 1.2.0
- 按 §6.6 三处同步 + 额外一致性同步：`CMakeLists.txt`（project VERSION + `juce_add_plugin` VERSION）、`rorrerror_installer.iss`（MyAppVersion）、`PluginEditor.cpp`（About 弹窗硬编码版本）、`grid_plugin.h`（模块 version）、`build_installer.bat`（APP_VERSION 与注释）、`README.md`（badge + 产物名）、本文件 §1.1。
- 更新检查用 `JucePlugin_VersionString` 自动跟随 CMake，无需手动改。

#### 8.14 本轮涉及文件
| 文件 | 本轮改动 |
| --- | --- |
| [PluginProcessor.h](/I:/rorrerror/grid_plugin/include/Grid/PluginProcessor.h) | 继承 `AudioProcessorValueTreeState::Listener`；新增 `parameters` 成员、`getAPVTS()`/`setParameterValueFromUi()`/`parameterChanged()`/`createParameterLayout()` |
| [PluginProcessor.cpp](/I:/rorrerror/grid_plugin/source/PluginProcessor.cpp) | 构造函数初始化 APVTS + `addParameterListener`×3；参数创建与双向同步；`setStateInformation` 静默同步 APVTS |
| [PluginEditor.h](/I:/rorrerror/grid_plugin/include/Grid/PluginEditor.h) | 新增 `saveWindowSize()`/`loadWindowSize()` 声明、`windowSizeSavingEnabled` 标志 |
| [PluginEditor.cpp](/I:/rorrerror/grid_plugin/source/PluginEditor.cpp) | 控制条拖动后通知宿主；`timerCallback` 轮询同步 UI；`PropertiesFile` 窗口大小持久化；About 弹窗版本 1.2.0 |
| [CMakeLists.txt](/I:/rorrerror/CMakeLists.txt) | 版本号 1.2.0 |
| [rorrerror_installer.iss](/I:/rorrerror/rorrerror_installer.iss) | MyAppVersion 1.2.0 |
