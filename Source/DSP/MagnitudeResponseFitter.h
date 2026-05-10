#pragma once
#include "DSPConstants.h"
#include "BiquadFilters.h"
#include "../AlgorithmPresets.h"
#include <array>

namespace FDNReverb {

    // ─────────────────────────────────────────────────────────────────────────────
    //  MagnitudeResponseFitter (Stage 1: Jot Orthogonalized 1st-order)
    // ─────────────────────────────────────────────────────────────────────────────
    //  10バンドのRT60ターゲットに対して、各遅延線の吸収フィルタを設計する。
    //
    //  設計理論:
    //    Jot–Chaigne (AES Preprint 3030, 1991) による直交化1次フィルタ:
    //
    //        H_i(z) = g'_i · (1 − p_i) / (1 − p_i · z^{-1})
    //
    //      g'_i  = 10^(-3·m_i / (fs·T60(0)))             // DC reverb time
    //      α     = T60(π/T) / T60(0)                     // ratio Nyquist/DC
    //      p_i   = (ln10/4) · log10(g'_i) · (1 − 1/α²)   // pole
    //
    //  Stage 1 では DC と Nyquist の 2 点だけで設計するシンプルな実装。
    //  Stage 2 で Välimäki–Liski 累積バイカッドGEQ に拡張予定。
    //
    //  重要:
    //    - dB スケールでなく T60 dB スケール (-60·m_i / (fs·T60)) でフィッティング
    //      これにより Schlecht–Habets (DAFx-17) で指摘された
    //      「2 kHz で T60 が無限大に発散する罠」を回避する。
    //    - 設計はオフライン（メッセージスレッド）で行い、
    //      結果を BiquadCoeffs としてオーディオスレッドに渡す。
    // ─────────────────────────────────────────────────────────────────────────────
    class MagnitudeResponseFitter {
    public:
        // 設計結果は ABSO_STAGES 個の Biquad に格納される
        // Stage 1 では:
        //   coeffs[0] = ミッドゲイン（1次の直交化フィルタを Biquad 形式で表現）
        //   coeffs[1] = 低域補正（Low Shelf, LF Absorption ユーザー操作分）
        //   coeffs[2] = 高域補正（High Shelf, HF Damping ユーザー操作分）
        struct DesignResult {
            std::array<BiquadCoeffs, ABSO_STAGES> coeffs;
            float dcGain{ 1.0f };           // DC ループゲイン (デバッグ・可視化用)
            float nyquistGain{ 1.0f };      // Nyquist ループゲイン
            float pole{ 0.0f };             // 直交化フィルタの極
        };

        // メインの設計関数
        // delaySamples: この遅延線のサンプル数 m_i
        // sampleRate:   現在のサンプルレート fs
        // rt60:         10バンドのRT60ターゲット (秒)
        // hfDamping:    ユーザー HF Damping (0..1, 高いほど高域減衰大)
        // lfAbsorption: ユーザー LF Absorption (0..1, 高いほど低域減衰大)
        static DesignResult design(
            int delaySamples,
            double sampleRate,
            const std::array<float, NUM_BANDS>& rt60,
            float hfDamping,
            float lfAbsorption);

    private:
        // T60(秒) と遅延サンプル数 m から 1サンプルあたりのループゲインを計算
        // g = 10^(-3·m / (fs·T60))
        static float t60ToLoopGain(float t60Seconds, int delaySamples, double sampleRate) noexcept;

        // 直交化1次フィルタの極を計算 (Jot 1991, CCRMA 定式)
        // p = (ln10/4) · log10(g_dc) · (1 − 1/α²)
        // α = T60(Nyq) / T60(DC)
        static float computeJotPole(float gDC, float alphaRatio) noexcept;

        // 1次の直交化フィルタを Biquad 形式 (b0, b1, b2, a1, a2) に変換
        // H(z) = g'·(1−p) / (1 − p·z^{-1})  →  Biquad の b2=a2=0
        static BiquadCoeffs orthogonalizedFirstOrderToBiquad(float gain, float pole) noexcept;

        // 設計から代表的な T60 を抽出するヘルパー
        // バンドインデックス: 0=31Hz, 1=63Hz, 2=125Hz, 3=250Hz, 4=500Hz,
        //                   5=1kHz, 6=2kHz, 7=4kHz, 8=8kHz, 9=16kHz
        static float getT60AtDC(const std::array<float, NUM_BANDS>& rt60) noexcept;
        static float getT60AtNyquist(const std::array<float, NUM_BANDS>& rt60, double sampleRate) noexcept;
    };

} // namespace FDNReverb