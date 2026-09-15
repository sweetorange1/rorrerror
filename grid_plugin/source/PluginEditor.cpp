#include "../include/Grid/PluginEditor.h"
#include "../include/Grid/PluginProcessor.h"
#include "../../shared/IisaacTelemetry.h"

#include "BinaryData.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>

namespace grid {

static constexpr int kBackgroundSheetCols = 10;
static constexpr int kBackgroundSheetRows = 6;

static constexpr float kColumnGap = 12.0f;
static constexpr float kColumnsMargin = 24.0f;

static constexpr float kSliderBandH = 34.0f;
static constexpr float kSliderH = 14.0f;
// 上方“老虎机控制器”与下方控制条之间的垂直间距（可按需调整）
static constexpr float kSlotToSliderGap = 28.0f;

// 老虎机“轮盘”透视：中心最大，越远越小，并带一点圆弧横向位移。
static constexpr float kWheelMinScale = 0.48f;
static constexpr float kWheelMaxScale = 1.00f;
static constexpr float kWheelArcX = 18.0f;       // 横向圆弧位移幅度（像是在绕圆形轨迹）
static constexpr float kWheelDepthPow = 1.25f;   // 深度曲线（越大越“中心突出”）
static constexpr float kWheelMinAlpha = 0.55f;   // 远处更淡

static juce::Image decodePngFromBinaryLocal(const void* data, int size) {
  if (data == nullptr || size <= 0)
    return {};

  juce::MemoryInputStream stream(data, static_cast<size_t>(size), false);
  return juce::PNGImageFormat().decodeImage(stream);
}

// 共享缓存：避免每次打开/关闭Editor都重复解码超大的背景图集。
struct BackgroundAnimCache {
  std::mutex mutex;
  juce::Image sheet;
  int frameW = 0;
  int frameH = 0;

  // 0=未开始 1=加载中 2=已就绪 3=失败
  std::atomic<int> state{ 0 };

  std::thread worker;
};

static BackgroundAnimCache& getBackgroundAnimCache() {
  // 重要：插件被宿主卸载/删除时，静态对象析构顺序不可控。
  // 某些宿主会在持有系统/loader锁的情况下卸载DLL，若此时析构释放超大Image或join线程，可能出现卡死。
  // 这里选择“进程级常驻”的缓存（有意泄漏），避免在DLL卸载阶段做任何重型析构工作。
  static auto* cache = new BackgroundAnimCache();
  return *cache;
}

static void tryJoinBackgroundAnimWorkerIfFinished() {
  auto& cache = getBackgroundAnimCache();
  const int st = cache.state.load();
  if (st == 1)
    return;  // still loading

  if (cache.worker.joinable())
    cache.worker.join();
}

static void ensureBackgroundAnimLoading() {
  auto& cache = getBackgroundAnimCache();

  int expected = 0;
  if (!cache.state.compare_exchange_strong(expected, 1))
    return;  // 已经在加载/已完成/失败

  auto* cptr = &cache;

  // 不detach：我们会在“加载完成后尽快join”，避免DLL卸载阶段再处理线程对象。
  cache.worker = std::thread([cptr] {
    auto& c = *cptr;

    auto sheet = decodePngFromBinaryLocal(BinaryData::background_pnglist_png,
                                          BinaryData::background_pnglist_pngSize);

    if (!sheet.isValid()) {
      c.state.store(3);
      return;
    }

    const int w = sheet.getWidth();
    const int h = sheet.getHeight();
    const int frameW = (kBackgroundSheetCols > 0) ? (w / kBackgroundSheetCols) : 0;
    const int frameH = (kBackgroundSheetRows > 0) ? (h / kBackgroundSheetRows) : 0;

    const bool valid = frameW > 0 && frameH > 0 &&
                       w == frameW * kBackgroundSheetCols &&
                       h == frameH * kBackgroundSheetRows;

    if (!valid) {
      c.state.store(3);
      return;
    }

    {
      std::scoped_lock lock(c.mutex);
      c.sheet = std::move(sheet);
      c.frameW = frameW;
      c.frameH = frameH;
    }

    c.state.store(2);
  });
}

static bool tryGetBackgroundAnimFromCache(juce::Image& outSheet, int& outFrameW, int& outFrameH) {
  auto& cache = getBackgroundAnimCache();
  if (cache.state.load() != 2)
    return false;

  std::scoped_lock lock(cache.mutex);
  if (!cache.sheet.isValid() || cache.frameW <= 0 || cache.frameH <= 0)
    return false;

  outSheet = cache.sheet;
  outFrameW = cache.frameW;
  outFrameH = cache.frameH;
  return true;
}

static bool setColumnOffsetToCenterImageIndex(
    int columnIndex,
    int imageIndex,
    const std::array<std::vector<juce::Image>, 3>& columnImages,
    std::array<float, 3>& columnOffsets,
    std::array<bool, 3>& snapping,
    juce::Rectangle<int> bounds) {
  static constexpr int kColumnCountLocal = 3;

  auto wrapPositiveLocal = [](float v, float m) -> float {
    if (m <= 0.0f)
      return 0.0f;
    v = std::fmod(v, m);
    if (v < 0.0f)
      v += m;
    return v;
  };

  if (columnIndex < 0 || columnIndex >= kColumnCountLocal)
    return false;

  // 这里避免调用splitLayoutAreas（其定义在后面），用相同规则自己算一次
  auto area = bounds.toFloat();
  area.reduce(kColumnsMargin, kColumnsMargin);
  area.removeFromBottom(kSliderBandH);
  area.removeFromBottom(kSlotToSliderGap);
  auto imagesArea = area;

  const float totalGap = kColumnGap * (kColumnCountLocal - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(kColumnCountLocal);

  const float x = imagesArea.getX() + columnIndex * (colW + kColumnGap);
  juce::Rectangle<float> col = { x, imagesArea.getY(), colW, imagesArea.getHeight() };

  const auto& imgs = columnImages[static_cast<size_t>(columnIndex)];
  if (imgs.empty())
    return false;

  imageIndex = juce::jlimit(0, static_cast<int>(imgs.size()) - 1, imageIndex);

  float cycleH = 0.0f;
  float targetCenterInCycle = 0.0f;
  bool targetSet = false;

  for (int i = 0; i < static_cast<int>(imgs.size()); ++i) {
    const auto& img = imgs[static_cast<size_t>(i)];
    if (!img.isValid())
      continue;

    const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
    const float drawW = col.getWidth();
    const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;

    if (i == imageIndex) {
      targetCenterInCycle = cycleH + drawH * 0.5f;
      targetSet = true;
    }

    cycleH += drawH;
  }

  if (!targetSet || cycleH <= 0.0f)
    return false;

  const float centerPos = col.getHeight() * 0.5f;
  const float offTarget = wrapPositiveLocal(centerPos - targetCenterInCycle, cycleH);

  columnOffsets[static_cast<size_t>(columnIndex)] = offTarget;
  snapping[static_cast<size_t>(columnIndex)] = false;
  return true;
}

static void splitLayoutAreas(juce::Rectangle<int> bounds,
                             juce::Rectangle<float>& outImagesArea,
                             juce::Rectangle<float>& outSliderBand) {
  auto area = bounds.toFloat();
  area.reduce(kColumnsMargin, kColumnsMargin);

  // 先从底部切出控制条区域，再切出一段间距，剩下的是老虎机图片区
  outSliderBand = area.removeFromBottom(kSliderBandH);
  area.removeFromBottom(kSlotToSliderGap);
  outImagesArea = area;
}

static float wrapPositive(float v, float m) {
  if (m <= 0.0f)
    return 0.0f;
  v = std::fmod(v, m);
  if (v < 0.0f)
    v += m;
  return v;
}

static float shortestDelta(float from, float to, float period) {
  if (period <= 0.0f)
    return 0.0f;

  float d = to - from;
  d = std::fmod(d, period);

  if (d > period * 0.5f)
    d -= period;
  else if (d < -period * 0.5f)
    d += period;

  return d;
}

juce::Image PluginEditor::decodePngFromBinary(const void* data, int size) {
  if (data == nullptr || size <= 0)
    return {};

  juce::MemoryInputStream stream(data, static_cast<size_t>(size), false);
  return juce::PNGImageFormat().decodeImage(stream);
}

void PluginEditor::loadImagesFromBinaryData() {
  // 背景：先同步加载小的background.png，确保打开界面立刻可显示。
  {
    auto fallback = decodePngFromBinary(BinaryData::background_png, BinaryData::background_pngSize);
    if (fallback.isValid())
      backgroundFallbackImage = std::move(fallback);
  }

  // 随机按钮图标
  {
    auto img = decodePngFromBinary(BinaryData::rand_png, BinaryData::rand_pngSize);
    if (img.isValid())
      randButtonImage = std::move(img);
  }

  // About按钮图标
  {
    auto img = decodePngFromBinary(BinaryData::about_png, BinaryData::about_pngSize);
    if (img.isValid())
      aboutButtonImage = std::move(img);
  }

  // 精灵图动画在后台异步加载：避免卡住UI
  ensureBackgroundAnimLoading();

  // 列1：1_1, 1_2, 1_3, 1_4
  columnImages[0].clear();
  columnImages[0].push_back(decodePngFromBinary(BinaryData::_1_1_png, BinaryData::_1_1_pngSize));
  columnImages[0].push_back(decodePngFromBinary(BinaryData::_1_2_png, BinaryData::_1_2_pngSize));
  columnImages[0].push_back(decodePngFromBinary(BinaryData::_1_3_png, BinaryData::_1_3_pngSize));
  columnImages[0].push_back(decodePngFromBinary(BinaryData::_1_4_png, BinaryData::_1_4_pngSize));

  // 列2：2_1 ...
  columnImages[1].clear();
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_1_png, BinaryData::_2_1_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_2_png, BinaryData::_2_2_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_3_png, BinaryData::_2_3_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_4_png, BinaryData::_2_4_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_5_png, BinaryData::_2_5_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_6_png, BinaryData::_2_6_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_7_png, BinaryData::_2_7_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_8_png, BinaryData::_2_8_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_9_png, BinaryData::_2_9_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_10_png, BinaryData::_2_10_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_11_png, BinaryData::_2_11_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_12_png, BinaryData::_2_12_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_13_png, BinaryData::_2_13_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_14_png, BinaryData::_2_14_pngSize));
  columnImages[1].push_back(decodePngFromBinary(BinaryData::_2_15_png, BinaryData::_2_15_pngSize));

  // 列3：3_1 ...
  columnImages[2].clear();
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_1_png, BinaryData::_3_1_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_2_png, BinaryData::_3_2_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_3_png, BinaryData::_3_3_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_4_png, BinaryData::_3_4_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_5_png, BinaryData::_3_5_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_6_png, BinaryData::_3_6_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_7_png, BinaryData::_3_7_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_8_png, BinaryData::_3_8_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_9_png, BinaryData::_3_9_pngSize));
  columnImages[2].push_back(decodePngFromBinary(BinaryData::_3_10_png, BinaryData::_3_10_pngSize));
}

