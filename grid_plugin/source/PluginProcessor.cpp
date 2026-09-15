#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "../include/Grid/PluginProcessor.h"
#include "../include/Grid/PluginEditor.h"

#include "network/UpdateChecker.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace grid {

static constexpr float kInputGainDbMin = 0.0f;
static constexpr float kInputGainDbMax = 24.0f;

static constexpr int kColumn1MaxIndex = 3;  // 1_1..1_4
static constexpr int kColumn2MaxIndex = 14;  // 2_1..2_15
static constexpr int kColumn3MaxIndex = 9;  // 3_1..3_10

static inline float softClip(float x) {
  // 简洁稳定的软削波：tanh
  return std::tanh(x);
}

static inline float hardClip(float x) {
  return juce::jlimit(-1.0f, 1.0f, x);
}

static inline float wrapPositiveF(float v, float m) {
  if (m <= 0.0f)
    return 0.0f;
  v = std::fmod(v, m);
  if (v < 0.0f)
    v += m;
  return v;
}

static inline float foldback(float x, float limit) {
  // 折返失真：超过limit后镜像折回；输出范围约[-limit, limit]
  // 用wrap避免while循环（更稳定/更快）
  if (limit <= 0.0f)
    return 0.0f;
  const float span = 4.0f * limit;
  float t = wrapPositiveF(x + limit, span); // [0, 4*limit)
  if (t > 2.0f * limit)
    t = span - t;
  return t - limit;
}

static inline float asymClip(float x) {
  // 1_4：非对称二极管式削波（正半周软、负半周硬），产生偶次谐波（暖/糙）。
  // 正负半周 drive 不同会带来直流偏置，由输出级 DC blocker 统一去除。
  const float k = (x >= 0.0f) ? 1.3f : 2.0f;
  return std::tanh(k * x);
}

static inline float clipColumn1(PluginProcessor::Column1Mode mode, float x) {
  switch (mode) {
    case PluginProcessor::Column1Mode::HardClip:
      return hardClip(x);
    case PluginProcessor::Column1Mode::FoldClip:
      return foldback(x, 1.0f);
    case PluginProcessor::Column1Mode::AsymClip:
      return asymClip(x);
    case PluginProcessor::Column1Mode::SoftClip:
    default:
      return softClip(x);
  }
}

static inline float readRingLinear(const float* ring, int ringSize, double pos) {
  // pos允许为任意实数；wrap到[0, ringSize)
  if (ringSize <= 0)
    return 0.0f;

  double p = std::fmod(pos, static_cast<double>(ringSize));
  if (p < 0.0)
    p += static_cast<double>(ringSize);

  const int i0 = static_cast<int>(p);
  const int i1 = (i0 + 1) % ringSize;
  const float frac = static_cast<float>(p - static_cast<double>(i0));
  return ring[i0] + (ring[i1] - ring[i0]) * frac;
}

static inline float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

static inline std::uint32_t hash32(std::uint32_t x) {
  // xorshift32
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

static inline float mapSlider2ToGlitchPeriodBeats(float norm01) {
  norm01 = juce::jlimit(0.0f, 1.0f, norm01);
  // 第三列离散7档（从左到右）：
  // 0: 持续触发
  // 1: 每2拍
  // 2: 每4拍
  // 3: 每8拍
  // 4: 每16拍
  // 5: 每32拍
  // 6: 永不触发
  const int idx = juce::jlimit(0, 6, static_cast<int>(std::lround(norm01 * 6.0f)));
  switch (idx) {
    case 0: return -1.0f;      // special: continuous
    case 1: return 2.00f;
    case 2: return 4.00f;
    case 3: return 8.00f;
    case 4: return 16.0f;
    case 5: return 32.0f;
    default: return 0.0f;      // never
  }
}

static inline float readBeatInterleaved(const std::vector<float>& buf, int samplesPerBeat, int channels, int sampleIndex, int ch) {
  if (samplesPerBeat <= 0 || channels <= 0 || buf.empty())
    return 0.0f;
  sampleIndex = juce::jlimit(0, samplesPerBeat - 1, sampleIndex);
  ch = juce::jlimit(0, channels - 1, ch);
  const size_t idx = static_cast<size_t>(sampleIndex * channels + ch);
  if (idx >= buf.size())
    return 0.0f;
  return buf[idx];
}

static inline float applyColumn3Glitch(
    int modeIndex,
    int ch,
    int windowSampleIndex,
    int samplesPerBeat,
    int channels,
    const std::vector<float>& prevBeat,
    std::uint32_t eventSeed) {
  modeIndex = juce::jlimit(0, 9, modeIndex);

  // 3_1：缓冲区重复（上一拍“开头”四次）
  if (modeIndex == 0) {
    const int seg = juce::jmax(1, samplesPerBeat / 4);
    const int idx = windowSampleIndex % seg;
    return readBeatInterleaved(prevBeat, samplesPerBeat, channels, idx, ch);
  }

  // 3_2：消音错误
  if (modeIndex == 1) {
    return 0.0f;
  }

  // 3_3：反向切片（上一拍前半拍倒放并重复）
  if (modeIndex == 2) {
    const int seg = juce::jmax(1, samplesPerBeat / 2);
    const int p = windowSampleIndex % seg;
    const int idx = (seg - 1) - p;
    return readBeatInterleaved(prevBeat, samplesPerBeat, channels, idx, ch);
  }

  // 3_4：Sample&Hold / bitcrush glitch（带少量符号翻转）
  if (modeIndex == 3) {
    const int hold = juce::jmax(1, samplesPerBeat / 48);
    const int step = windowSampleIndex / hold;
    const int idx = juce::jlimit(0, samplesPerBeat - 1, step * hold);
    float s = readBeatInterleaved(prevBeat, samplesPerBeat, channels, idx, ch);

    const std::uint32_t h = hash32(eventSeed ^ static_cast<std::uint32_t>(step * 2654435761u + ch * 97u));
    if ((h & 1u) != 0u)
      s = -s;

    // 小幅“过驱”，让故障更明显
    s *= 1.15f;
    return juce::jlimit(-1.2f, 1.2f, s);
  }

  // 3_6：颗粒重排（上一拍切成64个细颗粒，随机挑颗粒 + 随机正放/倒放）
  if (modeIndex == 5) {
    const int grainLen = juce::jmax(1, samplesPerBeat / 64);
    const int grain = windowSampleIndex / grainLen;
    const int inGrain = windowSampleIndex % grainLen;

    const std::uint32_t h = hash32(eventSeed + static_cast<std::uint32_t>(grain * 2654435761u + ch * 97u));
    const int pick = static_cast<int>(h % 64u);
    const bool reverse = ((h >> 8) & 1u) != 0u;
    const int base = pick * grainLen;
    const int idx = reverse ? (grainLen - 1 - inGrain) : inGrain;
    return readBeatInterleaved(prevBeat, samplesPerBeat, channels, base + idx, ch);
  }

  // 3_7：磁带停止（播放头先快后慢指数减速，最后几乎停住）
  if (modeIndex == 6) {
    const float t = static_cast<float>(windowSampleIndex) / static_cast<float>(juce::jmax(1, samplesPerBeat));
    const float u = 1.0f - t;
    const float pos01 = 1.0f - u * u * u;  // 先快后慢
    const int idx = juce::jlimit(0, samplesPerBeat - 1, static_cast<int>(pos01 * static_cast<float>(samplesPerBeat)));
    return readBeatInterleaved(prevBeat, samplesPerBeat, channels, idx, ch);
  }

  // 3_8：硬核降采样（1/4拍长保持 + 1-bit量化，只留 ±1）
  if (modeIndex == 7) {
    const int hold = juce::jmax(1, samplesPerBeat / 4);
    const int step = windowSampleIndex / hold;
    const int idx = juce::jlimit(0, samplesPerBeat - 1, step * hold);
    const float s = readBeatInterleaved(prevBeat, samplesPerBeat, channels, idx, ch);
    return (s >= 0.0f) ? 1.0f : -1.0f;
  }

  // 3_9：比特翻转（随机翻转采样值 IEEE754 符号/尾数位，产生爆音/数字噪声）
  if (modeIndex == 8) {
    const float s = readBeatInterleaved(prevBeat, samplesPerBeat, channels, windowSampleIndex, ch);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &s, sizeof(bits));
    const std::uint32_t h = hash32(eventSeed + static_cast<std::uint32_t>(windowSampleIndex * 1013904223u + ch * 131u));
    bits ^= (h & 0x807FFFFFu);  // 只翻符号位 + 尾数位，不碰指数位（避免 inf/NaN）
    float out = 0.0f;
    std::memcpy(&out, &bits, sizeof(out));
    if (!std::isfinite(out))
      out = 0.0f;
    return juce::jlimit(-1.5f, 1.5f, out);
  }

  // 3_10：扫频环调（正弦载波从低扫到高，金属环调/射频干扰）
  if (modeIndex == 9) {
    const float t = static_cast<float>(windowSampleIndex) / static_cast<float>(juce::jmax(1, samplesPerBeat));
    const float cycles = 4.0f + 60.0f * t;  // 每拍 4 -> 64 周期（扫频）
    const float carrier = std::sin(juce::MathConstants<float>::twoPi * cycles * t);
    const float s = readBeatInterleaved(prevBeat, samplesPerBeat, channels, windowSampleIndex, ch);
    return juce::jlimit(-1.2f, 1.2f, s * carrier);
  }

  // 3_5：随机切片跳读（上一拍被切成16片，最后一拍内随机跳片）
  {
    const int sliceLen = juce::jmax(1, samplesPerBeat / 16);
    const int slice = windowSampleIndex / sliceLen;
    const int inSlice = windowSampleIndex % sliceLen;

    const std::uint32_t h = hash32(eventSeed + static_cast<std::uint32_t>(slice * 1013904223u + ch * 131u));
    const int slicePick = static_cast<int>(h % 16u);
    const int base = slicePick * sliceLen;
    const int idx = juce::jlimit(0, samplesPerBeat - 1, base + inSlice);
    return readBeatInterleaved(prevBeat, samplesPerBeat, channels, idx, ch);
  }
}

