#include "MagnitudeResponseFitter.h"
#include <JuceHeader.h>
#include <cmath>
#include <algorithm>

namespace FDNReverb {

    // ─────────────────────────────────────────────────────────────────────────────
    //  内部ヘルパー
    // ─────────────────────────────────────────────────────────────────────────────

    float MagnitudeResponseFitter::t60ToLoopGain(float t60Seconds, int delaySamples, double sampleRate) noexcept {
        // g = 10^(-3·m / (fs·T60))
        // -60dB after T60 seconds → per-sample loss
        float t60Safe = std::max(0.01f, t60Seconds);
        float exponent = -3.0f * static_cast<float>(delaySamples) / (static_cast<float>(sampleRate) * t60Safe);
        return std::pow(10.0f, exponent);
    }

    float MagnitudeResponseFitter::computeJotPole(float gDC, float alphaRatio) noexcept {
        // p = (ln10/4) · log10(g_dc) · (1 − 1/α²)
        // α = T60(Nyq) / T60(DC)
        // α < 1 のとき高域減衰、α > 1 のとき高域強調

        // 安全範囲にクランプ
        float alphaSafe = juce::jlimit(0.05f, 20.0f, alphaRatio);
        float gDCSafe = juce::jlimit(1e-6f, 0.99999f, gDC);

        // ln(10)/4 ≈ 0.5756
        constexpr float kLn10Over4 = 0.5756462732485f;

        float log10g = std::log10(gDCSafe);
        float alphaSqInv = 1.0f / (alphaSafe * alphaSafe);
        float pole = kLn10Over4 * log10g * (1.0f - alphaSqInv);

        // 極の安定性確保 (|p| < 1)
        return juce::jlimit(-0.98f, 0.98f, pole);
    }

    BiquadCoeffs MagnitudeResponseFitter::orthogonalizedFirstOrderToBiquad(float gain, float pole) noexcept {
        // H(z) = gain · (1 − pole) / (1 − pole · z^{-1})
        //
        // Direct Form II Transposed の Biquad 形式に変換:
        //   y[n] = b0·x[n] + s1
        //   s1   = b1·x[n] - a1·y[n] + s2
        //   s2   = b2·x[n] - a2·y[n]
        //
        // 1次フィルタなので b2 = a2 = 0
        // b0 = gain · (1 − pole)
        // b1 = 0
        // a1 = -pole  (注: BiquadCoeffs の a1 は Direct Form II Transposed の係数
        //              tick() 内では「-c.a1·y」として使われるため、a1 = -pole とする)

        BiquadCoeffs c;
        c.b0 = gain * (1.0f - pole);
        c.b1 = 0.0f;
        c.b2 = 0.0f;
        c.a1 = -pole;  // tick内で s1 += c.b1*x - c.a1*y → -(-pole)*y = pole*y となり正しい
        c.a2 = 0.0f;
        return c;
    }

    float MagnitudeResponseFitter::getT60AtDC(const std::array<float, NUM_BANDS>& rt60) noexcept {
        // 31Hz と 63Hz の平均を DC 近傍の T60 とする
        return (rt60[0] + rt60[1]) * 0.5f;
    }

    float MagnitudeResponseFitter::getT60AtNyquist(const std::array<float, NUM_BANDS>& rt60, double sampleRate) noexcept {
        // サンプルレートに応じて Nyquist 近傍のバンドを選ぶ
        // 48kHz 以下: 16kHz バンド (index 9)
        // 96kHz 以上: 16kHz バンドだけだとカバー不十分なので 8kHz と 16kHz の平均
        // 192kHz: 同上 (測定データが 16kHz までしかないため外挿せず最高帯域を使う)
        if (sampleRate <= 50000.0) {
            return rt60[9]; // 16 kHz
        }
        else {
            // 高サンプルレート時は最高2バンドの平均で安定化
            return (rt60[8] + rt60[9]) * 0.5f;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  メイン設計関数
    // ─────────────────────────────────────────────────────────────────────────────

    MagnitudeResponseFitter::DesignResult MagnitudeResponseFitter::design(
        int delaySamples,
        double sampleRate,
        const std::array<float, NUM_BANDS>& rt60,
        float hfDamping,
        float lfAbsorption)
    {
        DesignResult result;

        // ── Step 1: DC と Nyquist の代表 T60 を取得 ──
        float t60DC = std::max(0.01f, getT60AtDC(rt60));
        float t60Nyq = std::max(0.01f, getT60AtNyquist(rt60, sampleRate));

        // ── Step 2: ループゲインに変換 ──
        float gDC = t60ToLoopGain(t60DC, delaySamples, sampleRate);
        float gNyq = t60ToLoopGain(t60Nyq, delaySamples, sampleRate);

        // α = T60(Nyq) / T60(DC)
        float alpha = t60Nyq / t60DC;

        // ── Step 3: Jot 直交化1次フィルタの極を計算 ──
        float pole = computeJotPole(gDC, alpha);

        // ── Step 4: 第1段に直交化1次フィルタを配置 ──
        // ゲインは gDC を使う（DC でのループ損失を保証）
        result.coeffs[0] = orthogonalizedFirstOrderToBiquad(gDC, pole);

        // ── Step 5: ユーザー HF Damping / LF Absorption を補正フィルタとして追加 ──
        // 既存の設計と互換性を保つため、Stage 1 ではユーザー操作分は別段で処理する
        float m = static_cast<float>(delaySamples);
        float fs = static_cast<float>(sampleRate);

        // 中域基準で 500Hz バンドの T60 を使う
        float t60Mid = std::max(0.01f, rt60[4]);
        float gMid = t60ToLoopGain(t60Mid, delaySamples, sampleRate);

        // LF 補正: ユーザーが LF Absorption を上げた分だけ低域を減衰
        // lfAbsorption=0 で補正なし、=1 で -3dB の追加減衰
        float lfShelfDB = -lfAbsorption * 3.0f;
        result.coeffs[1] = FilterDesign::lowShelf(150.0f, lfShelfDB, sampleRate);

        // HF 補正: ユーザーが HF Damping を上げた分だけ高域を減衰
        // hfDamping=0 で補正なし、=1 で -6dB の追加減衰
        float hfShelfDB = -hfDamping * 6.0f;
        result.coeffs[2] = FilterDesign::highShelf(4000.0f, hfShelfDB, sampleRate);

        // ── デバッグ用情報の格納 ──
        result.dcGain = gDC;
        result.nyquistGain = gNyq;
        result.pole = pole;

        return result;
    }

} // namespace FDNReverb