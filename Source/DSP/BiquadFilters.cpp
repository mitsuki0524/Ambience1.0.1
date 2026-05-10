#include "BiquadFilters.h"
#include "MagnitudeResponseFitter.h"
#include <JuceHeader.h>
#include <cmath>
#include <algorithm>

namespace FDNReverb {
    namespace FilterDesign {

        static float tanPi(float f, double fs) noexcept {
            return std::tan(juce::MathConstants<float>::pi * (float)(f / fs));
        }

        BiquadCoeffs lowShelf(float fcHz, float gainDB, double sampleRate) {
            float A = std::pow(10.f, gainDB / 40.f);
            float K = tanPi(fcHz, sampleRate);
            BiquadCoeffs c;
            if (gainDB >= 0.f) {
                float norm = 1.f / (1.f + K);
                c.b0 = (1.f + A * K) * norm;
                c.b1 = (A * K - 1.f) * norm;
                c.b2 = 0.f;
                c.a1 = (K - 1.f) * norm;
                c.a2 = 0.f;
            }
            else {
                c.b0 = (1.f + K / A) / (1.f + K);
                c.b1 = (K / A - 1.f) / (1.f + K);
                c.b2 = 0.f;
                c.a1 = (K - 1.f) / (1.f + K);
                c.a2 = 0.f;
            }
            return c;
        }

        BiquadCoeffs highShelf(float fcHz, float gainDB, double sampleRate) {
            float A = std::pow(10.f, gainDB / 40.f);
            float K = tanPi(fcHz, sampleRate);
            BiquadCoeffs c;
            if (gainDB >= 0.f) {
                float norm = 1.f / (1.f + K);
                c.b0 = (A + K) * norm;
                c.b1 = (K - A) * norm;
                c.b2 = 0.f;
                c.a1 = (K - 1.f) * norm;
                c.a2 = 0.f;
            }
            else {
                float norm = 1.f / (1.f + K);
                c.b0 = (1.f + A * K) * norm;
                c.b1 = (A * K - 1.f) * norm;
                c.b2 = 0.f;
                c.a1 = (K - 1.f) * norm;
                c.a2 = 0.f;
            }
            return c;
        }

        BiquadCoeffs peak(float fcHz, float gainDB, float Q, double sampleRate) {
            float A = std::pow(10.f, gainDB / 40.f);
            float w0 = 2.f * juce::MathConstants<float>::pi * fcHz / (float)sampleRate;
            float alpha = std::sin(w0) / (2.f * Q);
            float cos0 = std::cos(w0);
            BiquadCoeffs c;
            c.a1 = 2.f * cos0 / (1.f + alpha / A);
            c.a2 = (1.f - alpha / A) / (1.f + alpha / A);
            c.b0 = (1.f + alpha * A) / (1.f + alpha / A);
            c.b1 = -2.f * cos0 / (1.f + alpha / A);
            c.b2 = (1.f - alpha * A) / (1.f + alpha / A);
            return c;
        }

        BiquadCoeffs highPass1st(float fcHz, double sampleRate) {
            float K = tanPi(fcHz, sampleRate);
            float n = 1.f + K;
            BiquadCoeffs c;
            c.b0 = 1.f / n; c.b1 = -1.f / n; c.b2 = 0.f;
            c.a1 = (K - 1.f) / n; c.a2 = 0.f;
            return c;
        }

        // ─────────────────────────────────────────────────────────────────────────
        //  designAbsorption: MagnitudeResponseFitter へのラッパー
        // ─────────────────────────────────────────────────────────────────────────
        //  既存コード (UniversalEngine 等) との互換性を保つため、関数シグネチャは
        //  維持したまま、内部実装を Stage 1 の MRF に差し替えた。
        //
        //  旧実装: ミッドゲイン + Low/High シェルフのカスケード
        //  新実装: Jot 直交化1次フィルタ + ユーザー LF/HF 補正
        // ─────────────────────────────────────────────────────────────────────────
        std::array<BiquadCoeffs, ABSO_STAGES> designAbsorption(
            int delaySamples, double sampleRate,
            const std::array<float, NUM_BANDS>& rt60,
            float hfDamping, float lfAbsorption)
        {
            // MagnitudeResponseFitter に処理を委譲
            auto result = MagnitudeResponseFitter::design(
                delaySamples, sampleRate, rt60, hfDamping, lfAbsorption);
            return result.coeffs;
        }

    } // namespace FilterDesign
} // namespace FDNReverb