#pragma once
#include <JuceHeader.h>
#include "../PluginProcessor.h"
#include "AmbienceUI.h"

// ─────────────────────────────────────────────────────────────────────────────
//  DecayCurveViz
// ─────────────────────────────────────────────────────────────────────────────
//  時間軸での減衰曲線を可視化するコンポーネント。
//  
//  表示要素:
//    - 背景グリッド (時間軸: 0ms ~ RT60×1.5、振幅軸: 0dB ~ -60dB)
//    - ER タップ: 青系の縦線、グラデーション付き塗りつぶし
//    - Late Reverb 減衰: オレンジ系の指数曲線、2Dグラデーション塗りつぶし
//
//  色の設計:
//    - ER:   青系  (上=濃い → 下=透明、左=濃い → 右=薄い)
//    - Late: オレンジ系 (同上)
//
//  時間軸:
//    - 自動スケーリング: 現在の RT60 中域値 × 1.5倍まで表示
// ─────────────────────────────────────────────────────────────────────────────
class DecayCurveViz : public juce::Component, private juce::Timer {
public:
    DecayCurveViz();
    ~DecayCurveViz() override;

    void setProcessor(FDNReverbAudioProcessor* p) noexcept { processor = p; }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    FDNReverbAudioProcessor* processor{ nullptr };

    // 描画用のキャッシュ値（タイマーで定期更新）
    float cachedRT60Mid{ 1.0f };
    int cachedERTapCount{ 0 };
    static constexpr int MAX_DISPLAY_TAPS = 12;
    std::array<float, MAX_DISPLAY_TAPS> cachedERDelayMs;
    std::array<float, MAX_DISPLAY_TAPS> cachedERGains;
    bool cachedERBypassed{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DecayCurveViz)
};