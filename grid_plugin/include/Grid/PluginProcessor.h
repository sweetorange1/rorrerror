#pragma once

#include <atomic>
#include <vector>
#include <cstdint>

#include <juce_audio_processors/juce_audio_processors.h>

namespace grid {

class PluginProcessor : public juce::AudioProcessor,
                             public juce::AudioProcessorValueTreeState::Listener {
public:
  enum class Column1Mode : int {
    SoftClip = 0,  // 1_1.png
    HardClip = 1,  // 1_2.png
    FoldClip = 2,  // 1_3.png
    AsymClip = 3,  // 1_4.png
  };

  enum class Column2Mode : int {
    Shape_2_1 = 0,
    Shape_2_2 = 1,
    Shape_2_3 = 2,
    Shape_2_4 = 3,
    Shape_2_5 = 4,
    Shape_2_6 = 5,
    Shape_2_7 = 6,
    Shape_2_8 = 7,
    Shape_2_9 = 8,
    Shape_2_10 = 9,
    Shape_2_11 = 10,
    Shape_2_12 = 11,
    Shape_2_13 = 12,
    Shape_2_14 = 13,
    Shape_2_15 = 14,
  };

  PluginProcessor();

  void prepareToPlay(double sampleRate, int expectedMaxFramesPerBlock) override;

  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
  using AudioProcessor::processBlock;

  void releaseResources() override;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override;

  const juce::String getName() const override;

  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  double getTailLengthSeconds() const override;

  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int index, const juce::String& newName) override;

  void getStateInformation(juce::MemoryBlock& destData) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

  void setColumn1Mode(Column1Mode mode) noexcept;
  Column1Mode getColumn1Mode() const noexcept;

  void setColumn2Mode(Column2Mode mode) noexcept;
  Column2Mode getColumn2Mode() const noexcept;

  void setColumn2Wet(float wet01) noexcept;
  float getColumn2Wet() const noexcept;

  void setInputGainDb(float db) noexcept;
  float getInputGainDb() const noexcept;

  // --- UI状态（用于Editor重开与宿主工程保存） ---
  void setColumn1SelectedIndex(int index) noexcept;
  int getColumn1SelectedIndex() const noexcept;

  void setColumn2SelectedIndex(int index) noexcept;
  int getColumn2SelectedIndex() const noexcept;

  void setColumn3SelectedIndex(int index) noexcept;
  int getColumn3SelectedIndex() const noexcept;

  void setSlider0Norm(float norm01) noexcept;
  float getSlider0Norm() const noexcept;

  void setSlider1Norm(float norm01) noexcept;
  float getSlider1Norm() const noexcept;

  void setSlider2Norm(float norm01) noexcept;
  float getSlider2Norm() const noexcept;

  // --- Column2（Waveshaper UI预览）---
  // 用于Editor绘制函数曲线：返回当前第二列waveshaper在输入x处的输出y。
  // 注意：这是“纯公式映射”，不包含wet混合/高通/第三列glitch/第一列clip。
  float evaluateColumn2Waveshaper(float x) const noexcept;

  // --- Column3（Glitch UI反馈）---
  // 用于Editor显示“当前是否触发”：事件计数用于捕捉短促触发，amount/phase用于做闪光与进度。
  std::uint32_t getColumn3GlitchEventCounter() const noexcept;
  float getColumn3GlitchUiAmount01() const noexcept;
  float getColumn3GlitchUiPhase01() const noexcept;

  // --- Scratch（搓碟）---
  // enabled=true时，输出会从“最近录到的音频环形缓冲”中读取，
  // rate>0 正放，rate<0 倒放；abs(rate)为速度倍数。
  void setScratchEnabled(bool enabled) noexcept;
  void setScratchRate(float rate) noexcept;

  // --- 宿主自动化 / MIDI CC 参数桥接 ---
  // 三个底部横向控制条通过 AudioProcessorValueTreeState 暴露给宿主，
  // 使 DAW 可以对其做自动化或 MIDI CC 映射。
  juce::AudioProcessorValueTreeState& getAPVTS() noexcept { return parameters; }

  // UI 交互（拖动控制条）后调用：把最新值通知给宿主（触发宿主参数面板/自动化刷新）。
  void setParameterValueFromUi(const juce::String& parameterID, float value);

private:
  // 宿主参数变化（自动化 / CC / 参数面板）→ 同步到内部原子状态（供音频线程读取）。
  void parameterChanged(const juce::String& parameterID, float newValue) override;

  static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

  // 宿主参数树：把三列底部控制条暴露给宿主（自动化 / MIDI CC）。
  juce::AudioProcessorValueTreeState parameters;

  // 2_3专用：55Hz 高通（12dB/oct = 二阶）
  struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void reset() noexcept {
      z1 = 0.0f;
      z2 = 0.0f;
    }

    float process(float x) noexcept {
      // Direct Form II Transposed
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  // waveshaper输出的直流偏置去除（一阶高通，约12Hz）
  struct DcBlocker {
    float x1 = 0.0f;
    float y1 = 0.0f;

    void reset() noexcept {
      x1 = 0.0f;
      y1 = 0.0f;
    }

    float process(float x, float r) noexcept {
      const float y = x - x1 + r * y1;
      x1 = x;
      y1 = y;
      return y;
    }
  };

  std::vector<Biquad> column2Shape23HP;
  int lastColumn2ModeForShape23HP = 0;

  // Column2响度均衡：每个waveshaper的归一化增益（prepareToPlay计算）
  static constexpr int kColumn2ModeCount = 15;  // 2_1..2_15
  float column2ModeGain[kColumn2ModeCount] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                              1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};

