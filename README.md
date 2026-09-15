<h1 align="center">rorrerror</h1>

<p align="center"><strong>老虎机式破坏性失真 / Glitch 效果器</strong> — VST3 / Standalone</p>

<p align="center">
<img src="https://img.shields.io/badge/version-1.1.0-blue" alt="Version">
  <img src="https://img.shields.io/badge/platform-Windows-lightgrey" alt="Platform">
  <img src="https://img.shields.io/badge/framework-JUCE%208.0.12-orange" alt="JUCE">
</p>

---

## 概述

**rorrerror** 是一款面向实验音乐 / 地下电子制作人的**破坏性失真效果插件**。整个插件是一台**三列老虎机**：拖动列即搓碟，拉底部横条即调参，按下随机按钮让三列一起滚动 —— 用赌博机的方式赌你的音轨。

> 分类：Fx / Distortion / Glitch | 插件代码：`Grdb` | 厂商：iisaacbeats.cn

---

## 功能模块

| 模块 | 功能描述 |
|------|---------|
| **第一列 · 削波** | 软削波（tanh）/ 硬削波（±1 限幅），切换带 20ms 交叉淡化 |
| **第二列 · Waveshaper** | 9 种波形整形：锯齿 / 梯形量化 / 三角折叠 / 相位扭曲全波整流 / 折返失真 / 自调制量化 / 波折叠自FM / 环调金属 / 粗量化 bitcrush |
| **第三列 · Glitch** | 5 种节拍同步故障：缓冲重复 / 消音错误 / 反向切片 / Sample&Hold 位压碎 / 随机切片跳读 |
| **搓碟 Scratch** | 拖动任意列 = 从 0.6s 环形缓冲中正放/倒放，拖动速度即播放速率 |
| **随机按钮** | 三列老虎机滚动 + 全链路倒带音效，停点随机 |
| **响度均衡** | 每种 Waveshaper 离线测量归一化增益 + 运行时动态补偿，切换不失响度 |
| **节拍同步** | Glitch 周期锁定宿主 BPM/PPQ：持续 / 每 2·4·8·16·32 拍 / 永不 |

---

## 怎么玩

- **拖列**：像拉老虎机拉杆一样上下拖 → 松手自动吸附，同时触发搓碟（往下拖 = 倒放）
- **拉横条**：三列底部各有一条控制条 —— 左：输入增益 0~8dB ｜ 中：Waveshaper 干湿比 ｜ 右：Glitch 触发周期
- **按随机**：三列一起滚，滚完停在随机组合上 —— 每个组合都是一个"预设"
- **切列不炸**：所有模式切换均做音频级交叉淡化，宿主暂停时 Glitch 自动静默

---

## 技术栈

| 项目 | 版本 |
|------|------|
| 语言 | C++23 |
| 框架 | [JUCE](https://juce.com) 8.0.12（FetchContent 自动拉取） |
| 构建 | CMake ≥ 3.22 |
| 安装器 | Inno Setup 6（Windows） |

---

## 构建

```bash
# 克隆仓库
git clone https://github.com/sweetorange1/rorrerror.git
cd rorrerror

# CMake 配置 & 构建（Release）
# 注意：构建目录必须命名为 cmake-build-release（打包脚本按此路径取产物）
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release --config Release
```

构建成功后 VST3 会自动复制到 `%LOCALAPPDATA%\Programs\Common\VST3`（无需管理员权限）。

## 打包安装器

```bash
# 需要先安装 Inno Setup 6
build_installer.bat
# 产物：dist\rorrerror_Setup_1.1.0_x64.exe
```

安装器将 VST3 装入系统目录 `C:\Program Files\Common Files\VST3\iisaacbeats.cn`；若改选其他目录，安装完成后请在 DAW 中手动添加该目录并重新扫描插件。

---

## 隐私说明

- **更新检查**：启动后异步请求 `iisaacbeats.cn` 一次（5s 超时，失败静默），仅在有新版本时弹窗提示。
- **匿名遥测**：每日一次匿名的"界面打开"事件（无任何音频数据、无个人身份信息），客户端为随机 UUID。
- 停用遥测：启动前设置环境变量 `IISAAC_TELEMETRY_DISABLED=1`。

---

## 许可

本项目代码骨架基于 [The WolfSound](https://thewolfsound.com) 的 JUCE CMake 模板（MIT），业务代码版权归 iisaacbeats.cn 所有。

第三方组件：

| 组件 | 许可 |
|------|------|
| JUCE 8 | GPL-3.0 / 商业双授权 |

---

<p align="center">
  <em>Spin the reels. Break the signal.</em><br>
  &copy; 2024-2026 iisaacbeats.cn
</p>