void PluginEditor::initFloatyParams() {
  // 固定种子：保证每次打开插件看到的“随机漂浮感”稳定一致。
  juce::Random rng(0x5A17C0DE);

  for (int c = 0; c < columnCount; ++c) {
    const auto& imgs = columnImages[static_cast<size_t>(c)];

    floatyParams[static_cast<size_t>(c)].clear();
    floatyParams[static_cast<size_t>(c)].reserve(imgs.size());

    for (size_t i = 0; i < imgs.size(); ++i) {
      FloatyParams p;

      // 旋转：很小的角度（弧度）
      p.rotAmpRad = juce::degreesToRadians(rng.nextFloat() * 3.0f + 0.8f) * (rng.nextBool() ? 1.0f : -1.0f);
      p.rotSpeed = 0.35f + rng.nextFloat() * 0.8f;

      // 尺寸呼吸：轻微缩放
      p.scaleAmp = 0.02f + rng.nextFloat() * 0.035f;
      p.scaleSpeed = 0.35f + rng.nextFloat() * 0.9f;

      // 位置呼吸：轻微漂移
      p.posAmpX = 1.5f + rng.nextFloat() * 3.5f;
      p.posAmpY = 1.0f + rng.nextFloat() * 4.0f;
      p.posSpeedX = 0.25f + rng.nextFloat() * 0.8f;
      p.posSpeedY = 0.25f + rng.nextFloat() * 0.8f;

      // 每张图各自的相位
      p.phase1 = rng.nextFloat() * juce::MathConstants<float>::twoPi;
      p.phase2 = rng.nextFloat() * juce::MathConstants<float>::twoPi;
      p.phase3 = rng.nextFloat() * juce::MathConstants<float>::twoPi;
      p.phase4 = rng.nextFloat() * juce::MathConstants<float>::twoPi;

      floatyParams[static_cast<size_t>(c)].push_back(p);
    }
  }
}

juce::Rectangle<float> PluginEditor::getColumnsArea(juce::Rectangle<int> bounds) {
  auto area = bounds.toFloat();
  area.reduce(kColumnsMargin, kColumnsMargin);
  return area;
}

int PluginEditor::hitTestColumn(juce::Point<float> p) const {
  juce::Rectangle<float> imagesArea, sliderBand;
  splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

  const float totalGap = kColumnGap * (columnCount - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

  if (p.x < imagesArea.getX() || p.x >= imagesArea.getRight() || p.y < imagesArea.getY() || p.y >= imagesArea.getBottom())
    return -1;

  const float localX = p.x - imagesArea.getX();
  for (int c = 0; c < columnCount; ++c) {
    const float x0 = c * (colW + kColumnGap);
    const float x1 = x0 + colW;
    if (localX >= x0 && localX < x1)
      return c;
  }
  return -1;
}

juce::Rectangle<float> PluginEditor::getSliderRect(int columnIndex) const {
  juce::Rectangle<float> imagesArea, sliderBand;
  splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

  const float totalGap = kColumnGap * (columnCount - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

  const float x = imagesArea.getX() + columnIndex * (colW + kColumnGap);
  const float y = sliderBand.getY() + (sliderBand.getHeight() - kSliderH) * 0.5f;

  return { x, y, colW, kSliderH };
}

int PluginEditor::hitTestSlider(juce::Point<float> p) const {
  for (int c = 0; c < columnCount; ++c) {
    if (getSliderRect(c).contains(p))
      return c;
  }
  return -1;
}

juce::Rectangle<float> PluginEditor::getRandButtonVisualRect() const {
  // 右上角随机预设按钮：视觉区域（绘制用）。
  // 触发区域也应与图片尺寸一致（不要扩展热区）。

  constexpr float pad = 10.0f;  // 距离窗口边缘的内边距

  auto b = getLocalBounds().toFloat();

  float w = 44.0f;
  float h = 44.0f;
  if (randButtonImage.isValid()) {
    w = static_cast<float>(randButtonImage.getWidth());
    h = static_cast<float>(randButtonImage.getHeight());
  }

  juce::Rectangle<float> r(
      b.getRight() - pad - w,
      b.getY() + pad,
      w,
      h);

  return r.getIntersection(b);
}

juce::Rectangle<float> PluginEditor::getRandButtonRect() const {
  // 触发区域：严格等于图片（rand.png）的视觉区域。
  return getRandButtonVisualRect();
}

bool PluginEditor::hitTestRandButton(juce::Point<float> p) const {
  return getRandButtonRect().contains(p);
}

juce::Rectangle<float> PluginEditor::getAboutButtonVisualRect() const {
  // About按钮：放在随机按钮正下方；触发区域与图片尺寸一致。

  constexpr float gapY = 8.0f;  // 与随机按钮的垂直间距

  auto b = getLocalBounds().toFloat();

  float w = 44.0f;
  float h = 44.0f;
  if (aboutButtonImage.isValid()) {
    w = static_cast<float>(aboutButtonImage.getWidth());
    h = static_cast<float>(aboutButtonImage.getHeight());
  } else if (randButtonImage.isValid()) {
    // 没加载到about.png时，至少保持与rand按钮一致的尺寸风格
    w = static_cast<float>(randButtonImage.getWidth());
    h = static_cast<float>(randButtonImage.getHeight());
  }

  const auto randR = getRandButtonVisualRect();

  juce::Rectangle<float> r(
      randR.getX() + (randR.getWidth() - w) * 0.5f,
      randR.getBottom() + gapY,
      w,
      h);

  return r.getIntersection(b);
}

juce::Rectangle<float> PluginEditor::getAboutButtonRect() const {
  // 触发区域：严格等于图片（about.png）的视觉区域。
  return getAboutButtonVisualRect();
}

bool PluginEditor::hitTestAboutButton(juce::Point<float> p) const {
  return getAboutButtonRect().contains(p);
}

void PluginEditor::showAboutDialog() {
  // 需求：纯黑底色、所有文本左对齐、文本可选择复制、链接可点击。

  struct AboutDialogComponent final : public juce::Component {
    AboutDialogComponent() {
      setOpaque(true);

      auto setupSectionTitle = [](juce::Label& l, const juce::String& s) {
        l.setText(s, juce::dontSendNotification);
        l.setJustificationType(juce::Justification::centred);
        l.setFont(juce::Font(16.0f, juce::Font::bold));
        l.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.95f));
      };

      auto setupReadOnlyText = [](juce::TextEditor& te) {
        te.setMultiLine(true, true);
        te.setReturnKeyStartsNewLine(true);
        te.setReadOnly(true);
        te.setCaretVisible(false);
        te.setScrollbarsShown(true);
        te.setPopupMenuEnabled(true);
        te.setJustification(juce::Justification::topLeft);

        te.setColour(juce::TextEditor::backgroundColourId, juce::Colours::black);
        te.setColour(juce::TextEditor::textColourId, juce::Colours::white.withAlpha(0.92f));
        te.setColour(juce::TextEditor::highlightColourId, juce::Colours::white.withAlpha(0.22f));
        te.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
        te.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        te.setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
        te.setFont(juce::Font(14.0f));
      };

      setupReadOnlyText(headerText);
      setupReadOnlyText(descriptionText);
      setupReadOnlyText(feedbackText);
      setupReadOnlyText(licenseText);

      headerText.setText("Plugin Name:rorrerror\nVersion: 1.1.0", false);

      descriptionText.setText(
          "This plugin is completely free and open-source software (FOSS) under a custom \"non-commercial\" open-source license. You are free to use, study, and modify the code, but commercial use is strictly prohibited (including but not limited to reselling, paid distribution, or integration into commercial products).\n\n"
          "Basic usage statistics: product/version, random per-product installation ID, OS/architecture and host/format. No audio or project data.",
          false);

      feedbackText.setText(
          "If you have great ideas, new feature requests, or find any bugs, please feel free to reach out! Your feedback helps make this plugin better.\n\n"
          "Douyin: @iisaac_m1\n"
          "Direct link: https://www.douyin.com/user/MS4wLjABAAAAiFGHXlMXjD35spKBsmhel6vBNf2GJoBdfqskdyzZJ7E\n\n"
          "Bilibili(vid): 2314428\n"
          "Direct link: https://space.bilibili.com/2314428\n\n"
          "Email: 1454949244@qq.com",
          false);

      licenseText.setText(
          "Custom Non-Commercial License (based on MIT, prohibits commercial closed-source distribution).",
          false);

      addAndMakeVisible(headerText);

      // 官网（显眼位置，点击跳转）
      websiteLink.setButtonText("web:iisaacbeats.cn");
      websiteLink.setURL(juce::URL("https://iisaacbeats.cn"));
      websiteLink.setJustificationType(juce::Justification::centred);
      websiteLink.setFont(juce::Font(18.0f, juce::Font::bold), false, juce::Justification::centred);
      websiteLink.setColour(juce::HyperlinkButton::textColourId, juce::Colours::red.withAlpha(0.98f));
      addAndMakeVisible(websiteLink);

      setupSectionTitle(descriptionTitle, "Description");
      addAndMakeVisible(descriptionTitle);
      addAndMakeVisible(descriptionText);

      setupSectionTitle(feedbackTitle, "Feedback & Contact");
      addAndMakeVisible(feedbackTitle);
      addAndMakeVisible(feedbackText);

      setupSectionTitle(licenseTitle, "License");
      addAndMakeVisible(licenseTitle);
      addAndMakeVisible(licenseText);

      // 可点击链接
      douyinLink.setButtonText("Douyin link: https://www.douyin.com/user/MS4wLjABAAAAiFGHXlMXjD35spKBsmhel6vBNf2GJoBdfqskdyzZJ7E");
      douyinLink.setURL(juce::URL("https://www.douyin.com/user/MS4wLjABAAAAiFGHXlMXjD35spKBsmhel6vBNf2GJoBdfqskdyzZJ7E"));
      douyinLink.setColour(juce::HyperlinkButton::textColourId, juce::Colours::white.withAlpha(0.92f));
      addAndMakeVisible(douyinLink);

      bilibiliLink.setButtonText("Bilibili link: https://space.bilibili.com/2314428");
      bilibiliLink.setURL(juce::URL("https://space.bilibili.com/2314428"));
      bilibiliLink.setColour(juce::HyperlinkButton::textColourId, juce::Colours::white.withAlpha(0.92f));
      addAndMakeVisible(bilibiliLink);

      closeButton.setButtonText("OK");
      closeButton.setColour(juce::TextButton::buttonColourId, juce::Colours::black);
      closeButton.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xFF111111));
      closeButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white.withAlpha(0.92f));
      closeButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white.withAlpha(0.92f));
      closeButton.onClick = [this] {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>())
          w->exitModalState(0);
      };
      addAndMakeVisible(closeButton);
    }

    void paint(juce::Graphics& g) override {
      g.fillAll(juce::Colours::black);
    }

    void resized() override {
      auto r = getLocalBounds().reduced(14);

      auto bottom = r.removeFromBottom(40);
      closeButton.setBounds(bottom.withSizeKeepingCentre(120, 28));

      r.removeFromBottom(10);

      // 顶部：基础信息
      headerText.setBounds(r.removeFromTop(62));
      r.removeFromTop(8);

      // 官网（显眼位置）
      websiteLink.setBounds(r.removeFromTop(32));
      r.removeFromTop(12);

      // Description
      descriptionTitle.setBounds(r.removeFromTop(24));
      r.removeFromTop(6);
      descriptionText.setBounds(r.removeFromTop(128));
      r.removeFromTop(12);

      // Feedback & Contact
      feedbackTitle.setBounds(r.removeFromTop(24));
      r.removeFromTop(6);
      feedbackText.setBounds(r.removeFromTop(160));
      r.removeFromTop(8);

      // Links
      auto links = r.removeFromTop(52);
      douyinLink.setBounds(links.removeFromTop(24));
      bilibiliLink.setBounds(links.removeFromTop(24));
      r.removeFromTop(12);

      // License
      licenseTitle.setBounds(r.removeFromTop(24));
      r.removeFromTop(6);
      licenseText.setBounds(r);
    }

    juce::TextEditor headerText;

    juce::HyperlinkButton websiteLink;

    juce::Label descriptionTitle;
    juce::TextEditor descriptionText;
    juce::Label feedbackTitle;
    juce::TextEditor feedbackText;
    juce::Label licenseTitle;
    juce::TextEditor licenseText;

    juce::HyperlinkButton douyinLink;
    juce::HyperlinkButton bilibiliLink;
    juce::TextButton closeButton;
  };

  juce::DialogWindow::LaunchOptions opt;
  opt.dialogTitle = "About";
  opt.componentToCentreAround = this;
  opt.escapeKeyTriggersCloseButton = true;
  opt.useNativeTitleBar = true;
  opt.resizable = true;
  opt.useBottomRightCornerResizer = true;
  opt.dialogBackgroundColour = juce::Colours::black;

  auto* content = new AboutDialogComponent();
  // 默认高度加大：尽量一次性展示完整内容，避免用户一打开就需要拖动。
  content->setSize(720, 760);
  opt.content.setOwned(content);

  opt.launchAsync();
}