  // wet平滑（音频线程），避免拖动湿音控制条时产生阶跃电流声
  float column2WetSmoothed = 0.5f;

  // Column2波形切换交叉淡化：拖动/随机切换时，平滑地在新旧waveshaper输出之间过渡，
  // 避免函数跳变导致的电流声/爆音。
  Column2Mode column2ActiveMode = Column2Mode::Shape_2_1;
  float column2ModeCrossfade = 1.0f;  // 1.0=过渡完成（active模式完全生效）

  // Column1软/硬削波切换交叉淡化：随机切换时平滑过渡，避免爆音
  Column1Mode column1ActiveMode = Column1Mode::SoftClip;
  float column1ModeCrossfade = 1.0f;  // 1.0=过渡完成（active模式完全生效）

  // 输入增益平滑（音频线程），避免控制条阶跃产生爆音
  float inputGainSmoothed = 1.0f;

  // Column2动态响度补偿：跟踪干/湿信号短时功率，动态调整湿信号增益，
  // 使各waveshaper在不同输入电平下响度尽量一致，同时保留波形谐波结构。
  float column2AutoTrim = 1.0f;
  float column2DryPower = 0.0f;
  float column2WetPower = 0.0f;

  std::vector<DcBlocker> column2DcBlockers;
  // 波形切换交叉淡化期间，用于处理“新目标模式”的独立DC blocker，
  // 避免对混合后的直流阶跃产生瞬态电流声。
  std::vector<DcBlocker> column2DcBlockersNew;
  float column2DcR = 0.0f;

  // 第一列输出级 DC blocker：1_4 非对称削波会产生直流偏置，
  // 在交叉淡化混合之后统一去除（其余模式奇函数输出基本无直流，影响可忽略）。
  std::vector<DcBlocker> column1DcBlockers;
  float column1DcR = 0.0f;

  std::atomic<int> column1Mode{ static_cast<int>(Column1Mode::SoftClip) };
  std::atomic<int> column2Mode{ static_cast<int>(Column2Mode::Shape_2_1) };

  // 0..1，默认50%
  std::atomic<float> column2Wet{ 0.5f };

  // 输入增益 dB（slider0：0..24dB；默认0.5 => 12dB）
  std::atomic<float> inputGainDb{ 12.0f };

  // 选择索引（用于恢复UI）：默认居中1_1/2_1/3_1
  std::atomic<int> column1SelectedIndex{ 0 };
  std::atomic<int> column2SelectedIndex{ 0 };
  // 第三列默认：3_5
  std::atomic<int> column3SelectedIndex{ 4 };

  // 控制条0..1（用于恢复UI）
  std::atomic<float> slider0Norm{ 0.5f };  // +8dB
  std::atomic<float> slider1Norm{ 0.5f };  // waveshaper wet
  // 第三列默认：每2拍触发一次（7点离散：idx=1 / (7-1)=1/6）
  std::atomic<float> slider2Norm{ 0.5f };

  // scratch控制（UI线程->音频线程）
  std::atomic<int> scratchEnabled{ 0 };
  std::atomic<float> scratchRate{ 0.0f };

  // scratch环形缓冲（音频线程使用；prepareToPlay初始化）
  double scratchSampleRate = 44100.0;
  int scratchChannels = 0;
  int scratchBufferSize = 0;  // samples
  int scratchWritePos = 0;
  double scratchPlayPos = 0.0; // 可为小数

  // 平滑切换，避免按下/抬起时点击
  float scratchMix = 0.0f; // 0=直通 1=全scratch

  bool scratchWasOn = false;

  std::vector<float> scratchBufferInterleaved;

  // --- Column3（Glitch）---
  // 说明：第三列处理发生在第二列之后、软/硬削波之前。
  // glitch周期由slider2Norm控制（映射到0.5..32拍），在“周期的最后一拍”内生效。
  double column3SampleRate = 44100.0;
  double column3PpqFallback = 0.0;
  double column3BpmFallback = 120.0;

  int column3Channels = 0;
  int column3SamplesPerBeat = 0;
  int column3WritePos = 0;                // 当前拍写入位置（sample index）
  long long column3BeatIndex = 0;         // floor(ppq)
  bool column3HavePrevBeat = false;

  // interleaved: [sampleIndex * channels + ch]
  std::vector<float> column3BeatPrevInterleaved;
  std::vector<float> column3BeatCurInterleaved;

  // 平滑切换（避免过度爆音；同时保留glitch味道）
  float column3GlitchMix = 0.0f; // 0=直通 1=全glitch
  bool column3WasActive = false;
  std::uint32_t column3EventCounter = 0;

  // UI线程读取（原子，避免竞态）
  std::atomic<std::uint32_t> column3UiEventCounter{ 0 };
  std::atomic<float> column3UiAmount01{ 0.0f };
  std::atomic<float> column3UiPhase01{ 0.0f };

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginProcessor)
};

}  // namespace grid