static inline float waveshapeColumn2(PluginProcessor::Column2Mode mode, float x) {
  // 说明：此处的x是“前置增益之后”的信号；后面还会进入软/硬削波。
  // 2_1：锯齿（round误差形成锯齿感）
  // (round((-x/2)*(0.2*10+1))-(-x/2)*(0.2*10+1))*2
  if (mode == PluginProcessor::Column2Mode::Shape_2_1) {
    const float a = (-x * 0.5f) * (0.2f * 10.0f + 1.0f);
    return (std::round(a) - a) * 2.0f;
  }

  // 2_2：梯形（量化台阶）
  // round((((32*((1-0.92)*5)^2)+0.5))*x)/((32*((1-0.92)*5)^2)+0.5)
  if (mode == PluginProcessor::Column2Mode::Shape_2_2) {
    const float q = (32.0f * std::pow((1.0f - 0.92f) * 5.0f, 2.0f)) + 0.5f;
    return std::round(q * x) / q;
  }

  // 2_3：三角（把x折叠到一个“来回”的三角形轨迹）
  // (1 - abs(fmod(x+1,2) - 1)) * 2 - 1
  if (mode == PluginProcessor::Column2Mode::Shape_2_3) {
    float t = wrapPositiveF(x + 1.0f, 2.0f);
    return (1.0f - std::abs(t - 1.0f)) * 2.0f - 1.0f;
  }

  // 2_4：正弦饱和（单调S曲线）
  // sin((pi/2) * x)
  if (mode == PluginProcessor::Column2Mode::Shape_2_4) {
    // 更疯狂：相位扭曲 + 全波整流（非单调、富谐波）
    // y = tanh(1.8 * (2*abs(sin(2pi*(x + 0.25*sin(7x)))) - 1))
    const float u = x + 0.25f * std::sin(7.0f * x);
    const float s = std::sin(juce::MathConstants<float>::twoPi * u);
    const float r = 2.0f * std::abs(s) - 1.0f;
    return std::tanh(1.8f * r);
  }

  // 2_5：tanh驱动（更“模拟”的压缩）
  // tanh(2.2*x)/tanh(2.2)
  if (mode == PluginProcessor::Column2Mode::Shape_2_5) {
    // 更疯狂：foldback（明显的“折叠/尖锐”质感）
    // y = foldback(2.8*x, 1)
    return foldback(2.8f * x, 1.0f);
  }

  // 2_6：atan驱动（平滑但更硬一点）
  // (2/pi)*atan(3*x)
  if (mode == PluginProcessor::Column2Mode::Shape_2_6) {
    // 更疯狂：自调制正弦 + 幂次扩展 + 量化
    // y = round( q * sign(s)*|s|^0.25 ) / q
    const float s = std::sin(9.0f * x + 2.0f * std::sin(2.5f * x));
    const float y = std::copysign(std::pow(std::abs(s), 0.25f), s);
    constexpr float q = 11.0f;
    return std::round(q * y) / q;
  }

  // 2_7：三次软削波（经典奇函数）
  // x - (1/3)*x^3
  if (mode == PluginProcessor::Column2Mode::Shape_2_7) {
    // 更疯狂：先多项式扩展，再用sin做波折叠，最后加一点自FM
    float y = x * (1.0f + 1.6f * x * x);              // 扩展（更容易“折”）
    y = std::sin(juce::MathConstants<float>::halfPi * y); // 波折叠
    y += 0.35f * std::sin(6.0f * y);                  // 自调制
    return juce::jlimit(-1.2f, 1.2f, y);
  }

  // 2_8：Chebyshev 3次（更强的三次谐波感）
  // 4*x^3 - 3*x
  if (mode == PluginProcessor::Column2Mode::Shape_2_8) {
    // 更疯狂：类似“ring/AM + 频率倍增”的质感（更像破碎的金属声）
    const float a = 1.0f + 0.85f * std::abs(x);
    float y = std::sin(juce::MathConstants<float>::twoPi * x * a);
    y *= (1.0f - 0.35f * std::cos(juce::MathConstants<float>::twoPi * x));
    return y;
  }

  // 2_10：级联过驱（fuzz）—— 多级 tanh 级联，把波形推向方波，谐波密集
  if (mode == PluginProcessor::Column2Mode::Shape_2_10) {
    float y = std::tanh(3.0f * x);
    y = std::tanh(4.0f * (y + 0.6f * x));
    y = std::tanh(5.0f * y);
    return y;
  }

  // 2_11：混沌波折叠 —— 迭代正弦映射，谐波密集且不可预测（接近噪声感）
  if (mode == PluginProcessor::Column2Mode::Shape_2_11) {
    float y = std::sin(juce::MathConstants<float>::pi * 2.4f * x);
    y = std::sin(juce::MathConstants<float>::pi * (1.5f * y + 0.9f * x));
    y = std::sin(juce::MathConstants<float>::pi * (1.5f * y + 0.9f * x));
    return y;
  }

  // 2_12：正弦波折叠
  // y = sin(pi * x)
  if (mode == PluginProcessor::Column2Mode::Shape_2_12) {
    return std::sin(juce::MathConstants<float>::pi * x);
  }

  // 2_13：全波整流（频率翻倍，产生八度感）
  // y = 2*|x| - 1
  if (mode == PluginProcessor::Column2Mode::Shape_2_13) {
    return 2.0f * std::abs(x) - 1.0f;
  }

  // 2_14：交越/死区失真（小信号静音，大信号才通过）
  // |x|<=t => 0；否则 sign(x)*(|x|-t)/(1-t)
  if (mode == PluginProcessor::Column2Mode::Shape_2_14) {
    constexpr float t = 0.15f;
    const float ax = std::abs(x);
    if (ax <= t)
      return 0.0f;
    return std::copysign((ax - t) / (1.0f - t), x);
  }

  // 2_15：Chebyshev 5次多项式（强五次谐波）
  // y = 16*x^5 - 20*x^3 + 5*x
  if (mode == PluginProcessor::Column2Mode::Shape_2_15) {
    const float x2 = x * x;
    return x * (16.0f * x2 * x2 - 20.0f * x2 + 5.0f);
  }

  // 2_9：更粗的量化台阶（bitcrush感）
  // round(16*x)/16
  {
    constexpr float q = 16.0f;
    return std::round(q * x) / q;
  }
}

