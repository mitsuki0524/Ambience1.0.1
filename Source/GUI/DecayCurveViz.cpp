#include "DecayCurveViz.h"

DecayCurveViz::DecayCurveViz() {
    cachedERDelayMs.fill(0.0f);
    cachedERGains.fill(0.0f);
    startTimerHz(15);  // 15Hz で更新（軽め）
}

DecayCurveViz::~DecayCurveViz() {
    stopTimer();
}

void DecayCurveViz::timerCallback() {
    if (processor == nullptr) return;

    const auto& engine = processor->getEngine();

    // RT60 中域値を取得
    auto rt60 = engine.getEffectiveRT60();
    cachedRT60Mid = std::max(0.1f, rt60[4]);  // 500Hz バンド

    // ER タップ情報を取得
    cachedERBypassed = engine.isERBypassed();
    cachedERTapCount = engine.getERTapCount();

    if (cachedERTapCount > MAX_DISPLAY_TAPS) cachedERTapCount = MAX_DISPLAY_TAPS;

    double sr = engine.getSampleRate();
    if (sr < 1.0) sr = 48000.0;

    for (int i = 0; i < cachedERTapCount; ++i) {
        float delaySamples = engine.getERTapDelaySamples(i);
        cachedERDelayMs[i] = delaySamples / static_cast<float>(sr) * 1000.0f;
        cachedERGains[i] = engine.getERTapGain(i);
    }

    repaint();
}

void DecayCurveViz::resized() {
    // 何もしない（paint で動的にサイズを取得）
}