void PluginEditor::startSnapToImageIndex(int columnIndex, int imageIndex) {
  if (columnIndex < 0 || columnIndex >= columnCount)
    return;

  juce::Rectangle<float> imagesArea, sliderBand;
  splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

  const float totalGap = kColumnGap * (columnCount - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

  const float x = imagesArea.getX() + columnIndex * (colW + kColumnGap);
  juce::Rectangle<float> col = { x, imagesArea.getY(), colW, imagesArea.getHeight() };

  const auto& imgs = columnImages[static_cast<size_t>(columnIndex)];
  if (imgs.empty())
    return;

  imageIndex = juce::jlimit(0, static_cast<int>(imgs.size()) - 1, imageIndex);

  float cycleH = 0.0f;
  float targetCenterInCycle = 0.0f;
  bool targetSet = false;

  for (int i = 0; i < static_cast<int>(imgs.size()); ++i) {
    const auto& img = imgs[static_cast<size_t>(i)];
    if (!img.isValid())
      continue;

    const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
    const float drawW = col.getWidth();
    const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;

    if (!targetSet && i == imageIndex) {
      targetCenterInCycle = cycleH + drawH * 0.5f;
      targetSet = true;
    }

    cycleH += drawH;
  }

  if (!targetSet || cycleH <= 0.0f)
    return;

  const float currentOffset = columnOffsets[static_cast<size_t>(columnIndex)];
  const float offCur = wrapPositive(currentOffset, cycleH);

  const float centerPos = col.getHeight() * 0.5f;
  const float offTarget = wrapPositive(centerPos - targetCenterInCycle, cycleH);
  const float d = shortestDelta(offCur, offTarget, cycleH);

  snapping[static_cast<size_t>(columnIndex)] = true;
  snapTargets[static_cast<size_t>(columnIndex)] = currentOffset + d;
}

void PluginEditor::startRandomSlotSpin() {
  // 重新开始一轮“老虎机随机”
  slotSpinActive = true;

  // 同步触发“快速倒带”：用scratch倒放实现（UI线程控制，音频线程读取）
  slotSpinScratchRewindActive = true;
  processor.setScratchEnabled(true);
  processor.setScratchRate(-2.8f);

  for (int c = 0; c < columnCount; ++c) {
    slotSpinDone[static_cast<size_t>(c)] = false;
    snapping[static_cast<size_t>(c)] = false;
  }

  const double nowMs = juce::Time::getMillisecondCounterHiRes();
  juce::Random rng(static_cast<int>(nowMs));

  for (int c = 0; c < columnCount; ++c) {
    const auto& imgs = columnImages[static_cast<size_t>(c)];
    const int n = static_cast<int>(imgs.size());

    if (n <= 0) {
      slotSpinDone[static_cast<size_t>(c)] = true;
      continue;
    }

    slotSpinTargetIndex[static_cast<size_t>(c)] = rng.nextInt(n);

    // 记录起点
    slotSpinStartOffset[static_cast<size_t>(c)] = columnOffsets[static_cast<size_t>(c)];
    slotSpinStartTimeMs[static_cast<size_t>(c)] = nowMs;

    // 持续时间错开：第一列先停，第三列后停
    const double dur = 1400.0 + static_cast<double>(c) * 520.0 + rng.nextDouble() * 620.0;
    slotSpinDurationMs[static_cast<size_t>(c)] = dur;

    // 计算目标：让指定 imageIndex 最终居中，同时多转若干圈保证“像老虎机一样滚很久”
    juce::Rectangle<float> imagesArea, sliderBand;
    splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

    const float totalGap = kColumnGap * (columnCount - 1);
    const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

    const float x = imagesArea.getX() + c * (colW + kColumnGap);
    juce::Rectangle<float> col = { x, imagesArea.getY(), colW, imagesArea.getHeight() };

    float cycleH = 0.0f;
    float targetCenterInCycle = 0.0f;
    bool targetSet = false;

    for (int i = 0; i < static_cast<int>(imgs.size()); ++i) {
      const auto& img = imgs[static_cast<size_t>(i)];
      if (!img.isValid())
        continue;

      const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
      const float drawW = col.getWidth();
      const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;

      if (!targetSet && i == slotSpinTargetIndex[static_cast<size_t>(c)]) {
        targetCenterInCycle = cycleH + drawH * 0.5f;
        targetSet = true;
      }

      cycleH += drawH;
    }

    if (!targetSet || cycleH <= 0.0f) {
      slotSpinTargetOffset[static_cast<size_t>(c)] = slotSpinStartOffset[static_cast<size_t>(c)];
      continue;
    }

    const float centerPos = col.getHeight() * 0.5f;
    const float offTargetInCycle = wrapPositive(centerPos - targetCenterInCycle, cycleH);

    const float offCurInCycle = wrapPositive(slotSpinStartOffset[static_cast<size_t>(c)], cycleH);
    const float dShortest = shortestDelta(offCurInCycle, offTargetInCycle, cycleH);

    // 额外转圈：保证最终位移为“向下滚动”的正方向，并且足够长
    const int extraTurns = 3 + rng.nextInt(3); // 3..5圈
    const float baseDelta = dShortest + static_cast<float>(extraTurns) * cycleH;

    // 如果shortestDelta恰好是负的，可能会抵消一部分，确保总增量为正
    const float delta = (baseDelta <= 0.0f) ? (baseDelta + cycleH) : baseDelta;

    slotSpinTargetOffset[static_cast<size_t>(c)] = slotSpinStartOffset[static_cast<size_t>(c)] + delta;
  }
}

void PluginEditor::updateSlotSpin() {
  if (!slotSpinActive)
    return;

  const double nowMs = juce::Time::getMillisecondCounterHiRes();
  bool anyRunning = false;

  auto easeOutCubic = [](float x) -> float {
    x = juce::jlimit(0.0f, 1.0f, x);
    const float a = 1.0f - x;
    return 1.0f - a * a * a;
  };

  float slowestU = 1.0f;

  for (int c = 0; c < columnCount; ++c) {
    if (slotSpinDone[static_cast<size_t>(c)])
      continue;

    anyRunning = true;

    const double t = nowMs - slotSpinStartTimeMs[static_cast<size_t>(c)];
    const double dur = juce::jmax(1.0, slotSpinDurationMs[static_cast<size_t>(c)]);
    const float u = static_cast<float>(t / dur);

    slowestU = juce::jmin(slowestU, juce::jlimit(0.0f, 1.0f, u));

    if (u >= 1.0f) {
      // 结束：精确落在目标offset
      columnOffsets[static_cast<size_t>(c)] = slotSpinTargetOffset[static_cast<size_t>(c)];
      slotSpinDone[static_cast<size_t>(c)] = true;
      continue;
    }

    const float e = easeOutCubic(u);
    const float start = slotSpinStartOffset[static_cast<size_t>(c)];
    const float target = slotSpinTargetOffset[static_cast<size_t>(c)];

    columnOffsets[static_cast<size_t>(c)] = start + (target - start) * e;
  }

  // 倒带速度跟随“最慢那一列”的进度：开头更快、尾部逐渐回到0
  if (slotSpinScratchRewindActive) {
    const float t01 = 1.0f - slowestU; // 1->0
    const float shaped = t01 * t01;    // 更偏向开头快
    const float rate = -0.35f + (-3.2f + 0.35f) * shaped; // 约 -3.2 .. -0.35
    processor.setScratchEnabled(true);
    processor.setScratchRate(rate);
  }

  if (!anyRunning) {
    slotSpinActive = false;

    // 停止倒带（回到正常播放）
    if (slotSpinScratchRewindActive) {
      slotSpinScratchRewindActive = false;
      processor.setScratchRate(0.0f);
      processor.setScratchEnabled(false);
    }
  }
}

void PluginEditor::setSliderNormValue(int columnIndex, float norm) {
  if (columnIndex < 0 || columnIndex >= columnCount)
    return;

  norm = juce::jlimit(0.0f, 1.0f, norm);

  // 第三列：离散7档（吸附到7个点）
  if (columnIndex == 2) {
    static constexpr int kSteps = 7;
    const int idx = juce::jlimit(0, kSteps - 1, static_cast<int>(std::lround(norm * static_cast<float>(kSteps - 1))));
    norm = static_cast<float>(idx) / static_cast<float>(kSteps - 1);
  }
  sliderNorm[static_cast<size_t>(columnIndex)] = norm;

  // 把UI滑条值持久化进Processor（用于Editor重开/宿主保存）
  if (columnIndex == 0)
    processor.setSlider0Norm(norm);
  else if (columnIndex == 1)
    processor.setSlider1Norm(norm);
  else if (columnIndex == 2)
    processor.setSlider2Norm(norm);

  // 第一列：控制前置增益 0..24dB（默认+12dB => 0.5）
  if (columnIndex == 0) {
    const float db = 0.0f + norm * 24.0f;
    processor.setInputGainDb(db);
  }

  // 第二列：控制waveshaper湿度（0..1，默认0.5）
  if (columnIndex == 1) {
    processor.setColumn2Wet(norm);
  }
}

PluginEditor::PluginEditor(PluginProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p) {
  loadImagesFromBinaryData();
  initFloatyParams();

  setResizable(true, true);

  // 打开时优先用background.png决定窗口尺寸（快速），动画图集就绪后只切换绘制，不改窗口尺寸。
  // 默认分辨率减半（长宽各缩小一半）。
  if (backgroundFallbackImage.isValid())
    setSize(backgroundFallbackImage.getWidth() / 2, backgroundFallbackImage.getHeight() / 2);
  else
    setSize(270, 270);

  // 恢复上一次状态（宿主会通过Processor::setStateInformation恢复到Processor，再由Editor从Processor读取）
  setSliderNormValue(0, processor.getSlider0Norm());
  setSliderNormValue(1, processor.getSlider1Norm());
  setSliderNormValue(2, processor.getSlider2Norm());

  lastBoundsForSelection = getLocalBounds();
  recenterColumnsToProcessorSelection(lastBoundsForSelection);

  // 注意：recenterColumnsToProcessorSelection 内部会在失败时回退到 setInitialCenteredDefaults()
  // 这里无需再次判断 ok1/ok2/ok3。

  // 立即把UI状态推送到Processor，确保音频线程参数正确
  pushUiStateToProcessor();

  animStartMs = juce::Time::getMillisecondCounterHiRes();
  startTimerHz(60);

  telemetrySession = std::make_unique<iisaac::telemetry::Session>(
      iisaac::telemetry::forPlugin("rorrerror", JucePlugin_VersionString,
                                 JucePlugin_VersionString, processor.wrapperType));
}

PluginEditor::~PluginEditor() {
  telemetrySession.reset();
  // 确保Timer不再触发回调（避免宿主在销毁阶段出现重入/消息线程卡住）
  stopTimer();

  // 等待glitch解码线程结束（解码很快，避免线程访问已销毁的成员）
  if (glitchDecodeThread.joinable())
    glitchDecodeThread.join();

  // 兜底：如果关闭界面时仍在拖动，确保搓碟关闭
  processor.setScratchEnabled(false);
  processor.setScratchRate(0.0f);

  // 释放Editor对大图的引用（缓存仍持有，从而避免在此处释放巨量内存造成宿主假死）
  backgroundSpriteSheet = {};
  backgroundFallbackImage = {};

  for (auto& v : columnImages)
    v.clear();
  for (auto& v : floatyParams)
    v.clear();

  // 如果后台加载线程已结束，则在这里及时join掉，避免DLL卸载阶段再处理线程对象。
  tryJoinBackgroundAnimWorkerIfFinished();
}

juce::String PluginEditor::makeGlitchResourceName(int frameIndex, int version) {
  // frame_XXX_vN.png 经过JUCE二进制资源名称变换后为 frame_XXX_vN_png
  return juce::String::formatted("frame_%03d_v%d_png", frameIndex, version + 1);
}

juce::Image PluginEditor::decodeGlitchFrame(int frameIndex, int version) {
  const juce::String name = makeGlitchResourceName(frameIndex, version);
  int dataSize = 0;
  const char* data = BinaryData::getNamedResource(name.toRawUTF8(), dataSize);
  if (data == nullptr || dataSize <= 0)
    return {};
  return decodePngFromBinaryLocal(data, dataSize);
}

void PluginEditor::launchGlitchDecode() {
  // 解码很快，直接join上一个线程，避免线程堆积
  if (glitchDecodeThread.joinable())
    glitchDecodeThread.join();

  for (auto& ready : glitchOverrideReady)
    ready.store(false);

  const auto frames = glitchOverrideFrames;
  const auto versions = glitchOverrideVersions;

  glitchDecodeThread = std::thread([this, frames, versions] {
    std::array<juce::Image, glitchOverrideCount> imgs;
    std::array<bool, glitchOverrideCount> ok{ { false, false, false } };

    for (int i = 0; i < glitchOverrideCount; ++i) {
      if (frames[static_cast<size_t>(i)] < 0)
        continue;
      imgs[static_cast<size_t>(i)] =
          decodeGlitchFrame(frames[static_cast<size_t>(i)], versions[static_cast<size_t>(i)]);
      ok[static_cast<size_t>(i)] = imgs[static_cast<size_t>(i)].isValid();
    }

    {
      std::scoped_lock lock(glitchDecodeMutex);
      glitchOverrideImages = std::move(imgs);
    }

    for (int i = 0; i < glitchOverrideCount; ++i)
      glitchOverrideReady[static_cast<size_t>(i)].store(ok[static_cast<size_t>(i)]);
  });
}

void PluginEditor::updateBackgroundGlitchOverrides() {
  // 背景动画图集尚未就绪时，无需处理glitch替换
  if (!backgroundSpriteSheet.isValid())
    return;

  const int totalFrames = backgroundSheetCols * backgroundSheetRows;

  const int cycle = (totalFrames > 0 && backgroundFps > 0.0f)
      ? (static_cast<int>(std::floor(animTimeSeconds * backgroundFps)) / totalFrames)
      : 0;

  if (cycle == lastBackgroundCycle)
    return;
  lastBackgroundCycle = cycle;

  // 每轮随机选3个不同帧，并为每帧随机选一个glitch版本
  const juce::int64 seed = static_cast<juce::int64>(juce::Time::getMillisecondCounterHiRes() * 1000.0)
      + static_cast<juce::int64>(cycle);
  juce::Random rng(seed);

  std::array<int, glitchOverrideCount> frames{ { -1, -1, -1 } };
  std::array<int, glitchOverrideCount> versions{ { 0, 0, 0 } };

  for (int i = 0; i < glitchOverrideCount; ++i) {
    int candidate = -1;
    for (int attempt = 0; attempt < totalFrames * 4; ++attempt) {
      const int c = rng.nextInt(totalFrames);
      bool used = false;
      for (int j = 0; j < i; ++j) {
        if (frames[static_cast<size_t>(j)] == c) {
          used = true;
          break;
        }
      }
      if (!used) {
        candidate = c;
        break;
      }
    }

    if (candidate < 0) {
      for (int c = 0; c < totalFrames; ++c) {
        bool used = false;
        for (int j = 0; j < i; ++j) {
          if (frames[static_cast<size_t>(j)] == c) {
            used = true;
            break;
          }
        }
        if (!used) {
          candidate = c;
          break;
        }
      }
    }

    frames[static_cast<size_t>(i)] = candidate;
    versions[static_cast<size_t>(i)] = rng.nextInt(glitchVersionCount);
  }

  glitchOverrideFrames = frames;
  glitchOverrideVersions = versions;

  launchGlitchDecode();
}

void PluginEditor::recenterColumnsToProcessorSelection(juce::Rectangle<int> bounds) {
  const bool ok1 = setColumnOffsetToCenterImageIndex(0, processor.getColumn1SelectedIndex(), columnImages, columnOffsets, snapping, bounds);
  const bool ok2 = setColumnOffsetToCenterImageIndex(1, processor.getColumn2SelectedIndex(), columnImages, columnOffsets, snapping, bounds);
  const bool ok3 = setColumnOffsetToCenterImageIndex(2, processor.getColumn3SelectedIndex(), columnImages, columnOffsets, snapping, bounds);

  if (!ok1 || !ok2 || !ok3) {
    // 兜底：如果图片还没就绪/索引异常，则用默认初始状态。
    setInitialCenteredDefaults();
  }
}

void PluginEditor::setInitialCenteredDefaults() {
  juce::Rectangle<float> imagesArea, sliderBand;
  splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

  const float totalGap = kColumnGap * (columnCount - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

  for (int c = 0; c < columnCount; ++c) {
    const float x = imagesArea.getX() + c * (colW + kColumnGap);
    juce::Rectangle<float> col = { x, imagesArea.getY(), colW, imagesArea.getHeight() };

    const auto& imgs = columnImages[static_cast<size_t>(c)];
    if (imgs.empty())
      continue;

    // 计算该列一轮高度 + 目标图中心位置（在cycle内）
    float cycleH = 0.0f;
    float targetCenterInCycle = 0.0f;
    bool targetSet = false;
    const int targetIndex = (c == 2 ? 4 : 0);  // 第三列默认3_5，其它列默认第一张

    for (size_t i = 0; i < imgs.size(); ++i) {
      const auto& img = imgs[i];
      if (!img.isValid())
        continue;

      const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
      const float drawW = col.getWidth();
      const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;

      if (!targetSet && static_cast<int>(i) == targetIndex) {
        targetCenterInCycle = cycleH + drawH * 0.5f;
        targetSet = true;
      }

      cycleH += drawH;
    }

    if (cycleH <= 0.0f)
      continue;

    // 如果目标索引不存在（比如图片没加载齐），退回到第一张
    if (!targetSet)
      targetCenterInCycle = col.getHeight() * 0.0f + (cycleH > 0.0f ? 0.0f : 0.0f);

    const float centerPos = col.getHeight() * 0.5f;
    const float offTarget = wrapPositive(centerPos - targetCenterInCycle, cycleH);

    columnOffsets[static_cast<size_t>(c)] = offTarget;
    snapping[static_cast<size_t>(c)] = false;
  }

  // slider默认值：第一列 +12dB；第二列 waveshaper wet=50%
  setSliderNormValue(0, 0.5f);
  setSliderNormValue(1, 0.5f);
  // 第三列默认：每8拍触发一次（7点离散：idx=3 => 3/6 = 0.5）
  setSliderNormValue(2, 0.5f);
}

void PluginEditor::paint(juce::Graphics& g) {
  // 动态变换会用到旋转/缩放，这里选择更高质量的采样
  g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);

  // 背景：20fps 精灵图动画
  if (backgroundSpriteSheet.isValid() && backgroundFrameW > 0 && backgroundFrameH > 0) {
    const int totalFrames = backgroundSheetCols * backgroundSheetRows;
    const int frameIndex = (totalFrames > 0)
        ? (static_cast<int>(std::floor(animTimeSeconds * backgroundFps)) % totalFrames)
        : 0;

    const auto dst = getLocalBounds();

    // 检查当前帧是否命中本轮随机选中的 glitch 替换帧
    int glitchSlot = -1;
    for (int i = 0; i < glitchOverrideCount; ++i) {
      if (glitchOverrideFrames[static_cast<size_t>(i)] == frameIndex) {
        glitchSlot = i;
        break;
      }
    }

    bool drewGlitch = false;
    if (glitchSlot >= 0 && glitchOverrideReady[static_cast<size_t>(glitchSlot)].load()) {
      juce::Image glitchImg;
      {
        std::scoped_lock lock(glitchDecodeMutex);
        glitchImg = glitchOverrideImages[static_cast<size_t>(glitchSlot)];
      }
      if (glitchImg.isValid()) {
        g.drawImage(glitchImg, dst.toFloat());
        drewGlitch = true;
      }
    }

    if (!drewGlitch) {
      const int col = (backgroundSheetCols > 0) ? (frameIndex % backgroundSheetCols) : 0;
      const int row = (backgroundSheetCols > 0) ? (frameIndex / backgroundSheetCols) : 0;

      const int sx = col * backgroundFrameW;
      const int sy = row * backgroundFrameH;

      g.drawImage(backgroundSpriteSheet,
                  dst.getX(), dst.getY(), dst.getWidth(), dst.getHeight(),
                  sx, sy,
                  backgroundFrameW, backgroundFrameH);
    }
  } else if (backgroundFallbackImage.isValid()) {
    g.drawImage(backgroundFallbackImage, getLocalBounds().toFloat());
  } else {
    auto bounds = getLocalBounds().toFloat();
    juce::Colour top = juce::Colour(0xFF1E1E1E);
    juce::Colour bottom = juce::Colour(0xFF121212);
    g.setGradientFill(
        juce::ColourGradient(top, 0.0f, 0.0f, bottom, 0.0f, bounds.getBottom(), false));
    g.fillAll();
  }

  // 第三列Glitch可视化（全屏）：底图之上、所有控制器之下
  // 说明：这里绘制在最前面（仅次于背景），这样不会压在老虎机/控制条之上。
  {
    auto hash01 = [](int n) -> float {
      const float x = std::sin(static_cast<float>(n) * 12.9898f) * 43758.5453f;
      return x - std::floor(x);
    };

    const float flash = juce::jlimit(0.0f, 1.0f, juce::jmax(col3Flash, col3UiAmount01));
    if (flash > 0.015f) {
      const float t2 = static_cast<float>(animTimeSeconds);
      auto overlay = getLocalBounds().toFloat();

      g.saveState();

      const int modeIdx = processor.getColumn3SelectedIndex();  // 0..9 => 3_1..3_10
      const int base = 50021 + modeIdx * 901 + static_cast<int>(overlay.getWidth()) + static_cast<int>(overlay.getHeight());

      // 全屏“画面抖动/错位”
      const float jx = (hash01(base + static_cast<int>(t2 * 70.0f)) - 0.5f) * 2.0f;
      const float jy = (hash01(base + 77 + static_cast<int>(t2 * 90.0f)) - 0.5f) * 2.0f;
      const float dx = jx * (3.0f + 14.0f * flash);
      const float dy = jy * (2.0f + 9.0f * flash);

      // scope：覆盖整个界面（稍微留边避免贴边太硬），并随dx/dy轻微漂移
      auto scope = overlay.reduced(10.0f, 10.0f).translated(dx * 0.25f, dy * 0.25f);

      // 全屏淡淡底噪：用多层径向渐变 + 少量噪点，避免出现明显矩形边界
      {
        const float a0 = 0.010f + 0.060f * flash;
        const float a1 = 0.006f + 0.035f * flash;

        juce::ColourGradient g0(
            juce::Colours::white.withAlpha(0.00f), scope.getCentreX(), scope.getCentreY(),
            juce::Colours::white.withAlpha(a0), scope.getX(), scope.getY(), true);
        g.setGradientFill(g0);
        g.fillRect(scope);

        juce::ColourGradient g1(
            juce::Colours::white.withAlpha(0.00f), scope.getCentreX(), scope.getCentreY(),
            juce::Colours::white.withAlpha(a1), scope.getRight(), scope.getBottom(), true);
        g.setGradientFill(g1);
        g.fillRect(scope);

        // 噪点（很稀疏）：让“边界”更难被看出
        const int dots = 60 + static_cast<int>(flash * 140.0f);
        g.setColour(juce::Colours::white.withAlpha(0.010f + 0.030f * flash));
        for (int i = 0; i < dots; ++i) {
          const float u = hash01(base + 8100 + i * 37 + static_cast<int>(t2 * 50.0f));
          const float v = hash01(base + 8200 + i * 41 + static_cast<int>(t2 * 60.0f));
          const float x = scope.getX() + u * scope.getWidth();
          const float y = scope.getY() + v * scope.getHeight();
          g.fillRect(juce::Rectangle<float>(x, y, 1.0f, 1.0f));
        }
      }

      // 扫描线（全屏）
      {
        const float scanY = scope.getY() + std::fmod(t2 * (140.0f + 120.0f * flash), juce::jmax(1.0f, scope.getHeight()));
        g.setColour(juce::Colours::white.withAlpha(0.025f + 0.11f * flash));
        g.fillRect(juce::Rectangle<float>(scope.getX(), scanY, scope.getWidth(), 1.0f));
      }

      auto xAt = [&](float u01) -> float { return scope.getX() + u01 * scope.getWidth(); };
      auto yAt = [&](float v01) -> float { return scope.getBottom() - v01 * scope.getHeight(); };

      const float lineA = 0.028f + 0.20f * flash;
      const float glowA = lineA * 0.45f;

      // 生成一个“基波形”（非音频数据，只是视觉暗示）
      auto baseWave = [&](float u01) -> float {
        const float ph = t2 * (4.0f + 10.0f * flash);
        float w = 0.55f * std::sin(ph + u01 * 6.28318f * (1.5f + 2.0f * flash));
        w += 0.25f * std::sin(ph * 1.37f + u01 * 6.28318f * (3.5f + 1.0f * flash));
        // 轻微噪声
        const float n = (hash01(base + static_cast<int>(u01 * 997.0f) + static_cast<int>(t2 * 50.0f) * 13) - 0.5f) * 2.0f;
        w += n * (0.06f + 0.10f * flash);
        return juce::jlimit(-1.0f, 1.0f, w);
      };

      auto strokeGlow = [&](const juce::Path& path, float w) {
        // RGB色移：红/蓝分离（随flash轻微波动）
        {
          // 注意：这里的偏移是“像素”，需要足够大才看得出色分离
          const float baseSep = 2.60f + 5.00f * flash; // px（显著加大，确保肉眼可见）
          const float wob = std::sin(t2 * (5.5f + 9.0f * flash) + hash01(base + 9101) * 6.28318f);
          const float rnd = (hash01(base + 9202 + static_cast<int>(t2 * 60.0f)) - 0.5f) * 1.40f;
          const float sep = juce::jmax(0.0f, baseSep + rnd);
          const float dxC = sep * (0.85f + 0.35f * wob);
          const float dyC = sep * (0.12f * wob);

          juce::Path pr = path;
          pr.applyTransform(juce::AffineTransform::translation(dxC, dyC));
          g.setColour(juce::Colours::red.withAlpha(lineA * 0.40f));
          g.strokePath(pr, juce::PathStrokeType(w * 0.78f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

          juce::Path pb = path;
          pb.applyTransform(juce::AffineTransform::translation(-dxC, -dyC));
          g.setColour(juce::Colours::blue.withAlpha(lineA * 0.40f));
          g.strokePath(pb, juce::PathStrokeType(w * 0.78f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // 白色辉光/主线整体削弱
        g.setColour(juce::Colours::white.withAlpha(glowA * 0.65f));
        g.strokePath(path, juce::PathStrokeType(w * 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(juce::Colours::white.withAlpha(lineA * 0.78f));
        g.strokePath(path, juce::PathStrokeType(w * 0.92f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
      };

      // 根据模式绘制不同的“故障波形”（全屏）
      switch (modeIdx) {
        default:
        case 0: {
          // 3_1：缓冲区重复（四等分重复）=> 4段相同波形重复
          juce::Path p;
          const int segs = 4;
          bool started = false;
          for (int s = 0; s < segs; ++s) {
            for (int i = 0; i <= 160; ++i) {
              const float uLocal = static_cast<float>(i) / 160.0f;
              const float u = (static_cast<float>(s) + uLocal) / static_cast<float>(segs);
              const float wv = baseWave(uLocal); // 重复：只看局部u
              const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
              const auto pt = juce::Point<float>(xAt(u), yAt(v));
              if (!started) {
                p.startNewSubPath(pt);
                started = true;
              } else {
                p.lineTo(pt);
              }
            }
            started = false;
          }
          strokeGlow(p, 1.0f + 1.1f * flash);

          g.setColour(juce::Colours::white.withAlpha(0.020f + 0.14f * flash));
          for (int s = 1; s < segs; ++s) {
            const float u = static_cast<float>(s) / static_cast<float>(segs);
            const float xx = xAt(u);
            g.drawLine(xx, scope.getY(), xx, scope.getBottom(), 1.0f);
          }
          break;
        }

        case 1: {
          // 3_2：消音错误 => “平线 + 缺口 + 黑块遮挡”
          juce::Path p;
          const float mid = 0.5f;
          bool started = false;
          for (int i = 0; i <= 520; ++i) {
            const float u = static_cast<float>(i) / 520.0f;
            const float r = hash01(base + 900 + i * 19 + static_cast<int>(t2 * 60.0f) * 17);
            if (r < (0.02f + 0.16f * flash)) {
              started = false;
              continue;
            }
            const float vv = mid + (hash01(base + 333 + i + static_cast<int>(t2 * 40.0f)) - 0.5f) * 0.10f * flash;
            const auto pt = juce::Point<float>(xAt(u), yAt(vv));
            if (!started) {
              p.startNewSubPath(pt);
              started = true;
            } else {
              p.lineTo(pt);
            }
          }
          strokeGlow(p, 0.95f + 1.0f * flash);

          const int blocks = 5 + static_cast<int>(std::floor(flash * 7.0f));
          for (int i = 0; i < blocks; ++i) {
            const float u = hash01(base + 1400 + i * 31 + static_cast<int>(t2 * 20.0f) * 7);
            const float w = (0.10f + 0.28f * hash01(base + 1500 + i * 17)) * scope.getWidth();
            const float h = (0.10f + 0.40f * hash01(base + 1600 + i * 13)) * scope.getHeight();
            const float xx = scope.getX() + u * (scope.getWidth() - w);
            const float yy = scope.getY() + (hash01(base + 1700 + i * 23) * 0.80f) * (scope.getHeight() - h);
            g.setColour(juce::Colours::black.withAlpha(0.05f + 0.16f * flash));
            g.fillRect(juce::Rectangle<float>(xx, yy, w, h));
          }
          break;
        }

        case 2: {
          // 3_3：反向切片 => “正向 + 反向镜像叠加”
          juce::Path p1, p2;
          for (int i = 0; i <= 560; ++i) {
            const float u = static_cast<float>(i) / 560.0f;
            const float wv = baseWave(u);
            const float v1 = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
            const float v2 = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * baseWave(1.0f - u));
            const auto pt1 = juce::Point<float>(xAt(u), yAt(v1));
            const auto pt2 = juce::Point<float>(xAt(u), yAt(v2));
            if (i == 0) {
              p1.startNewSubPath(pt1);
              p2.startNewSubPath(pt2);
            } else {
              p1.lineTo(pt1);
              p2.lineTo(pt2);
            }
          }
          strokeGlow(p1, 1.0f + 1.05f * flash);
          g.setColour(juce::Colours::white.withAlpha(0.018f + 0.12f * flash));
          g.strokePath(p2, juce::PathStrokeType(0.9f + 0.9f * flash));
          break;
        }

        case 3: {
          // 3_4：Sample&Hold/bitcrush => “台阶保持波形 + 码流条”
          juce::Path p;
          const int steps = 34;
          const float hold = 1.0f / static_cast<float>(steps);

          for (int s = 0; s < steps; ++s) {
            const float u0 = s * hold;
            const float u1 = (s + 1) * hold;
            float wv = baseWave(u0);
            const float q = 6.0f + 10.0f * flash;
            wv = std::round(wv * q) / q;
            const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);

            const float x0 = xAt(u0);
            const float x1 = xAt(u1);
            const float yv = yAt(v);

            if (s == 0)
              p.startNewSubPath(x0, yv);
            else
              p.lineTo(x0, yv);

            p.lineTo(x1, yv);
          }
          strokeGlow(p, 1.0f + 1.15f * flash);

          g.setColour(juce::Colours::white.withAlpha(0.012f + 0.10f * flash));
          for (int i = 0; i < 26; ++i) {
            const float u = hash01(base + 3000 + i * 41 + static_cast<int>(t2 * 30.0f));
            const float xx = xAt(u);
            const float hh = (0.10f + 0.85f * hash01(base + 3100 + i * 17)) * scope.getHeight();
            const float yy = scope.getCentreY() - hh * 0.5f;
            g.fillRect(juce::Rectangle<float>(xx, yy, 1.0f, hh));
          }
          break;
        }

        case 4: {
          // 3_5：随机切片跳读 => “16片拼接乱序波形 + 断裂”
          juce::Path p;
          const int slices = 16;
          bool started = false;

          for (int s = 0; s < slices; ++s) {
            const float u0 = static_cast<float>(s) / static_cast<float>(slices);
            const float u1 = static_cast<float>(s + 1) / static_cast<float>(slices);

            const int src = static_cast<int>(std::floor(hash01(base + 4000 + s * 37 + static_cast<int>(t2 * 12.0f)) * slices)) % slices;
            const float su0 = static_cast<float>(src) / static_cast<float>(slices);

            for (int i = 0; i <= 40; ++i) {
              const float uu = static_cast<float>(i) / 40.0f;
              const float u = u0 + uu * (u1 - u0);
              const float su = su0 + uu * (1.0f / static_cast<float>(slices));

              float wv = baseWave(su);
              wv += (hash01(base + 4100 + s * 13 + i * 7 + static_cast<int>(t2 * 60.0f)) - 0.5f) * 2.0f * (0.07f + 0.10f * flash);
              wv = juce::jlimit(-1.0f, 1.0f, wv);

              const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
              const auto pt = juce::Point<float>(xAt(u), yAt(v));

              const float rr = hash01(base + 4200 + s * 29 + i * 11 + static_cast<int>(t2 * 20.0f));
              if (rr < (0.008f + 0.06f * flash)) {
                started = false;
                continue;
              }

              if (!started) {
                p.startNewSubPath(pt);
                started = true;
              } else {
                p.lineTo(pt);
              }
            }
            started = false;
          }

          strokeGlow(p, 1.0f + 1.20f * flash);

          g.setColour(juce::Colours::white.withAlpha(0.012f + 0.12f * flash));
          for (int s = 1; s < slices; ++s) {
            if ((s % 2) == 0) {
              const float u = static_cast<float>(s) / static_cast<float>(slices);
              const float xx = xAt(u);
              g.drawLine(xx, scope.getY(), xx, scope.getBottom(), 1.0f);
            }
          }
          break;
        }

        case 5: {
          // 3_6：颗粒重排 => 大量随机小颗粒散布
          juce::Path p;
          bool started = false;
          const int grains = 64;
          for (int s = 0; s < grains; ++s) {
            const float u0 = static_cast<float>(s) / static_cast<float>(grains);
            const float u1 = static_cast<float>(s + 1) / static_cast<float>(grains);
            const int src = static_cast<int>(std::floor(hash01(base + 5000 + s * 41 + static_cast<int>(t2 * 25.0f)) * grains)) % grains;
            const float su = static_cast<float>(src) / static_cast<float>(grains);
            const float wv = baseWave(su + 0.5f / static_cast<float>(grains));
            const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
            const auto pt = juce::Point<float>(xAt((u0 + u1) * 0.5f), yAt(v));
            if (!started) {
              p.startNewSubPath(pt);
              started = true;
            } else {
              p.lineTo(pt);
            }
          }
          strokeGlow(p, 0.9f + 1.1f * flash);
          g.setColour(juce::Colours::white.withAlpha(0.02f + 0.14f * flash));
          for (int s = 0; s < grains; s += 2) {
            const float u = static_cast<float>(s) / static_cast<float>(grains);
            const float xx = xAt(u);
            g.drawLine(xx, scope.getY(), xx, scope.getBottom(), 1.0f);
          }
          break;
        }

        case 6: {
          // 3_7：磁带停止 => 先陡后平的减速曲线
          juce::Path p;
          bool started = false;
          for (int i = 0; i <= 200; ++i) {
            const float t = static_cast<float>(i) / 200.0f;
            const float u = 1.0f - std::pow(1.0f - t, 3.0f);  // 先快后慢
            const float wv = baseWave(u);
            const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
            const auto pt = juce::Point<float>(xAt(u), yAt(v));
            if (!started) {
              p.startNewSubPath(pt);
              started = true;
            } else {
              p.lineTo(pt);
            }
          }
          strokeGlow(p, 1.0f + 1.05f * flash);
          break;
        }

        case 7: {
          // 3_8：1-bit 降采样 => 只有 ±1 两个电平的方波
          juce::Path p;
          const int steps = 16;
          for (int s = 0; s < steps; ++s) {
            const float u0 = static_cast<float>(s) / static_cast<float>(steps);
            const float u1 = static_cast<float>(s + 1) / static_cast<float>(steps);
            const float wv = (baseWave(u0) >= 0.0f) ? 1.0f : -1.0f;
            const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
            const float x0 = xAt(u0);
            const float x1 = xAt(u1);
            const float yv = yAt(v);
            if (s == 0)
              p.startNewSubPath(x0, yv);
            else
              p.lineTo(x0, yv);
            p.lineTo(x1, yv);
          }
          strokeGlow(p, 1.0f + 1.1f * flash);
          break;
        }

        case 8: {
          // 3_9：比特翻转 => 大量随机尖刺/爆点
          juce::Path p;
          bool started = false;
          for (int i = 0; i <= 200; ++i) {
            const float u = static_cast<float>(i) / 200.0f;
            float wv = baseWave(u);
            const float r = hash01(base + 6000 + i * 31 + static_cast<int>(t2 * 90.0f));
            if (r < (0.10f + 0.30f * flash))
              wv = (r - 0.5f) * 4.0f;  // 爆点
            wv = juce::jlimit(-1.0f, 1.0f, wv);
            const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
            const auto pt = juce::Point<float>(xAt(u), yAt(v));
            if (!started) {
              p.startNewSubPath(pt);
              started = true;
            } else {
              p.lineTo(pt);
            }
          }
          strokeGlow(p, 1.0f + 1.15f * flash);
          break;
        }

        case 9: {
          // 3_10：扫频环调 => 频率渐高的正弦
          juce::Path p;
          bool started = false;
          for (int i = 0; i <= 320; ++i) {
            const float u = static_cast<float>(i) / 320.0f;
            const float cycles = 4.0f + 60.0f * u;
            const float wv = std::sin(juce::MathConstants<float>::twoPi * cycles * u);
            const float v = juce::jlimit(0.02f, 0.98f, 0.5f + 0.92f * wv);
            const auto pt = juce::Point<float>(xAt(u), yAt(v));
            if (!started) {
              p.startNewSubPath(pt);
              started = true;
            } else {
              p.lineTo(pt);
            }
          }
          strokeGlow(p, 1.0f + 1.1f * flash);
          break;
        }
      }

      g.restoreState();
    }
  }

  juce::Rectangle<float> imagesArea, sliderBand;
  splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

  const float totalGap = kColumnGap * (columnCount - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

  const float t = static_cast<float>(animTimeSeconds);

  // 第二列（waveshaper）函数曲线预览：背景之上、控件之下
  // 坐标：界面中心为(0,0)，右上角(1,1)，左下角(-1,-1)
  {
    auto hash01 = [](int n) -> float {
      const float x = std::sin(static_cast<float>(n) * 12.9898f) * 43758.5453f;
      return x - std::floor(x);
    };

    const auto b = getLocalBounds().toFloat();
    const float cx = b.getCentreX();
    const float cy = b.getCentreY();
    const float sx = juce::jmax(1.0f, b.getWidth() * 0.5f);
    const float sy = juce::jmax(1.0f, b.getHeight() * 0.5f);

    auto toScreen = [&](float xn, float yn) -> juce::Point<float> {
      return { cx + xn * sx, cy - yn * sy };
    };

    const float t2 = static_cast<float>(animTimeSeconds);
    const int modeSeed = 7001 + processor.getColumn2SelectedIndex() * 1337;

    // 故障门控：偶尔“撕裂/断续/闪烁”
    const float gate = std::sin(t2 * 1.65f + hash01(modeSeed) * 6.28318f);
    const float glitch = juce::jlimit(0.0f, 1.0f, (gate - 0.60f) / 0.35f);
    const float g2 = glitch * glitch;

    // 画一条很淡的对角参考线（y=x），帮助感知映射（仍在控件之下）
    {
      const float a = 0.045f + 0.045f * g2;
      g.setColour(juce::Colours::white.withAlpha(a));
      g.drawLine(toScreen(-1.0f, -1.0f).x, toScreen(-1.0f, -1.0f).y,
                 toScreen(1.0f, 1.0f).x,   toScreen(1.0f, 1.0f).y, 1.1f);
    }

    // 主曲线
    juce::Path p;
    bool started = false;

    static constexpr int kPoints = 320;
    const int frame = static_cast<int>(std::floor(t2 * 60.0f));

    for (int i = 0; i < kPoints; ++i) {
      const float xn = -1.0f + 2.0f * (static_cast<float>(i) / static_cast<float>(kPoints - 1));
      float y = processor.evaluateColumn2Waveshaper(xn);
      y = juce::jlimit(-1.0f, 1.0f, y);

      // glitch：随机丢点（形成断裂）
      const float dropP = 0.02f + 0.10f * g2;
      const float r = hash01(modeSeed + i * 17 + frame * 31);
      if (r < dropP) {
        started = false;
        continue;
      }

      // glitch：轻微坐标抖动 + 扫描线偏移
      const float jx = (hash01(modeSeed + 1009 + i * 29 + frame * 13) - 0.5f) * 2.0f;
      const float jy = (hash01(modeSeed + 2003 + i * 31 + frame * 17) - 0.5f) * 2.0f;

      const float scan = std::sin(t2 * 22.0f + xn * 7.0f);
      const float xj = xn + jx * (0.0025f + 0.010f * g2);
      const float yj = y  + (jy * (0.0035f + 0.014f * g2)) + (scan * (0.0010f + 0.0040f * g2));

      const auto pt = toScreen(xj, yj);
      if (!started) {
        p.startNewSubPath(pt);
        started = true;
      } else {
        p.lineTo(pt);
      }
    }

    // 闪烁：透明度与线宽轻微波动
    const float flick = 0.5f + 0.5f * std::sin(t2 * 9.0f + hash01(modeSeed + frame) * 6.28318f);
    const float alpha = 0.115f + 0.085f * flick + 0.175f * g2;
    const float w = 1.35f + 1.10f * g2;

    // 轻微“辉光”（整体削弱一些）
    g.setColour(juce::Colours::white.withAlpha(alpha * 0.42f));
    g.strokePath(p, juce::PathStrokeType(w * 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // RGB色移：红/蓝两根偏移线（偏移量有微弱波动，glitch时更明显）
    {
      // 注意：这里的偏移是“像素”，需要足够大才看得出色分离
      const float sepBase = 2.60f + 5.00f * g2; // px（显著加大，确保肉眼可见）
      const float sepJit = (hash01(modeSeed + 9009 + frame * 7) - 0.5f) * 1.20f;
      const float sep = juce::jmax(0.0f, sepBase + sepJit);
      const float wob = std::sin(t2 * (6.0f + 8.0f * g2) + hash01(modeSeed + 77) * 6.28318f);
      const float dx = sep * (0.85f + 0.35f * wob);
      const float dy = sep * (0.10f * wob);

      juce::Path pr = p;
      pr.applyTransform(juce::AffineTransform::translation(dx, dy));
      g.setColour(juce::Colours::red.withAlpha(alpha * (0.3f + 0.10f * g2)));
      g.strokePath(pr, juce::PathStrokeType(w * 0.70f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

      juce::Path pb = p;
      pb.applyTransform(juce::AffineTransform::translation(-dx, -dy));
      g.setColour(juce::Colours::blue.withAlpha(alpha * (0.3f + 0.10f * g2)));
      g.strokePath(pb, juce::PathStrokeType(w * 0.70f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // 主白线略削弱
    g.setColour(juce::Colours::white.withAlpha(alpha * 0.78f));
    g.strokePath(p, juce::PathStrokeType(w * 0.92f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  }

  // 画三列图片（老虎机式：无限循环，无上下限）
  // 需求：动画要铺满整个界面的上下边界；底部控制条在其之上（绘制顺序更靠后，交互也仍走控制条）。
  for (int c = 0; c < columnCount; ++c) {
    const float x = imagesArea.getX() + c * (colW + kColumnGap);

    // 只限制横向为“列宽”，纵向铺满整个Editor高度。
    juce::Rectangle<float> col = { x, 0.0f, colW, static_cast<float>(getHeight()) };

    // clip到列区域内
    g.saveState();
    g.reduceClipRegion(col.toNearestInt());

    const auto& imgs = columnImages[static_cast<size_t>(c)];
    const auto& params = floatyParams[static_cast<size_t>(c)];

    // 计算该列一轮的总高度（根据列宽按比例缩放）
    float cycleH = 0.0f;
    for (const auto& img : imgs) {
      if (!img.isValid())
        continue;
      const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
      const float drawW = col.getWidth();
      const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;
      cycleH += drawH;
    }

    if (cycleH <= 0.0f) {
      g.restoreState();
      continue;
    }

    // 把offset折叠到一个周期内（允许无限拖动，但显示为循环）
    float off = wrapPositive(columnOffsets[static_cast<size_t>(c)], cycleH);

    // 让第一轮的起点落在列顶部之上，保证能填满整个列
    float y = col.getY() + off;
    while (y > col.getY())
      y -= cycleH;

    // 反复绘制序列直到填满列底部
    while (y < col.getBottom()) {
      for (size_t i = 0; i < imgs.size(); ++i) {
        const auto& img = imgs[i];
        if (!img.isValid())
          continue;

        const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
        const float drawW = col.getWidth();
        const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;

        // 像素对齐作为基础，减少静态“虚”
        juce::Rectangle<float> dst(
            std::round(col.getX()),
            std::round(y),
            std::round(drawW),
            std::round(drawH));

        // 每张图都有自己微微“漂浮在宇宙中”的动态
        FloatyParams p;
        if (i < params.size())
          p = params[i];

        const float rot = p.rotAmpRad * std::sin(t * p.rotSpeed + p.phase1);
        const float scale = 1.0f + p.scaleAmp * std::sin(t * p.scaleSpeed + p.phase2);
        const float dx = p.posAmpX * std::sin(t * p.posSpeedX + p.phase3);
        const float dy = p.posAmpY * std::sin(t * p.posSpeedY + p.phase4);

        const float cx = dst.getCentreX();
        const float cy = dst.getCentreY();

        // 距离中心越远越小：模拟绕圆形轮盘滚动（中心=最近=最大）
        const float halfH = juce::jmax(1.0f, col.getHeight() * 0.5f);
        const float u0 = (cy - col.getCentreY()) / halfH;                 // [-1, 1]附近
        const float u = juce::jlimit(-1.0f, 1.0f, u0);
        const float angle = u * juce::MathConstants<float>::halfPi;       // 顶/底 = ±90°
        float depth = std::cos(angle);                                    // [0, 1]
        depth = std::pow(juce::jlimit(0.0f, 1.0f, depth), kWheelDepthPow);

        const float wheelScale = kWheelMinScale + (kWheelMaxScale - kWheelMinScale) * depth;
        const float arcX = (1.0f - depth) * kWheelArcX * std::sin(angle);
        const float alpha = kWheelMinAlpha + (1.0f - kWheelMinAlpha) * depth;

        // 原有“漂浮”动态叠加在轮盘透视之上
        const float totalScale = wheelScale * scale;
        const float totalDx = dx + arcX;
        const float totalDy = dy;

        // 远处旋转更小一点，避免看起来“飘得太乱”
        const float rotFinal = rot * (0.35f + 0.65f * depth);

        juce::AffineTransform tr = juce::AffineTransform::translation(totalDx, totalDy)
                                      .rotated(rotFinal, cx, cy)
                                      .scaled(totalScale, totalScale, cx, cy);

        g.saveState();
        g.setOpacity(alpha);
        g.addTransform(tr);
        g.drawImage(img, dst);
        g.restoreState();

        y += drawH;
        if (y >= col.getBottom())
          break;
      }
    }

    g.restoreState();
  }


  // 随机按钮：右上角（仅绘制图片，不要底框/底色）
  {
    const auto r = getRandButtonVisualRect();

    if (r.getWidth() > 2.0f && r.getHeight() > 2.0f) {
      g.saveState();

      if (randButtonImage.isValid()) {
        g.setOpacity(1.0f);
        g.drawImageWithin(randButtonImage,
                          static_cast<int>(std::round(r.getX())),
                          static_cast<int>(std::round(r.getY())),
                          static_cast<int>(std::round(r.getWidth())),
                          static_cast<int>(std::round(r.getHeight())),
                          juce::RectanglePlacement::centred);
      } else {
        // 兜底：没有图片时不画底框，避免产生“灰底边界”
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawFittedText("RND", r.toNearestInt(), juce::Justification::centred, 1);
      }

      g.restoreState();
    }
  }

  // About按钮：放在随机按钮下方（仅绘制图片，不要底框/底色）
  {
    const auto r = getAboutButtonVisualRect();

    if (r.getWidth() > 2.0f && r.getHeight() > 2.0f) {
      g.saveState();

      if (aboutButtonImage.isValid()) {
        g.setOpacity(1.0f);
        g.drawImageWithin(aboutButtonImage,
                          static_cast<int>(std::round(r.getX())),
                          static_cast<int>(std::round(r.getY())),
                          static_cast<int>(std::round(r.getWidth())),
                          static_cast<int>(std::round(r.getHeight())),
                          juce::RectanglePlacement::centred);
      } else {
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.drawFittedText("ABOUT", r.toNearestInt(), juce::Justification::centred, 1);
      }

      g.restoreState();
    }
  }

  // 三列下方：横向控制条（白色，老式进度条风格）
  {
    auto hash01 = [](int n) -> float {
      const float x = std::sin(static_cast<float>(n) * 12.9898f) * 43758.5453f;
      return x - std::floor(x);
    };

    for (int c = 0; c < columnCount; ++c) {
      auto r = getSliderRect(c);

      const float norm = sliderNorm[static_cast<size_t>(c)];
      auto inner = r.reduced(2.0f, 2.0f);

      // 外框：矩形
      g.setColour(juce::Colours::white.withAlpha(0.85f));
      g.drawRect(r.toNearestInt(), 1);

      // 轨道底色（淡白）
      g.setColour(juce::Colours::white.withAlpha(0.10f));
      g.fillRect(inner);

      // --- 第三列：离散点 + glitch真实反馈（闪光 + 抖动片段） ---
      if (c == 2) {
        const float t2 = static_cast<float>(animTimeSeconds);
        const float flash = juce::jlimit(0.0f, 1.0f, juce::jmax(col3Flash, col3UiAmount01));

        // 轻微“闪光”覆盖
        if (flash > 0.001f) {
          juce::ColourGradient glow(
              juce::Colours::white.withAlpha(0.06f + 0.28f * flash), inner.getX(), inner.getY(),
              juce::Colours::white.withAlpha(0.00f), inner.getRight(), inner.getY(), false);
          g.setGradientFill(glow);
          g.fillRect(inner);
        }

        // 不连续片段：多段小块，上下抖动（类似显示错误）
        if (flash > 0.02f) {
          const int base = 30011;
          const int blocks = 11;
          for (int i = 0; i < blocks; ++i) {
            const float u = hash01(base + i * 17 + static_cast<int>(t2 * 60.0f));
            const float v = hash01(base + i * 31 + 7);

            const float bw = inner.getWidth() * (0.06f + 0.10f * v);
            const float bx = inner.getX() + u * (inner.getWidth() - bw);

            const float jy = (hash01(base + i * 23 + static_cast<int>(t2 * 90.0f)) - 0.5f) * 2.0f;
            const float by = inner.getCentreY() - 1.0f + jy * (inner.getHeight() * 0.55f) * flash;

            const float bh = 1.5f + 2.5f * hash01(base + i * 13 + 3);

            g.setColour(juce::Colours::white.withAlpha(0.10f + 0.35f * flash));
            g.fillRect(juce::Rectangle<float>(bx, by, bw, bh));
          }
        }

        // 7个等距点（从左到右）
        static constexpr int kSteps = 7;
        const int idx = juce::jlimit(0, kSteps - 1, static_cast<int>(std::lround(norm * static_cast<float>(kSteps - 1))));

        const float pad = 5.0f;
        const float x0 = inner.getX() + pad;
        const float x1 = inner.getRight() - pad;
        const float cy = inner.getCentreY();

        // 底线（很淡）
        g.setColour(juce::Colours::white.withAlpha(0.18f));
        g.drawLine(x0, cy, x1, cy, 1.0f);

        for (int i = 0; i < kSteps; ++i) {
          const float u = (kSteps == 1) ? 0.0f : (static_cast<float>(i) / static_cast<float>(kSteps - 1));
          float x = x0 + u * (x1 - x0);

          // glitch时点也轻微上下抖动
          float dy = 0.0f;
          if (flash > 0.02f) {
            const float j = (hash01(9007 + i * 97 + static_cast<int>(t2 * 70.0f)) - 0.5f) * 2.0f;
            dy = j * (inner.getHeight() * 0.18f) * flash;
          }

          const bool selected = (i == idx);
          const float rad = selected ? 3.2f : 2.2f;

          if (selected) {
            g.setColour(juce::Colours::white.withAlpha(0.95f));
            g.fillEllipse(x - rad, (cy + dy) - rad, rad * 2.0f, rad * 2.0f);
          } else {
            g.setColour(juce::Colours::white.withAlpha(0.45f));
            g.drawEllipse(x - rad, (cy + dy) - rad, rad * 2.0f, rad * 2.0f, 1.0f);
          }
        }

        // 进度指示：按phase画一个小竖条（方便看“是否接近触发/正在触发”）
        const float px = x0 + juce::jlimit(0.0f, 1.0f, col3UiPhase01) * (x1 - x0);
        g.setColour(juce::Colours::white.withAlpha(0.35f + 0.45f * flash));
        g.drawLine(px, r.getY() - 2.0f, px, r.getBottom() + 2.0f, 1.0f);

        // flash时边框更亮一点
        if (flash > 0.02f) {
          g.setColour(juce::Colours::white.withAlpha(0.25f + 0.35f * flash));
          g.drawRect(r.toNearestInt(), 1);
        }

        continue;
      }

      // 填充（无分格子）
      auto fill = inner;
      fill.setWidth(inner.getWidth() * juce::jlimit(0.0f, 1.0f, norm));

      if (fill.getWidth() > 0.5f) {
        juce::ColourGradient grad(
            juce::Colours::white.withAlpha(0.95f), fill.getX(), fill.getY(),
            juce::Colours::white.withAlpha(0.55f), fill.getRight(), fill.getY(), false);
        g.setGradientFill(grad);
        g.fillRect(fill);
      }

      // 故障元素：轻微扫描线 + 瞬时抖动块（不影响交互，仅视觉）
      const float t2 = static_cast<float>(animTimeSeconds);
      const int base = 10007 + c * 7919;
      const float glitchGate = std::sin(t2 * (2.6f + 0.3f * c) + hash01(base) * 6.28318f);
      const bool glitchOn = glitchGate > 0.82f;

      // 扫描线（常驻，但很弱）
      {
        const float scanY = inner.getY() + std::fmod(t2 * (28.0f + 3.0f * c), juce::jmax(1.0f, inner.getHeight()));
        g.setColour(juce::Colours::white.withAlpha(0.10f));
        g.fillRect(juce::Rectangle<float>(inner.getX(), scanY, inner.getWidth(), 1.0f));
      }

      if (glitchOn) {
        const float jitter = (hash01(base + static_cast<int>(t2 * 60.0f)) - 0.5f) * 2.0f;

        // 1) 在填充区域里挖掉/叠加几段小块，模拟“故障丢帧”
        const int blocks = 3 + (c % 2);
        for (int i = 0; i < blocks; ++i) {
          const float u = hash01(base + i * 17 + static_cast<int>(t2 * 30.0f));
          const float v = hash01(base + i * 23 + 77);
          const float w = (0.10f + 0.20f * v) * inner.getWidth();
          const float x = inner.getX() + u * (inner.getWidth() - w);
          const float y = inner.getY() + (0.15f + 0.70f * v) * inner.getHeight();
          const float h = 1.5f + 2.5f * hash01(base + i * 31 + 3);

          // 叠加亮块
          g.setColour(juce::Colours::white.withAlpha(0.18f));
          g.fillRect(juce::Rectangle<float>(x, y, w, h));

          // 同时在少量位置做“缺口”
          if (i == 1) {
            g.setColour(juce::Colours::black.withAlpha(0.18f));
            g.fillRect(juce::Rectangle<float>(x + jitter, y - 1.0f, w * 0.7f, h));
          }
        }

        // 2) 边框局部闪烁
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        const float fx0 = inner.getX() + hash01(base + 99) * (inner.getWidth() * 0.6f);
        const float fw = inner.getWidth() * (0.25f + 0.20f * hash01(base + 123));
        g.drawLine(fx0, r.getY(), juce::jmin(r.getRight(), fx0 + fw), r.getY(), 1.0f);
      }

      // 指示器：小竖条（故障时轻微抖动）
      float px = r.getX() + norm * r.getWidth();
      if (glitchOn)
        px += (hash01(base + static_cast<int>(t2 * 90.0f)) - 0.5f) * 3.0f;

      g.setColour(juce::Colours::white.withAlpha(0.9f));
      g.drawLine(px, r.getY() - 2.0f, px, r.getBottom() + 2.0f, 1.0f);
    }
  }

  // 分割线：只画中间一小段灰线（不从上到下）
  {
    const float lineLen = juce::jmin(120.0f, imagesArea.getHeight() * 0.45f);
    const float y0 = imagesArea.getCentreY() - lineLen * 0.5f;
    const float y1 = y0 + lineLen;

    g.setColour(juce::Colour(0xFF808080).withAlpha(0.7f));

    const float xSep1 = imagesArea.getX() + colW + (kColumnGap * 0.5f);
    const float xSep2 = imagesArea.getX() + 2.0f * colW + (kColumnGap * 1.5f);

    g.drawLine(xSep1, y0, xSep1, y1, 1.0f);
    g.drawLine(xSep2, y0, xSep2, y1, 1.0f);
  }
}

void PluginEditor::resized() {
  // 拖拽调整窗口大小时，保持当前居中选择不发生变化
  const auto b = getLocalBounds();
  if (b != lastBoundsForSelection) {
    recenterColumnsToProcessorSelection(b);
    lastBoundsForSelection = b;
  }
}

void PluginEditor::mouseDown(const juce::MouseEvent& e) {
  // About按钮：弹出免责声明/联系方式
  if (hitTestAboutButton(e.position)) {
    activeSliderColumn = -1;
    activeDragColumn = -1;

    showAboutDialog();
    repaint();
    return;
  }

  // 随机按钮：触发三列老虎机滚动
  if (hitTestRandButton(e.position)) {
    activeSliderColumn = -1;
    activeDragColumn = -1;

    startRandomSlotSpin();
    repaint();
    return;
  }

  // 正在滚动时，避免用户拖动打断
  if (slotSpinActive)
    return;

  activeSliderColumn = hitTestSlider(e.position);
  if (activeSliderColumn >= 0) {
    activeDragColumn = -1;

    auto r = getSliderRect(activeSliderColumn);
    const float norm = (e.position.x - r.getX()) / juce::jmax(1.0f, r.getWidth());
    setSliderNormValue(activeSliderColumn, norm);
    repaint();
    return;
  }

  activeDragColumn = hitTestColumn(e.position);
  if (activeDragColumn < 0)
    return;

  snapping[static_cast<size_t>(activeDragColumn)] = false;

  dragStartY = e.position.y;
  dragStartOffset = columnOffsets[static_cast<size_t>(activeDragColumn)];

  // 开始搓碟：先启用，rate初始为0（按下不动时相当于停住）
  scratchActive = true;
  scratchLastDragMs = juce::Time::getMillisecondCounterHiRes();
  scratchLastDragY = e.position.y;
  processor.setScratchEnabled(true);
  processor.setScratchRate(0.0f);
}

void PluginEditor::mouseDrag(const juce::MouseEvent& e) {
  if (slotSpinActive)
    return;

  if (activeSliderColumn >= 0) {
    auto r = getSliderRect(activeSliderColumn);
    const float norm = (e.position.x - r.getX()) / juce::jmax(1.0f, r.getWidth());
    setSliderNormValue(activeSliderColumn, norm);
    repaint();
    return;
  }

  if (activeDragColumn < 0)
    return;

  const float dy = e.position.y - dragStartY;
  columnOffsets[static_cast<size_t>(activeDragColumn)] = dragStartOffset + dy;

  // 搓碟rate：用鼠标Y速度估算（像“搓碟”一样快推=快放，反向=倒放）
  if (scratchActive) {
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double dtMs = juce::jmax(0.001, nowMs - scratchLastDragMs);
    const float v = (e.position.y - scratchLastDragY) / static_cast<float>(dtMs);  // px/ms

    // 经验缩放：10px/16ms ~= 0.625px/ms -> ~1x
    constexpr float kRateScale = 1.6f;
    float rate = -v * kRateScale;
    rate = juce::jlimit(-3.5f, 3.5f, rate);

    processor.setScratchRate(rate);

    scratchLastDragMs = nowMs;
    scratchLastDragY = e.position.y;
  }

  repaint();
}

void PluginEditor::mouseUp(const juce::MouseEvent&) {
  if (activeSliderColumn >= 0) {
    activeSliderColumn = -1;
    return;
  }

  if (activeDragColumn >= 0)
    startSnapToNearestCenteredImage(activeDragColumn);

  activeDragColumn = -1;

  // 结束搓碟
  scratchActive = false;

  // 如果当前正在随机滚动触发“倒带”，则由随机动画接管scratch开关
  if (!slotSpinScratchRewindActive) {
    processor.setScratchRate(0.0f);
    processor.setScratchEnabled(false);
  }
}

void PluginEditor::startSnapToNearestCenteredImage(int columnIndex) {
  if (columnIndex < 0 || columnIndex >= columnCount)
    return;

  juce::Rectangle<float> imagesArea, sliderBand;
  splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

  const float totalGap = kColumnGap * (columnCount - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

  const float x = imagesArea.getX() + columnIndex * (colW + kColumnGap);
  juce::Rectangle<float> col = { x, imagesArea.getY(), colW, imagesArea.getHeight() };

  const auto& imgs = columnImages[static_cast<size_t>(columnIndex)];

  // 计算每张图的高度与其“中心点”在一个cycle中的位置
  std::vector<float> centersInCycle;
  centersInCycle.reserve(imgs.size());

  float cycleH = 0.0f;
  for (auto& img : imgs) {
    if (!img.isValid())
      continue;

    const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
    const float drawW = col.getWidth();
    const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;

    centersInCycle.push_back(cycleH + drawH * 0.5f);
    cycleH += drawH;
  }

  if (cycleH <= 0.0f || centersInCycle.empty())
    return;

  const float currentOffset = columnOffsets[static_cast<size_t>(columnIndex)];
  const float offCur = wrapPositive(currentOffset, cycleH);

  // 希望“列中心”对应到cycle中的某个图片中心
  const float centerPos = col.getHeight() * 0.5f;

  float bestDelta = 0.0f;
  float bestAbs = std::numeric_limits<float>::max();

  for (float centerInCycle : centersInCycle) {
    // (centerPos - off) mod cycleH = centerInCycle
    // => off = (centerPos - centerInCycle) mod cycleH
    const float offTarget = wrapPositive(centerPos - centerInCycle, cycleH);
    const float d = shortestDelta(offCur, offTarget, cycleH);

    const float ad = std::abs(d);
    if (ad < bestAbs) {
      bestAbs = ad;
      bestDelta = d;
    }
  }

  snapping[static_cast<size_t>(columnIndex)] = true;
  snapTargets[static_cast<size_t>(columnIndex)] = currentOffset + bestDelta;
}

int PluginEditor::getCenteredImageIndex(int columnIndex) const {
  if (columnIndex < 0 || columnIndex >= columnCount)
    return 0;

  juce::Rectangle<float> imagesArea, sliderBand;
  splitLayoutAreas(getLocalBounds(), imagesArea, sliderBand);

  const float totalGap = kColumnGap * (columnCount - 1);
  const float colW = (imagesArea.getWidth() - totalGap) / static_cast<float>(columnCount);

  const float x = imagesArea.getX() + columnIndex * (colW + kColumnGap);
  juce::Rectangle<float> col = { x, imagesArea.getY(), colW, imagesArea.getHeight() };

  const auto& imgs = columnImages[static_cast<size_t>(columnIndex)];
  if (imgs.empty())
    return 0;

  std::vector<std::pair<float, int>> centers;
  centers.reserve(imgs.size());

  float cycleH = 0.0f;
  for (int i = 0; i < static_cast<int>(imgs.size()); ++i) {
    const auto& img = imgs[static_cast<size_t>(i)];
    if (!img.isValid())
      continue;

    const float aspect = static_cast<float>(img.getWidth()) / static_cast<float>(img.getHeight());
    const float drawW = col.getWidth();
    const float drawH = (aspect > 0.0f) ? (drawW / aspect) : 0.0f;

    centers.push_back({ cycleH + drawH * 0.5f, i });
    cycleH += drawH;
  }

  if (cycleH <= 0.0f || centers.empty())
    return 0;

  const float currentOffset = columnOffsets[static_cast<size_t>(columnIndex)];
  const float offCur = wrapPositive(currentOffset, cycleH);
  const float centerPos = col.getHeight() * 0.5f;

  float bestAbs = std::numeric_limits<float>::max();
  int bestIndex = centers.front().second;

  for (auto [centerInCycle, idx] : centers) {
    const float offTarget = wrapPositive(centerPos - centerInCycle, cycleH);
    const float d = shortestDelta(offCur, offTarget, cycleH);
    const float ad = std::abs(d);
    if (ad < bestAbs) {
      bestAbs = ad;
      bestIndex = idx;
    }
  }

  return bestIndex;
}

void PluginEditor::pushUiStateToProcessor() {
  // 第一列：0->1_1(软削波), 1->1_2(硬削波), 2->1_3(折返削波), 3->1_4(非对称削波)
  {
    const int idx = getCenteredImageIndex(0);
    processor.setColumn1SelectedIndex(idx);
    processor.setColumn1Mode(static_cast<PluginProcessor::Column1Mode>(idx));
  }

  // 第二列：0..14 对应 2_1..2_15
  {
    const int idx = juce::jlimit(0, 14, getCenteredImageIndex(1));
    processor.setColumn2SelectedIndex(idx);
    processor.setColumn2Mode(static_cast<PluginProcessor::Column2Mode>(idx));
  }

  // 第三列：0..9 对应 3_1..3_10
  {
    const int idx = juce::jlimit(0, 9, getCenteredImageIndex(2));
    processor.setColumn3SelectedIndex(idx);
  }
}

void PluginEditor::timerCallback() {
  animTimeSeconds = (juce::Time::getMillisecondCounterHiRes() - animStartMs) / 1000.0;

  // 每个动画周期开始时，随机决定本轮替换哪三帧为glitch效果帧
  updateBackgroundGlitchOverrides();

  // 窗口大小变化时，先把offset重新对齐到“当前选中的图片索引”，避免中心选项在缩放时漂移
  {
    const auto b = getLocalBounds();
    if (b != lastBoundsForSelection) {
      recenterColumnsToProcessorSelection(b);
      lastBoundsForSelection = b;
    }
  }

  // 第三列：从Processor读取真实glitch触发状态，用于UI闪光/抖动
  {
    const auto ev = processor.getColumn3GlitchEventCounter();
    if (ev != col3LastEventCounter) {
      col3LastEventCounter = ev;
      col3Flash = 1.0f;
    } else {
      // 视觉衰减：约0.25~0.35秒回落
      col3Flash *= 0.86f;
    }

    col3UiPhase01 = processor.getColumn3GlitchUiPhase01();
    col3UiAmount01 = processor.getColumn3GlitchUiAmount01();
  }

  // 背景动画图集在后台加载好之后再接入显示
  if (!backgroundSpriteSheet.isValid()) {
    juce::Image sheet;
    int fw = 0;
    int fh = 0;
    if (tryGetBackgroundAnimFromCache(sheet, fw, fh)) {
      backgroundSpriteSheet = std::move(sheet);
      backgroundFrameW = fw;
      backgroundFrameH = fh;
    }
  }

  // 一旦加载线程完成（成功/失败），尽快join，避免宿主卸载DLL阶段再join导致卡死。
  tryJoinBackgroundAnimWorkerIfFinished();

  // 随机按钮触发的“老虎机滚动”
  updateSlotSpin();

  for (int c = 0; c < columnCount; ++c) {
    if (!snapping[static_cast<size_t>(c)])
      continue;

    float& off = columnOffsets[static_cast<size_t>(c)];
    const float target = snapTargets[static_cast<size_t>(c)];

    const float diff = target - off;

    // 简单的ease-out：越接近越慢
    off += diff * 0.22f;

    if (std::abs(target - off) < 0.5f) {
      off = target;
      snapping[static_cast<size_t>(c)] = false;
    }
  }

  pushUiStateToProcessor();
  repaint();
}

}  // namespace grid