// 平台标识（与遥测口径一致，<os>-<arch>）
static juce::String GetUpdatePlatformString() {
#if defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
  const juce::String arch = "arm64";
#elif defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
  const juce::String arch = "x64";
#else
  const juce::String arch = "x86";
#endif

#if JUCE_WINDOWS
  return "win-" + arch;
#elif JUCE_MAC
  return "mac-" + arch;
#elif JUCE_LINUX
  return "linux-" + arch;
#else
  return "unknown";
#endif
}

PluginProcessor::PluginProcessor()
    : juce::AudioProcessor(
          BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
  // 启动时延迟 5 秒检查一次更新（进程级去重，避免多实例重复触发）
  static std::atomic<bool> updateOnceFlag{false};
  if (!updateOnceFlag.exchange(true, std::memory_order_acquire)) {
    juce::Timer::callAfterDelay(5000, [] {
      grid::network::CheckForUpdatesAsync(
          "rorrerror",
          juce::String(JucePlugin_VersionString),
          GetUpdatePlatformString(),
          [](const grid::network::UpdateInfo& info) {
            if (info.has_update) {
              grid::network::ShowUpdateDialog(info);
            }
          });
    });
  }
}

void PluginProcessor::setScratchEnabled(bool enabled) noexcept {
  scratchEnabled.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

void PluginProcessor::setScratchRate(float rate) noexcept {
  // 避免极端值造成噪声/CPU问题
  rate = juce::jlimit(-4.0f, 4.0f, rate);
  // deadzone：接近0时直接归0
  if (std::abs(rate) < 0.02f)
    rate = 0.0f;
  scratchRate.store(rate, std::memory_order_relaxed);
}

void PluginProcessor::setColumn1Mode(Column1Mode mode) noexcept {
  int v = static_cast<int>(mode);
  v = juce::jlimit(0, kColumn1MaxIndex, v);
  column1Mode.store(v, std::memory_order_relaxed);
}

PluginProcessor::Column1Mode PluginProcessor::getColumn1Mode() const noexcept {
  int v = column1Mode.load(std::memory_order_relaxed);
  v = juce::jlimit(0, kColumn1MaxIndex, v);
  return static_cast<Column1Mode>(v);
}

void PluginProcessor::setColumn2Mode(Column2Mode mode) noexcept {
  int v = static_cast<int>(mode);
  v = juce::jlimit(0, kColumn2MaxIndex, v);
  column2Mode.store(v, std::memory_order_relaxed);
}

PluginProcessor::Column2Mode PluginProcessor::getColumn2Mode() const noexcept {
  int v = column2Mode.load(std::memory_order_relaxed);
  v = juce::jlimit(0, kColumn2MaxIndex, v);
  return static_cast<Column2Mode>(v);
}

void PluginProcessor::setColumn2Wet(float wet01) noexcept {
  wet01 = juce::jlimit(0.0f, 1.0f, wet01);
  column2Wet.store(wet01, std::memory_order_relaxed);
}

float PluginProcessor::getColumn2Wet() const noexcept {
  return column2Wet.load(std::memory_order_relaxed);
}

void PluginProcessor::setInputGainDb(float db) noexcept {
  db = juce::jlimit(kInputGainDbMin, kInputGainDbMax, db);
  inputGainDb.store(db, std::memory_order_relaxed);
}

float PluginProcessor::getInputGainDb() const noexcept {
  return inputGainDb.load(std::memory_order_relaxed);
}

void PluginProcessor::setColumn1SelectedIndex(int index) noexcept {
  index = juce::jlimit(0, kColumn1MaxIndex, index);
  column1SelectedIndex.store(index, std::memory_order_relaxed);
  setColumn1Mode(static_cast<Column1Mode>(index));
}

int PluginProcessor::getColumn1SelectedIndex() const noexcept {
  int v = column1SelectedIndex.load(std::memory_order_relaxed);
  return juce::jlimit(0, kColumn1MaxIndex, v);
}

void PluginProcessor::setColumn2SelectedIndex(int index) noexcept {
  index = juce::jlimit(0, kColumn2MaxIndex, index);
  column2SelectedIndex.store(index, std::memory_order_relaxed);
  setColumn2Mode(static_cast<Column2Mode>(index));
}

int PluginProcessor::getColumn2SelectedIndex() const noexcept {
  int v = column2SelectedIndex.load(std::memory_order_relaxed);
  return juce::jlimit(0, kColumn2MaxIndex, v);
}

void PluginProcessor::setColumn3SelectedIndex(int index) noexcept {
  index = juce::jlimit(0, kColumn3MaxIndex, index);
  column3SelectedIndex.store(index, std::memory_order_relaxed);
}

int PluginProcessor::getColumn3SelectedIndex() const noexcept {
  int v = column3SelectedIndex.load(std::memory_order_relaxed);
  return juce::jlimit(0, kColumn3MaxIndex, v);
}

void PluginProcessor::setSlider0Norm(float norm01) noexcept {
  norm01 = juce::jlimit(0.0f, 1.0f, norm01);
  slider0Norm.store(norm01, std::memory_order_relaxed);
  setInputGainDb(norm01 * 24.0f);
}

float PluginProcessor::getSlider0Norm() const noexcept {
  return slider0Norm.load(std::memory_order_relaxed);
}

void PluginProcessor::setSlider1Norm(float norm01) noexcept {
  norm01 = juce::jlimit(0.0f, 1.0f, norm01);
  slider1Norm.store(norm01, std::memory_order_relaxed);
  setColumn2Wet(norm01);
}

float PluginProcessor::getSlider1Norm() const noexcept {
  return slider1Norm.load(std::memory_order_relaxed);
}

void PluginProcessor::setSlider2Norm(float norm01) noexcept {
  norm01 = juce::jlimit(0.0f, 1.0f, norm01);
  slider2Norm.store(norm01, std::memory_order_relaxed);
}

float PluginProcessor::getSlider2Norm() const noexcept {
  return slider2Norm.load(std::memory_order_relaxed);
}

float PluginProcessor::evaluateColumn2Waveshaper(float x) const noexcept {
  // UI绘制用：仅评估第二列waveshaper的“纯映射曲线”
  // （不包含wet/高通/第三列/第一列clip）
  x = juce::jlimit(-1.0f, 1.0f, x);
  const float y = waveshapeColumn2(getColumn2Mode(), x);
  if (!std::isfinite(y))
    return 0.0f;
  return juce::jlimit(-1.5f, 1.5f, y);
}

std::uint32_t PluginProcessor::getColumn3GlitchEventCounter() const noexcept {
  return column3UiEventCounter.load(std::memory_order_relaxed);
}

float PluginProcessor::getColumn3GlitchUiAmount01() const noexcept {
  return column3UiAmount01.load(std::memory_order_relaxed);
}

float PluginProcessor::getColumn3GlitchUiPhase01() const noexcept {
  return column3UiPhase01.load(std::memory_order_relaxed);
}

const juce::String PluginProcessor::getName() const {
  return GRID_PLUGIN_NAME;
}

bool PluginProcessor::acceptsMidi() const { return false; }

bool PluginProcessor::producesMidi() const { return false; }

bool PluginProcessor::isMidiEffect() const { return false; }

double PluginProcessor::getTailLengthSeconds() const { return 0.0; }

int PluginProcessor::getNumPrograms() {
  // 某些宿主在0个program时处理不佳
  return 1;
}

int PluginProcessor::getCurrentProgram() { return 0; }

void PluginProcessor::setCurrentProgram(int index) {
  juce::ignoreUnused(index);
}

const juce::String PluginProcessor::getProgramName(int index) {
  juce::ignoreUnused(index);
  return {};
}

void PluginProcessor::changeProgramName(int index, const juce::String& newName) {
  juce::ignoreUnused(index, newName);
}

void PluginProcessor::prepareToPlay(double sampleRate, int expectedMaxFramesPerBlock) {
  juce::ignoreUnused(expectedMaxFramesPerBlock);

  scratchSampleRate = (sampleRate > 0.0 ? sampleRate : 44100.0);
  scratchChannels = juce::jmax(1, getTotalNumInputChannels());

  // Column3 glitch基础状态
  column3SampleRate = scratchSampleRate;
  column3Channels = scratchChannels;
  column3GlitchMix = 0.0f;
  column3WasActive = false;
  column3EventCounter = 0;
  column3UiEventCounter.store(0, std::memory_order_relaxed);
  column3UiAmount01.store(0.0f, std::memory_order_relaxed);
  column3UiPhase01.store(0.0f, std::memory_order_relaxed);
  column3HavePrevBeat = false;
  column3WritePos = 0;
  column3BeatIndex = 0;

  // 用一个合理默认BPM初始化拍缓冲（真正运行时会按宿主BPM动态调整）
  column3BpmFallback = 120.0;
  column3PpqFallback = 0.0;
  {
    const double spb = column3SampleRate * 60.0 / juce::jmax(1.0, column3BpmFallback);
    column3SamplesPerBeat = juce::jlimit(16, static_cast<int>(column3SampleRate * 4.0), static_cast<int>(std::round(spb)));
    const size_t n = static_cast<size_t>(column3SamplesPerBeat * column3Channels);
    column3BeatPrevInterleaved.assign(n, 0.0f);
    column3BeatCurInterleaved.assign(n, 0.0f);
  }

  // 2_3专用高通：55Hz / 12dB/oct（RBJ cookbook, Butterworth Q）
  // 注意：该滤波器只在2_3模式下启用
  {
    const float fs = static_cast<float>(scratchSampleRate);
    const float fc = 55.0f;
    const float Q = 0.70710678f;

    const float w0 = 2.0f * juce::MathConstants<float>::pi * (fc / juce::jmax(1.0f, fs));
    const float cw = std::cos(w0);
    const float sw = std::sin(w0);
    const float alpha = sw / (2.0f * Q);

    float b0 = (1.0f + cw) * 0.5f;
    float b1 = -(1.0f + cw);
    float b2 = (1.0f + cw) * 0.5f;
    const float a0 = 1.0f + alpha;
    float a1 = -2.0f * cw;
    float a2 = 1.0f - alpha;

    // 归一化
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;

    column2Shape23HP.assign(static_cast<size_t>(scratchChannels), {});
    for (auto& f : column2Shape23HP) {
      f.b0 = b0;
      f.b1 = b1;
      f.b2 = b2;
      f.a1 = a1;
      f.a2 = a2;
      f.reset();
    }
    lastColumn2ModeForShape23HP = -1;
  }

  // 缓冲长度：0.6秒（足够搓碟感，又不会太占内存）
  scratchBufferSize = juce::jmax(256, static_cast<int>(scratchSampleRate * 0.6));
  scratchWritePos = 0;
  scratchPlayPos = 0.0;
  scratchMix = 0.0f;
  scratchWasOn = false;

  // Column2响度均衡 + 直流偏置去除
  // 思路：对每个waveshaper输出做一阶高通去除DC，再用固定增益把全wet时RMS
  // 对齐到参考正弦（A=0.5）的干信号RMS，从而尽量平衡各效果的响度，
  // 同时保留各自波形（谐波结构/风味）不变。
  {
    constexpr int kLoudnessSamples = 16384;
    constexpr float kRefAmplitude = 0.5f;
    constexpr float kTestFreq = 997.0f;
    const float fs = static_cast<float>(scratchSampleRate);
    const float targetRms = kRefAmplitude / std::sqrt(2.0f);

    // 一阶高通DC blocker：约12Hz，兼顾低音保留与直流去除
    column2DcR = 1.0f - (2.0f * juce::MathConstants<float>::pi * 12.0f / fs);
    column2DcR = juce::jlimit(0.9f, 0.9999f, column2DcR);
    column2DcBlockers.assign(static_cast<size_t>(scratchChannels), DcBlocker{});
    column2DcBlockersNew.assign(static_cast<size_t>(scratchChannels), DcBlocker{});

    // 第一列输出级 DC blocker（复用相同的 12Hz 一阶高通系数）
    column1DcR = column2DcR;
    column1DcBlockers.assign(static_cast<size_t>(scratchChannels), DcBlocker{});

    for (int mode = 0; mode < kColumn2ModeCount; ++mode) {
      const auto wsMode = static_cast<Column2Mode>(mode);
      DcBlocker meas;
      double sum = 0.0;
      double sumSq = 0.0;
      for (int i = 0; i < kLoudnessSamples; ++i) {
        const float phase =
            2.0f * juce::MathConstants<float>::pi * kTestFreq * static_cast<float>(i) / fs;
        const float x = kRefAmplitude * std::sin(phase);
        const float y = waveshapeColumn2(wsMode, x);
        const float yb = meas.process(y, column2DcR);
        sum += yb;
        sumSq += static_cast<double>(yb) * static_cast<double>(yb);
      }

      const double mean = sum / static_cast<double>(kLoudnessSamples);
      const double meanSq = sumSq / static_cast<double>(kLoudnessSamples);
      const double var = juce::jmax(0.0, meanSq - mean * mean);
      const double rms = std::sqrt(var);

      float gain = 1.0f;
      if (rms > 1.0e-6) {
        gain = static_cast<float>(targetRms / rms);
      }
      column2ModeGain[mode] = juce::jlimit(0.125f, 8.0f, gain);
    }

    // 与当前wet对齐，避免prepare后首帧从默认值跳变
    column2WetSmoothed = getColumn2Wet();

    // 波形切换交叉淡化状态复位
    column2ActiveMode = getColumn2Mode();
    column2ModeCrossfade = 1.0f;
    column1ActiveMode = getColumn1Mode();
    column1ModeCrossfade = 1.0f;

    // 输入增益与动态响度补偿状态复位
    inputGainSmoothed = juce::Decibels::decibelsToGain(getInputGainDb());
    column2AutoTrim = 1.0f;
    column2DryPower = 0.0f;
    column2WetPower = 0.0f;
  }

  scratchBufferInterleaved.assign(static_cast<size_t>(scratchBufferSize * scratchChannels), 0.0f);
}

void PluginProcessor::releaseResources() {}

bool PluginProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) {
    return false;
  }

  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet()) {
    return false;
  }

  return true;
}

void PluginProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                  juce::MidiBuffer& midiMessages) {
  juce::ignoreUnused(midiMessages);

  juce::ScopedNoDenormals noDenormals;

  const auto totalNumInputChannels = getTotalNumInputChannels();
  const auto totalNumOutputChannels = getTotalNumOutputChannels();

  // 清理多余输出通道
  for (int channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel) {
    buffer.clear(channel, 0, buffer.getNumSamples());
  }

  const int n = buffer.getNumSamples();

  const float inputGainTarget = juce::Decibels::decibelsToGain(getInputGainDb());

  const Column2Mode wsMode = getColumn2Mode();
  const float wetTarget = getColumn2Wet();

  // Column3 glitch参数
  const int col3ModeIdx = juce::jlimit(0, 9, getColumn3SelectedIndex());
  const float glitchPeriodBeats = mapSlider2ToGlitchPeriodBeats(getSlider2Norm());
  const bool glitchNever = (glitchPeriodBeats == 0.0f);
  const bool glitchContinuous = (glitchPeriodBeats < 0.0f);
  const float glitchPeriodBeatsAbs = glitchContinuous ? 1.0f : glitchPeriodBeats;

  // 对于<1拍的周期，窗口按周期的末尾1/4（避免“全程都在glitch”）；>=1拍则窗口=最后1拍。
  const float glitchWindowBeats = glitchContinuous
      ? 1.0f
      : ((glitchPeriodBeatsAbs >= 1.0f) ? 1.0f : (glitchPeriodBeatsAbs * 0.25f));

  // 2_3专用高通：当进入/退出2_3时重置状态，避免切换时的“记忆/砰声”
  if (!column2Shape23HP.empty()) {
    const int cur = static_cast<int>(wsMode);
    if (cur != lastColumn2ModeForShape23HP) {
      const bool oldIs23 = (lastColumn2ModeForShape23HP == static_cast<int>(Column2Mode::Shape_2_3));
      const bool newIs23 = (wsMode == Column2Mode::Shape_2_3);
      if (oldIs23 || newIs23) {
        for (auto& f : column2Shape23HP)
          f.reset();
      }
      lastColumn2ModeForShape23HP = cur;
    }
  }

  const Column1Mode clipMode = getColumn1Mode();

  // 读取宿主节拍信息（尽量用playhead；没有则用fallback）
  double bpm = column3BpmFallback;
  double ppqStart = column3PpqFallback;
  bool isPlaying = true;

  if (auto* ph = getPlayHead()) {
#if JUCE_MAJOR_VERSION >= 6
    if (auto posOpt = ph->getPosition()) {
      const auto& pos = *posOpt;
      if (auto b = pos.getBpm())
        bpm = *b;
      if (auto p = pos.getPpqPosition())
        ppqStart = *p;
      isPlaying = pos.getIsPlaying();
    } else {
      isPlaying = false;
    }
#else
    juce::AudioPlayHead::CurrentPositionInfo pos;
    if (ph->getCurrentPosition(pos)) {
      if (pos.bpm > 1.0)
        bpm = pos.bpm;
      ppqStart = pos.ppqPosition;
      isPlaying = pos.isPlaying;
    } else {
      isPlaying = false;
    }
#endif
  }

  bpm = std::isfinite(bpm) ? bpm : column3BpmFallback;
  if (bpm < 1.0)
    bpm = column3BpmFallback;

  const double sr = scratchSampleRate;
  // wet平滑时间约25ms，避免控制条阶跃产生爆音/电流声
  const float wetSmoothCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(sr) * 0.025f));
  // 输入增益平滑时间约20ms
  const float inputGainSmoothCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(sr) * 0.020f));
  // Column2动态响度补偿的包络与增益平滑时间
  const float column2PowerCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(sr) * 0.030f));
  const float column2TrimAttackCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(sr) * 0.030f));
  const float column2TrimReleaseCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(sr) * 0.150f));
  // Column2波形切换交叉淡化时间约20ms
  const float column2CrossfadeStep = 1.0f / juce::jmax(1.0f, static_cast<float>(sr) * 0.020f);
  // Column1软/硬削波切换交叉淡化时间约20ms
  const float column1CrossfadeStep = 1.0f / juce::jmax(1.0f, static_cast<float>(sr) * 0.020f);
  const double samplesPerBeat = sr * 60.0 / bpm; // 1 beat = 1 quarter note
  const int spbI = juce::jlimit(16, static_cast<int>(sr * 4.0), static_cast<int>(std::round(samplesPerBeat)));

  // tempo变化时重建拍缓冲（用于3_1/3_3/3_5等读取“上一拍”）
  if (spbI != column3SamplesPerBeat || column3Channels != scratchChannels) {
    column3SamplesPerBeat = spbI;
    column3Channels = scratchChannels;
    const size_t nBuf = static_cast<size_t>(column3SamplesPerBeat * column3Channels);
    column3BeatPrevInterleaved.assign(nBuf, 0.0f);
    column3BeatCurInterleaved.assign(nBuf, 0.0f);
    column3WritePos = 0;
    column3BeatIndex = static_cast<long long>(std::floor(ppqStart));
    column3HavePrevBeat = false;
  }

  // block结束后更新fallback（用于无playhead或极端宿主）
  column3BpmFallback = bpm;
  if (isPlaying)
    column3PpqFallback = ppqStart + (static_cast<double>(n) / juce::jmax(1.0, samplesPerBeat));

  const bool scratchOn = scratchEnabled.load(std::memory_order_relaxed) != 0;
  const float rate = scratchRate.load(std::memory_order_relaxed);

  // scratch从关->开：对齐playPos到“刚录到的附近”，听起来更像在刮同一段声音
  if (scratchOn && !scratchWasOn) {
    const int back = juce::jlimit(1, scratchBufferSize - 1, static_cast<int>(scratchSampleRate * 0.20));
    int start = scratchWritePos - back;
    start %= scratchBufferSize;
    if (start < 0)
      start += scratchBufferSize;
    scratchPlayPos = static_cast<double>(start);
  }
  scratchWasOn = scratchOn;

  // scratchMix 平滑：大约15ms达到目标
  const float mixTarget = scratchOn ? 1.0f : 0.0f;
  const float mixStep = 1.0f / juce::jmax(1.0f, static_cast<float>(scratchSampleRate * 0.015));

  // UI采样：记录本block的最后相位/强度
  float uiLastPhase01 = 0.0f;
  float uiLastAmount01 = 0.0f;

  // 确保scratch缓冲已初始化
  if (scratchBufferSize <= 0 || scratchChannels <= 0 ||
      static_cast<int>(scratchBufferInterleaved.size()) < scratchBufferSize * scratchChannels) {
    // 兜底：不scratch（仍然保留Column2 + Column3 glitch + clip链路）
    const float mixStepCol3 = 1.0f / juce::jmax(1.0f, static_cast<float>(sr * 0.003));
    for (int i = 0; i < n; ++i) {
      column2WetSmoothed += (wetTarget - column2WetSmoothed) * wetSmoothCoeff;
      const float wet = column2WetSmoothed;

      inputGainSmoothed += (inputGainTarget - inputGainSmoothed) * inputGainSmoothCoeff;
      const float g = inputGainSmoothed;

      // 波形切换交叉淡化：每sample推进一次，让新旧waveshaper输出平滑过渡
      const bool column2TargetChanged = (wsMode != column2ActiveMode);
      if (column2TargetChanged && column2ModeCrossfade >= 1.0f) {
        // 上一个过渡已完成，启动新的过渡：重置目标模式DC blocker
        column2ModeCrossfade = 0.0f;
        for (auto& f : column2DcBlockersNew)
          f.reset();
      }
      const bool column2Transitioning = column2TargetChanged || (column2ModeCrossfade < 1.0f);
      const float column2Mix = column2ModeCrossfade;
      if (column2Transitioning) {
        column2ModeCrossfade = juce::jmin(1.0f, column2ModeCrossfade + column2CrossfadeStep);
        if (column2ModeCrossfade >= 1.0f) {
          column2ModeCrossfade = 1.0f;
          column2DcBlockers = column2DcBlockersNew;
          column2ActiveMode = wsMode;
        }
      }
      const float column2BaseGainOld = column2ModeGain[static_cast<int>(column2ActiveMode)];
      const float column2BaseGainNew = column2ModeGain[static_cast<int>(wsMode)];
      const float column2BaseGain = column2BaseGainOld + (column2BaseGainNew - column2BaseGainOld) * column2Mix;

      // Column1软/硬削波切换交叉淡化：每sample推进一次
      const bool column1Transitioning = (clipMode != column1ActiveMode) || (column1ModeCrossfade < 1.0f);
      const float column1Mix = column1ModeCrossfade;
      if (column1Transitioning) {
        column1ModeCrossfade = juce::jmin(1.0f, column1ModeCrossfade + column1CrossfadeStep);
        if (column1ModeCrossfade >= 1.0f)
          column1ActiveMode = clipMode;
      }

      const double beatPos = ppqStart + (static_cast<double>(i) / juce::jmax(1.0, samplesPerBeat));
      const long long beatIdx = static_cast<long long>(std::floor(beatPos));
      if (beatIdx != column3BeatIndex) {
        column3BeatPrevInterleaved.swap(column3BeatCurInterleaved);
        std::fill(column3BeatCurInterleaved.begin(), column3BeatCurInterleaved.end(), 0.0f);
        column3WritePos = 0;
        column3BeatIndex = beatIdx;
        column3HavePrevBeat = true;
      }

      bool glitchActive = false;
      int windowSampleIndex = 0;

      if (isPlaying && !glitchNever) {
        if (glitchContinuous) {
          glitchActive = true;
          const float beatFrac = static_cast<float>(beatPos - std::floor(beatPos));
          windowSampleIndex = juce::jlimit(0, column3SamplesPerBeat - 1,
                                           static_cast<int>(beatFrac * static_cast<float>(column3SamplesPerBeat)));
          uiLastPhase01 = beatFrac;
        } else {
          const float p = glitchPeriodBeatsAbs;
          float phase = std::fmod(static_cast<float>(beatPos), p);
          if (phase < 0.0f)
            phase += p;
          uiLastPhase01 = (p > 0.0f) ? (phase / p) : 0.0f;

          const float winBeats = juce::jlimit(0.001f, p, glitchWindowBeats);
          const float start = p - winBeats;
          if (phase >= start) {
            glitchActive = true;
            const float local01 = (phase - start) / juce::jmax(1.0e-6f, winBeats);
            const float spanSamples = static_cast<float>(column3SamplesPerBeat) * winBeats;
            windowSampleIndex = juce::jlimit(0, column3SamplesPerBeat - 1,
                                             static_cast<int>(local01 * juce::jmax(1.0f, spanSamples)));
          }
        }
      }

      if (glitchActive && !column3WasActive)
        ++column3EventCounter;
      column3WasActive = glitchActive;

      // 事件计数同步给UI（用于捕捉极短触发）
      column3UiEventCounter.store(column3EventCounter, std::memory_order_relaxed);

      const float targetMix = glitchActive ? 1.0f : 0.0f;
      if (column3GlitchMix < targetMix)
        column3GlitchMix = juce::jmin(targetMix, column3GlitchMix + mixStepCol3);
      else if (column3GlitchMix > targetMix)
        column3GlitchMix = juce::jmax(targetMix, column3GlitchMix - mixStepCol3);

      uiLastAmount01 = column3GlitchMix;

      float xCh[2] = { 0.0f, 0.0f };
      float yDcCh[2] = { 0.0f, 0.0f };
      for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        const float x = buffer.getReadPointer(ch)[i] * g;
        xCh[ch] = x;

        float yDc;
        if (column2Transitioning) {
          const float yOldRaw = waveshapeColumn2(column2ActiveMode, x);
          const float yNewRaw = waveshapeColumn2(wsMode, x);
          float yOldDc = yOldRaw;
          float yNewDc = yNewRaw;
          if (!column2DcBlockers.empty() && !column2DcBlockersNew.empty()) {
            yOldDc = column2DcBlockers[static_cast<size_t>(ch % static_cast<int>(column2DcBlockers.size()))].process(
                yOldRaw, column2DcR);
            yNewDc = column2DcBlockersNew[static_cast<size_t>(ch % static_cast<int>(column2DcBlockersNew.size()))].process(
                yNewRaw, column2DcR);
          }
          yDc = yOldDc + (yNewDc - yOldDc) * column2Mix;
        } else {
          const float yRaw = waveshapeColumn2(column2ActiveMode, x);
          yDc = yRaw;
          if (!column2DcBlockers.empty()) {
            yDc = column2DcBlockers[static_cast<size_t>(ch % static_cast<int>(column2DcBlockers.size()))].process(
                yRaw, column2DcR);
          }
        }
        yDcCh[ch] = yDc;
      }

      // 动态响度补偿：用干/湿短时功率估计调整wet增益
      {
        float dryP = 0.0f;
        float wetP = 0.0f;
        for (int ch = 0; ch < totalNumInputChannels; ++ch) {
          dryP += xCh[ch] * xCh[ch];
          wetP += yDcCh[ch] * yDcCh[ch];
        }
        const float invCh = 1.0f / static_cast<float>(juce::jmax(1, totalNumInputChannels));
        dryP *= invCh;
        wetP *= invCh;
        column2DryPower += (dryP - column2DryPower) * column2PowerCoeff;
        column2WetPower += (wetP - column2WetPower) * column2PowerCoeff;

        constexpr float kPowerFloor = 1.0e-8f;
        if (column2DryPower > kPowerFloor) {
          const float totalGain = std::sqrt(column2DryPower / juce::jmax(column2WetPower, kPowerFloor));
          const float targetTrim = totalGain / juce::jmax(0.001f, column2BaseGain);
          const float a = (targetTrim < column2AutoTrim) ? column2TrimAttackCoeff : column2TrimReleaseCoeff;
          column2AutoTrim += (targetTrim - column2AutoTrim) * a;
          column2AutoTrim = juce::jlimit(0.1f, 4.0f, column2AutoTrim);
        }
      }

      const float yGain = juce::jlimit(0.05f, 8.0f, column2BaseGain * column2AutoTrim);
      for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        const float x = xCh[ch];
        const float yNorm = yDcCh[ch] * yGain;
        float preClip = x + (yNorm - x) * wet;
        if (column2ActiveMode == Column2Mode::Shape_2_3 && !column2Shape23HP.empty()) {
          preClip = column2Shape23HP[static_cast<size_t>(ch % static_cast<int>(column2Shape23HP.size()))].process(preClip);
        }

        // 写入“当前拍”缓冲（用于后续读上一拍）
        if (column3WritePos < column3SamplesPerBeat && ch < column3Channels) {
          const size_t w = static_cast<size_t>(column3WritePos * column3Channels + (ch % column3Channels));
          if (w < column3BeatCurInterleaved.size())
            column3BeatCurInterleaved[w] = preClip;
        }

        // 第三列 glitch（第二列之后、clip之前）
        float postCol3 = preClip;
        if (column3GlitchMix > 0.0001f && column3HavePrevBeat) {
          const float gSig = applyColumn3Glitch(col3ModeIdx, ch % column3Channels,
                                                windowSampleIndex, column3SamplesPerBeat, column3Channels,
                                                column3BeatPrevInterleaved, column3EventCounter);
          postCol3 = lerp(preClip, gSig, column3GlitchMix);
        } else if (column3GlitchMix > 0.0001f && col3ModeIdx == 1) {
          // 3_2消音：即使还没有prevBeat，也允许静音
          postCol3 = preClip * (1.0f - column3GlitchMix);
        }

        float clipped;
        if (column1Transitioning) {
          const float cOld = clipColumn1(column1ActiveMode, postCol3);
          const float cNew = clipColumn1(clipMode, postCol3);
          clipped = cOld + (cNew - cOld) * column1Mix;
        } else {
          clipped = clipColumn1(column1ActiveMode, postCol3);
        }
        // 输出级 DC blocker（去除 1_4 非对称削波的直流偏置）
        if (!column1DcBlockers.empty()) {
          clipped = column1DcBlockers[static_cast<size_t>(ch % static_cast<int>(column1DcBlockers.size()))].process(clipped, column1DcR);
        }
        buffer.getWritePointer(ch)[i] = clipped;
      }

      // 每个sample推进一次拍内写指针
      if (column3WritePos < column3SamplesPerBeat)
        ++column3WritePos;
    }

    // block末尾：把UI需要的相位/强度写入原子（Editor按60Hz读取）
    column3UiAmount01.store(juce::jlimit(0.0f, 1.0f, uiLastAmount01), std::memory_order_relaxed);
    column3UiPhase01.store(juce::jlimit(0.0f, 1.0f, uiLastPhase01), std::memory_order_relaxed);
    return;
  }

  // Scratch + 原链路：scratch读取发生在“效果器链路之前”，
  // 然后同样进入 waveshaper/clip，这样搓碟时也会反映当前预设。
  const float mixStepCol3 = 1.0f / juce::jmax(1.0f, static_cast<float>(sr * 0.003));
  for (int i = 0; i < n; ++i) {
    column2WetSmoothed += (wetTarget - column2WetSmoothed) * wetSmoothCoeff;
    const float wet = column2WetSmoothed;

    inputGainSmoothed += (inputGainTarget - inputGainSmoothed) * inputGainSmoothCoeff;
    const float g = inputGainSmoothed;

    // 波形切换交叉淡化：每sample推进一次，让新旧waveshaper输出平滑过渡
    const bool column2TargetChanged = (wsMode != column2ActiveMode);
    if (column2TargetChanged && column2ModeCrossfade >= 1.0f) {
      // 上一个过渡已完成，启动新的过渡：重置目标模式DC blocker
      column2ModeCrossfade = 0.0f;
      for (auto& f : column2DcBlockersNew)
        f.reset();
    }
    const bool column2Transitioning = column2TargetChanged || (column2ModeCrossfade < 1.0f);
    const float column2Mix = column2ModeCrossfade;
    if (column2Transitioning) {
      column2ModeCrossfade = juce::jmin(1.0f, column2ModeCrossfade + column2CrossfadeStep);
      if (column2ModeCrossfade >= 1.0f) {
        column2ModeCrossfade = 1.0f;
        column2DcBlockers = column2DcBlockersNew;
        column2ActiveMode = wsMode;
      }
    }
    const float column2BaseGainOld = column2ModeGain[static_cast<int>(column2ActiveMode)];
    const float column2BaseGainNew = column2ModeGain[static_cast<int>(wsMode)];
    const float column2BaseGain = column2BaseGainOld + (column2BaseGainNew - column2BaseGainOld) * column2Mix;

    // Column1软/硬削波切换交叉淡化：每sample推进一次
    const bool column1Transitioning = (clipMode != column1ActiveMode) || (column1ModeCrossfade < 1.0f);
    const float column1Mix = column1ModeCrossfade;
    if (column1Transitioning) {
      column1ModeCrossfade = juce::jmin(1.0f, column1ModeCrossfade + column1CrossfadeStep);
      if (column1ModeCrossfade >= 1.0f)
        column1ActiveMode = clipMode;
    }

    const double beatPos = ppqStart + (static_cast<double>(i) / juce::jmax(1.0, samplesPerBeat));
    const long long beatIdx = static_cast<long long>(std::floor(beatPos));
    if (beatIdx != column3BeatIndex) {
      column3BeatPrevInterleaved.swap(column3BeatCurInterleaved);
      std::fill(column3BeatCurInterleaved.begin(), column3BeatCurInterleaved.end(), 0.0f);
      column3WritePos = 0;
      column3BeatIndex = beatIdx;
      column3HavePrevBeat = true;
    }

    bool glitchActive = false;
    int windowSampleIndex = 0;

    if (isPlaying && !glitchNever) {
      if (glitchContinuous) {
        glitchActive = true;
        const float beatFrac = static_cast<float>(beatPos - std::floor(beatPos));
        windowSampleIndex = juce::jlimit(0, column3SamplesPerBeat - 1,
                                         static_cast<int>(beatFrac * static_cast<float>(column3SamplesPerBeat)));
        uiLastPhase01 = beatFrac;
      } else {
        const float p = glitchPeriodBeatsAbs;
        float phase = std::fmod(static_cast<float>(beatPos), p);
        if (phase < 0.0f)
          phase += p;
        uiLastPhase01 = (p > 0.0f) ? (phase / p) : 0.0f;

        const float winBeats = juce::jlimit(0.001f, p, glitchWindowBeats);
        const float start = p - winBeats;
        if (phase >= start) {
          glitchActive = true;
          const float local01 = (phase - start) / juce::jmax(1.0e-6f, winBeats);
          const float spanSamples = static_cast<float>(column3SamplesPerBeat) * winBeats;
          windowSampleIndex = juce::jlimit(0, column3SamplesPerBeat - 1,
                                           static_cast<int>(local01 * juce::jmax(1.0f, spanSamples)));
        }
      }
    }

    if (glitchActive && !column3WasActive)
      ++column3EventCounter;
    column3WasActive = glitchActive;

    column3UiEventCounter.store(column3EventCounter, std::memory_order_relaxed);

    const float targetMix = glitchActive ? 1.0f : 0.0f;
    if (column3GlitchMix < targetMix)
      column3GlitchMix = juce::jmin(targetMix, column3GlitchMix + mixStepCol3);
    else if (column3GlitchMix > targetMix)
      column3GlitchMix = juce::jmax(targetMix, column3GlitchMix - mixStepCol3);

    uiLastAmount01 = column3GlitchMix;

    // 淡入淡出更新
    if (scratchMix < mixTarget)
      scratchMix = juce::jmin(mixTarget, scratchMix + mixStep);
    else if (scratchMix > mixTarget)
      scratchMix = juce::jmax(mixTarget, scratchMix - mixStep);

    // 1) 先取实时输入并写入环形缓冲（写入的是“前置增益之后”的信号，便于搓碟更明显）
    float xIn[2] = { 0.0f, 0.0f };
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
      float v = buffer.getReadPointer(ch)[i] * g;
      xIn[ch] = v;

      const int c = ch % scratchChannels;
      scratchBufferInterleaved[static_cast<size_t>(scratchWritePos * scratchChannels + c)] = v;
    }

    scratchWritePos++;
    if (scratchWritePos >= scratchBufferSize)
      scratchWritePos = 0;

    // 2) 从环形缓冲读取scratch信号
    float xScr[2] = { 0.0f, 0.0f };
    if (scratchMix > 0.0001f) {
      // 以“最近历史”为基准读取；rate决定播放头移动方向/速度
      for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        const int c = ch % scratchChannels;

        auto sampleAt = [&](int idx) -> float {
          idx %= scratchBufferSize;
          if (idx < 0)
            idx += scratchBufferSize;
          return scratchBufferInterleaved[static_cast<size_t>(idx * scratchChannels + c)];
        };

        double pp = std::fmod(scratchPlayPos, static_cast<double>(scratchBufferSize));
        if (pp < 0.0)
          pp += static_cast<double>(scratchBufferSize);

        const int i0 = static_cast<int>(pp);
        const int i1 = (i0 + 1) % scratchBufferSize;
        const float frac = static_cast<float>(pp - static_cast<double>(i0));
        const float s0 = sampleAt(i0);
        const float s1 = sampleAt(i1);
        xScr[ch] = s0 + (s1 - s0) * frac;
      }

      // rate==0 时不移动播放头，相当于“停住/按住”
      if (rate != 0.0f)
        scratchPlayPos += static_cast<double>(rate);
    }

    // 3) 混合：scratchMix=1时完全用scratch，否则直通
    float xCh[2] = { 0.0f, 0.0f };
    float yDcCh[2] = { 0.0f, 0.0f };
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
      float x = xIn[ch] + (xScr[ch] - xIn[ch]) * scratchMix;
      xCh[ch] = x;

      // 第二列 waveshaper（波形切换期间分别去直流后再交叉淡化）
      float yDc;
      if (column2Transitioning) {
        const float yOldRaw = waveshapeColumn2(column2ActiveMode, x);
        const float yNewRaw = waveshapeColumn2(wsMode, x);
        float yOldDc = yOldRaw;
        float yNewDc = yNewRaw;
        if (!column2DcBlockers.empty() && !column2DcBlockersNew.empty()) {
          yOldDc = column2DcBlockers[static_cast<size_t>(ch % static_cast<int>(column2DcBlockers.size()))].process(
              yOldRaw, column2DcR);
          yNewDc = column2DcBlockersNew[static_cast<size_t>(ch % static_cast<int>(column2DcBlockersNew.size()))].process(
              yNewRaw, column2DcR);
        }
        yDc = yOldDc + (yNewDc - yOldDc) * column2Mix;
      } else {
        const float yRaw = waveshapeColumn2(column2ActiveMode, x);
        yDc = yRaw;
        if (!column2DcBlockers.empty()) {
          yDc = column2DcBlockers[static_cast<size_t>(ch % static_cast<int>(column2DcBlockers.size()))].process(
              yRaw, column2DcR);
        }
      }
      yDcCh[ch] = yDc;
    }

    // 动态响度补偿（与兜底路径保持一致）
    {
      float dryP = 0.0f;
      float wetP = 0.0f;
      for (int ch = 0; ch < totalNumInputChannels; ++ch) {
        dryP += xCh[ch] * xCh[ch];
        wetP += yDcCh[ch] * yDcCh[ch];
      }
      const float invCh = 1.0f / static_cast<float>(juce::jmax(1, totalNumInputChannels));
      dryP *= invCh;
      wetP *= invCh;
      column2DryPower += (dryP - column2DryPower) * column2PowerCoeff;
      column2WetPower += (wetP - column2WetPower) * column2PowerCoeff;

      constexpr float kPowerFloor = 1.0e-8f;
      if (column2DryPower > kPowerFloor) {
        const float totalGain = std::sqrt(column2DryPower / juce::jmax(column2WetPower, kPowerFloor));
        const float targetTrim = totalGain / juce::jmax(0.001f, column2BaseGain);
        const float a = (targetTrim < column2AutoTrim) ? column2TrimAttackCoeff : column2TrimReleaseCoeff;
        column2AutoTrim += (targetTrim - column2AutoTrim) * a;
        column2AutoTrim = juce::jlimit(0.1f, 4.0f, column2AutoTrim);
      }
    }

    const float yGain = juce::jlimit(0.05f, 8.0f, column2BaseGain * column2AutoTrim);
    for (int ch = 0; ch < totalNumInputChannels; ++ch) {
      const float x = xCh[ch];
      const float yNorm = yDcCh[ch] * yGain;
      float mixed2 = x + (yNorm - x) * wet;
      if (column2ActiveMode == Column2Mode::Shape_2_3 && !column2Shape23HP.empty()) {
        mixed2 = column2Shape23HP[static_cast<size_t>(ch % static_cast<int>(column2Shape23HP.size()))].process(mixed2);
      }

      // 写入“当前拍”缓冲（用于后续读上一拍）
      if (column3WritePos < column3SamplesPerBeat && ch < column3Channels) {
        const size_t w = static_cast<size_t>(column3WritePos * column3Channels + (ch % column3Channels));
        if (w < column3BeatCurInterleaved.size())
          column3BeatCurInterleaved[w] = mixed2;
      }

      // 第三列 glitch（第二列之后、clip之前）
      float postCol3 = mixed2;
      if (column3GlitchMix > 0.0001f && column3HavePrevBeat) {
        const float gSig = applyColumn3Glitch(col3ModeIdx, ch % column3Channels,
                                              windowSampleIndex, column3SamplesPerBeat, column3Channels,
                                              column3BeatPrevInterleaved, column3EventCounter);
        postCol3 = lerp(mixed2, gSig, column3GlitchMix);
      } else if (column3GlitchMix > 0.0001f && col3ModeIdx == 1) {
        postCol3 = mixed2 * (1.0f - column3GlitchMix);
      }

      // 第一列削波（模式切换期间做交叉淡化）
      float clipped;
      if (column1Transitioning) {
        const float cOld = clipColumn1(column1ActiveMode, postCol3);
        const float cNew = clipColumn1(clipMode, postCol3);
        clipped = cOld + (cNew - cOld) * column1Mix;
      } else {
        clipped = clipColumn1(column1ActiveMode, postCol3);
      }
      // 输出级 DC blocker（去除 1_4 非对称削波的直流偏置）
      if (!column1DcBlockers.empty()) {
        clipped = column1DcBlockers[static_cast<size_t>(ch % static_cast<int>(column1DcBlockers.size()))].process(clipped, column1DcR);
      }
      buffer.getWritePointer(ch)[i] = clipped;
    }

    if (column3WritePos < column3SamplesPerBeat)
      ++column3WritePos;
  }

  // block末尾：把UI需要的相位/强度写入原子（Editor按60Hz读取）
  column3UiAmount01.store(juce::jlimit(0.0f, 1.0f, uiLastAmount01), std::memory_order_relaxed);
  column3UiPhase01.store(juce::jlimit(0.0f, 1.0f, uiLastPhase01), std::memory_order_relaxed);
}