void DecayCurveViz::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    if (bounds.getWidth() < 10.0f || bounds.getHeight() < 10.0f) return;

    // 背景クリア
    g.fillAll(AmbienceColors::Background);

    // ── 描画パラメータ ──
    const float topMargin = 10.0f;
    const float bottomMargin = 18.0f;
    const float leftMargin = 30.0f;
    const float rightMargin = 8.0f;

    const float plotX = bounds.getX() + leftMargin;
    const float plotY = bounds.getY() + topMargin;
    const float plotW = bounds.getWidth() - leftMargin - rightMargin;
    const float plotH = bounds.getHeight() - topMargin - bottomMargin;

    // 時間軸の最大値（RT60 × 1.5、最低 0.5秒、最高 8秒）
    const float maxTimeSec = juce::jlimit(0.5f, 8.0f, cachedRT60Mid * 1.5f);

    // dB 範囲 (0 ~ -60 dB)
    const float minDB = -60.0f;
    const float maxDB = 0.0f;

    // 座標変換ヘルパー
    auto timeToX = [&](float timeSec) {
        return plotX + (timeSec / maxTimeSec) * plotW;
        };
    auto dbToY = [&](float db) {
        float normalized = (db - minDB) / (maxDB - minDB);  // 0 ~ 1
        return plotY + (1.0f - normalized) * plotH;
        };

    // ── グリッド描画 ──
    g.setColour(AmbienceColors::Separator.withAlpha(0.3f));

    // 水平線 (dB)
    for (float db = 0.0f; db >= -60.0f; db -= 20.0f) {
        float y = dbToY(db);
        g.drawHorizontalLine((int)y, plotX, plotX + plotW);
    }

    // 垂直線 (時間) - 動的に間隔を決定
    float timeStep;
    if (maxTimeSec <= 1.0f)      timeStep = 0.2f;
    else if (maxTimeSec <= 2.0f) timeStep = 0.5f;
    else if (maxTimeSec <= 4.0f) timeStep = 1.0f;
    else                         timeStep = 2.0f;

    for (float t = 0.0f; t <= maxTimeSec; t += timeStep) {
        float x = timeToX(t);
        g.drawVerticalLine((int)x, plotY, plotY + plotH);
    }

    // ── 軸ラベル ──
    g.setColour(AmbienceColors::TextSecondary.withAlpha(0.6f));
    g.setFont(juce::Font(juce::FontOptions(8.5f)));

    // dB ラベル (左)
    for (float db = 0.0f; db >= -60.0f; db -= 20.0f) {
        float y = dbToY(db);
        g.drawText(juce::String((int)db) + "dB",
            (int)(plotX - leftMargin + 2),
            (int)(y - 6),
            (int)(leftMargin - 4), 12,
            juce::Justification::centredRight);
    }

    // 時間ラベル (下)
    for (float t = 0.0f; t <= maxTimeSec; t += timeStep) {
        float x = timeToX(t);
        juce::String label;
        if (t < 1.0f) label = juce::String((int)(t * 1000)) + "ms";
        else          label = juce::String(t, 1) + "s";
        g.drawText(label,
            (int)(x - 25),
            (int)(plotY + plotH + 2),
            50, 14,
            juce::Justification::centred);
    }

    // ─────────────────────────────────────────────────────────────
    //  Late Reverb 減衰カーブの描画 (オレンジ系、2Dグラデーション)
    // ─────────────────────────────────────────────────────────────
    {
        // 指数減衰: y(t) = -60 × t / RT60 (dB)
        juce::Path latePath;
        latePath.startNewSubPath(plotX, dbToY(maxDB));  // 左上から

        const int numPoints = 64;
        for (int i = 0; i <= numPoints; ++i) {
            float t = (i / static_cast<float>(numPoints)) * maxTimeSec;
            float db = -60.0f * t / cachedRT60Mid;
            db = std::max(minDB, db);

            float x = timeToX(t);
            float y = dbToY(db);
            latePath.lineTo(x, y);
        }
        // 下端を閉じる
        latePath.lineTo(timeToX(maxTimeSec), dbToY(minDB));
        latePath.lineTo(plotX, dbToY(minDB));
        latePath.closeSubPath();

        // 2D グラデーション (左上=濃い → 右下=透明)
        juce::Colour orangeColor = AmbienceColors::Accent;
        juce::ColourGradient lateGrad(
            orangeColor.withAlpha(0.65f),
            plotX, plotY,                          // 左上: 濃い
            orangeColor.withAlpha(0.0f),
            plotX + plotW, plotY + plotH,          // 右下: 透明
            false
        );

        g.setGradientFill(lateGrad);
        g.fillPath(latePath);

        // カーブの輪郭線（強調）
        juce::Path lateOutline;
        lateOutline.startNewSubPath(plotX, dbToY(maxDB));
        for (int i = 0; i <= numPoints; ++i) {
            float t = (i / static_cast<float>(numPoints)) * maxTimeSec;
            float db = -60.0f * t / cachedRT60Mid;
            db = std::max(minDB, db);
            lateOutline.lineTo(timeToX(t), dbToY(db));
        }
        g.setColour(orangeColor.withAlpha(0.7f));
        g.strokePath(lateOutline, juce::PathStrokeType(1.5f));
    }

    // ─────────────────────────────────────────────────────────────
    //  ER タップの描画 (青系、2Dグラデーション)
    // ─────────────────────────────────────────────────────────────
    if (!cachedERBypassed && cachedERTapCount > 0) {
        // 青色（業界標準: 鮮やかな青）
        juce::Colour blueColor = juce::Colour::fromRGB(80, 160, 230);

        // 各タップを縦の塗りつぶし矩形として描画
        for (int t = 0; t < cachedERTapCount; ++t) {
            float delayMs = cachedERDelayMs[t];
            float gain = cachedERGains[t];

            float timeSec = delayMs * 0.001f;
            if (timeSec > maxTimeSec) continue;  // 範囲外

            // ゲインを dB に変換
            float gainDB = (gain > 1e-6f) ? juce::Decibels::gainToDecibels(gain) : minDB;
            gainDB = juce::jlimit(minDB, maxDB, gainDB);

            float x = timeToX(timeSec);
            float yTop = dbToY(gainDB);
            float yBottom = dbToY(minDB);

            // タップの幅 (細い縦線、両側に薄い裾野)
            float lineW = 2.0f;

            // 縦方向グラデーション
            juce::ColourGradient tapGrad(
                blueColor.withAlpha(0.85f),
                x, yTop,                           // 上端: 濃い
                blueColor.withAlpha(0.0f),
                x, yBottom,                        // 下端: 透明
                false
            );

            g.setGradientFill(tapGrad);
            g.fillRect(x - lineW * 0.5f, yTop, lineW, yBottom - yTop);

            // タップ位置にマーカー
            g.setColour(blueColor.withAlpha(0.95f));
            g.fillEllipse(x - 2.0f, yTop - 2.0f, 4.0f, 4.0f);
        }

        // ER 全体を薄くつなぐエンベロープ（任意の演出）
        // 各タップのピーク位置を結ぶライン
        if (cachedERTapCount >= 2) {
            juce::Path erEnvelope;
            bool started = false;

            for (int t = 0; t < cachedERTapCount; ++t) {
                float delayMs = cachedERDelayMs[t];
                float gain = cachedERGains[t];
                float timeSec = delayMs * 0.001f;
                if (timeSec > maxTimeSec) continue;

                float gainDB = (gain > 1e-6f) ? juce::Decibels::gainToDecibels(gain) : minDB;
                gainDB = juce::jlimit(minDB, maxDB, gainDB);

                float x = timeToX(timeSec);
                float y = dbToY(gainDB);

                if (!started) {
                    erEnvelope.startNewSubPath(x, y);
                    started = true;
                }
                else {
                    erEnvelope.lineTo(x, y);
                }
            }

            g.setColour(blueColor.withAlpha(0.4f));
            g.strokePath(erEnvelope, juce::PathStrokeType(1.0f));
        }
    }



    // ── 凡例 (左上、小さく) ──
    g.setFont(juce::Font(juce::FontOptions("Helvetica Neue", 8.0f, juce::Font::bold)));

    g.setColour(juce::Colour::fromRGB(80, 160, 230));
    g.drawText("ER", (int)(plotX + 4), (int)(plotY + 2), 30, 12, juce::Justification::centredLeft);

    g.setColour(AmbienceColors::Accent);
    g.drawText("LATE", (int)(plotX + 30), (int)(plotY + 2), 40, 12, juce::Justification::centredLeft);
}