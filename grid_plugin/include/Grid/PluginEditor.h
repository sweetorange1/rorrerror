#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace iisaac::telemetry { class Session; }

namespace grid {

class PluginProcessor;

class PluginEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
  explicit PluginEditor(PluginProcessor&);
  ~PluginEditor() override;

  void resized() override;
  void paint(juce::Graphics& g) override;

  void mouseDown(const juce::MouseEvent& e) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseUp(const juce::MouseEvent& e) override;

private:
  struct FloatyParams {
    float rotAmpRad = 0.0f;
    float rotSpeed = 0.0f;

    float scaleAmp = 0.0f;
    float scaleSpeed = 0.0f;

    float posAmpX = 0.0f;
    float posAmpY = 0.0f;
    float posSpeedX = 0.0f;
    float posSpeedY = 0.0f;

    float phase1 = 0.0f;
    float phase2 = 0.0f;
    float phase3 = 0.0f;
    float phase4 = 0.0f;
  };

  PluginProcessor& processor;
  std::unique_ptr<iisaac::telemetry::Session> telemetrySession;

  juce::Image backgroundSpriteSheet;
  int backgroundFrameW = 0;
  int backgroundFrameH = 0;

  juce::Image backgroundFallbackImage;

  // 背景动画每轮随机替换3帧为 glitch 效果帧
  static constexpr int glitchOverrideCount = 3;  // 每轮替换3帧
  static constexpr int glitchVersionCount = 3;   // 每帧 v1/v2/v3 三个版本

  std::array<int, glitchOverrideCount> glitchOverrideFrames{ { -1, -1, -1 } };
  std::array<int, glitchOverrideCount> glitchOverrideVersions{ { 0, 0, 0 } };
  std::array<juce::Image, glitchOverrideCount> glitchOverrideImages;
  std::array<std::atomic<bool>, glitchOverrideCount> glitchOverrideReady{ { false, false, false } };

  int lastBackgroundCycle = -1;

  std::thread glitchDecodeThread;
  std::mutex glitchDecodeMutex;

  static constexpr int columnCount = 3;

  static constexpr int backgroundSheetCols = 10;
  static constexpr int backgroundSheetRows = 6;
  static constexpr float backgroundFps = 20.0f;

  std::array<std::vector<juce::Image>, columnCount> columnImages;
  std::array<std::vector<FloatyParams>, columnCount> floatyParams;

  // 注意：offset 不设上下限；显示端会按周期做 wrap，从而形成循环老虎机效果。
  std::array<float, columnCount> columnOffsets{ { 0.0f, 0.0f, 0.0f } };

  // 三列底部横向控制条（0..1）
  std::array<float, columnCount> sliderNorm{ { 0.5f, 0.5f, 0.5f } };

  // 记录上一次的bounds，用于在窗口缩放时保持当前居中选择不变
  juce::Rectangle<int> lastBoundsForSelection;

  // 窗口大小保存开关：构造期间 setSize/setResizeLimits 会触发 resized，
  // 此时不允许保存，否则会把初始化的中间尺寸写入设置文件。构造完成后才置 true。
  bool windowSizeSavingEnabled = false;

  // 第三列Glitch UI反馈（从Processor读取，timer里更新/衰减）
  std::uint32_t col3LastEventCounter = 0;
  float col3Flash = 0.0f;      // 0..1，触发时瞬间拉满后衰减
  float col3UiPhase01 = 0.0f;  // 0..1
  float col3UiAmount01 = 0.0f; // 0..1

  int activeDragColumn = -1;
  int activeSliderColumn = -1;

  float dragStartY = 0.0f;
  float dragStartOffset = 0.0f;

  // 搓碟：根据拖动速度计算rate
  double scratchLastDragMs = 0.0;
  float scratchLastDragY = 0.0f;
  bool scratchActive = false;

  std::array<bool, columnCount> snapping{ { false, false, false } };
  std::array<float, columnCount> snapTargets{ { 0.0f, 0.0f, 0.0f } };

  double animStartMs = 0.0;
  double animTimeSeconds = 0.0;

  juce::Image randButtonImage;
  juce::Image aboutButtonImage;

  // 随机按钮触发的“老虎机滚动”动画状态
  // 采用“已知终点”的缓动插值：保证最后停在随机目标上，不会出现减速后突然跳变。
  bool slotSpinActive = false;
  bool slotSpinScratchRewindActive = false;
  std::array<bool, columnCount> slotSpinDone{ { false, false, false } };
  std::array<int, columnCount> slotSpinTargetIndex{ { 0, 0, 0 } };

  std::array<float, columnCount> slotSpinStartOffset{ { 0.0f, 0.0f, 0.0f } };
  std::array<float, columnCount> slotSpinTargetOffset{ { 0.0f, 0.0f, 0.0f } };
  std::array<double, columnCount> slotSpinStartTimeMs{ { 0.0, 0.0, 0.0 } };
  std::array<double, columnCount> slotSpinDurationMs{ { 0.0, 0.0, 0.0 } };

  void timerCallback() override;

  void saveWindowSize();
  bool loadWindowSize();

  void updateBackgroundGlitchOverrides();
  void launchGlitchDecode();
  static juce::String makeGlitchResourceName(int frameIndex, int version);
  static juce::Image decodeGlitchFrame(int frameIndex, int version);

  void loadImagesFromBinaryData();
  void initFloatyParams();
  void setInitialCenteredDefaults();

  int getCenteredImageIndex(int columnIndex) const;
  void pushUiStateToProcessor();

  void setSliderNormValue(int columnIndex, float norm);
  int hitTestSlider(juce::Point<float> p) const;
  juce::Rectangle<float> getSliderRect(int columnIndex) const;

  static juce::Image decodePngFromBinary(const void* data, int size);
  static juce::Rectangle<float> getColumnsArea(juce::Rectangle<int> bounds);
  int hitTestColumn(juce::Point<float> p) const;

  void startSnapToNearestCenteredImage(int columnIndex);

  void recenterColumnsToProcessorSelection(juce::Rectangle<int> bounds);

  juce::Rectangle<float> getRandButtonVisualRect() const;
  juce::Rectangle<float> getRandButtonRect() const;
  bool hitTestRandButton(juce::Point<float> p) const;

  juce::Rectangle<float> getAboutButtonVisualRect() const;
  juce::Rectangle<float> getAboutButtonRect() const;
  bool hitTestAboutButton(juce::Point<float> p) const;
  void showAboutDialog();
  void startRandomSlotSpin();
  void updateSlotSpin();
  void startSnapToImageIndex(int columnIndex, int imageIndex);

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginEditor)
};

}  // namespace grid