bool PluginProcessor::hasEditor() const { return true; }

juce::AudioProcessorEditor* PluginProcessor::createEditor() {
  return new PluginEditor(*this);
}

void PluginProcessor::getStateInformation(juce::MemoryBlock& destData) {
  // 简单二进制序列化（宿主会持久化这段数据）
  static constexpr int kMagic = 0x47524944;  // 'GRID'
  static constexpr int kVersion = 1;

  juce::MemoryOutputStream s(destData, false);
  s.writeInt(kMagic);
  s.writeInt(kVersion);

  s.writeInt(getColumn1SelectedIndex());
  s.writeInt(getColumn2SelectedIndex());
  s.writeInt(getColumn3SelectedIndex());

  s.writeFloat(getSlider0Norm());
  s.writeFloat(getSlider1Norm());
  s.writeFloat(getSlider2Norm());
}

void PluginProcessor::setStateInformation(const void* data, int sizeInBytes) {
  if (data == nullptr || sizeInBytes <= 0)
    return;

  juce::MemoryInputStream s(data, static_cast<size_t>(sizeInBytes), false);

  static constexpr int kMagic = 0x47524944;  // 'GRID'
  static constexpr int kVersion = 1;

  const int magic = s.readInt();
  const int ver = s.readInt();

  if (magic != kMagic || ver != kVersion)
    return;

  const int c1 = s.readInt();
  const int c2 = s.readInt();
  const int c3 = s.readInt();

  const float sl0 = s.readFloat();
  const float sl1 = s.readFloat();
  const float sl2 = s.readFloat();

  // 注意：setXXX 内部会做clamp，并同步派生的音频参数（gain/wet/mode）
  setColumn1SelectedIndex(c1);
  setColumn2SelectedIndex(c2);
  setColumn3SelectedIndex(c3);

  setSlider0Norm(sl0);
  setSlider1Norm(sl1);
  setSlider2Norm(sl2);
}

}  // namespace grid

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new grid::PluginProcessor();
}