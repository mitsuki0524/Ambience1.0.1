#pragma once
#include <cmath>
#include <algorithm>
#include <array>

namespace FDNReverb {

    // ─────────────────────────────────────────────────────────────────────────────
    //  Saturator: ADAA 1次 + シンプル OS (1x/2x/4x)
    // ─────────────────────────────────────────────────────────────────────────────
    //   設計思想:
    //     ADAA がエイリアシング対策の主役 (低域〜中域で劇的に減衰)。
    //     OS は ADAA で残った高域エイリアスを補完的に除去する役割。
    //     重い FIR/IIR フィルタは不要、シンプルな複数点平均で十分。
    //
    //   OS 動作:
    //     1x: ADAA のみ
    //     2x: 1サンプル間で 2点 ADAA 実行 → 平均
    //     4x: 1サンプル間で 4点 ADAA 実行 → 平均
    //   
    //   この方式は「box car フィルタ」と呼ばれ、エイリアスを単純な合成で除去する。
    //   高域での減衰量は単純だが、ADAA との組み合わせで十分なエイリアス除去を達成。
    // ─────────────────────────────────────────────────────────────────────────────
    class Saturator {
    public:
        Saturator() = default;

        void reset() noexcept {
            prevInput = 0.0f;
            prevF = 1.0f;
            lastIn = 0.0f;
        }

        void setAmount(float amount) noexcept {
            amount = std::clamp(amount, 0.0f, 1.0f);
            currentAmount = amount;
            drive = 1.0f + amount * amount * 3.0f;
            wetMix = amount;
            dryMix = 1.0f - amount * 0.5f;
        }

        void setOversamplingFactor(int factor) noexcept {
            if (factor <= 1) osFactor = 1;
            else if (factor <= 2) osFactor = 2;
            else                  osFactor = 4;
        }

        inline float processSample(float input) noexcept {
            if (currentAmount < 1e-4f) {
                lastIn = input;
                return input;
            }

            float dryInput = input;
            float saturated = 0.0f;

            if (osFactor == 1) {
                // 1x: ADAA のみ
                saturated = adaaProcess(input * drive) / drive;
            }
            else if (osFactor == 2) {
                // 2x: 2点で ADAA 実行、平均
                float p0 = (lastIn + input) * 0.5f;   // 中間補間点
                float p1 = input;                     // 現在点

                float s0 = adaaProcess(p0 * drive) / drive;
                float s1 = adaaProcess(p1 * drive) / drive;

                saturated = (s0 + s1) * 0.5f;
            }
            else {
                // 4x: 4点で ADAA 実行、平均
                float p0 = lastIn * 0.25f + input * 0.75f;  // 25% 補間
                float p1 = lastIn * 0.5f + input * 0.5f;   // 50% 補間
                float p2 = lastIn * 0.75f + input * 0.25f;  // 75% 補間
                float p3 = input;                            // 現在点

                // ※ 補間順は時系列的に逆順だが、ADAA は前後関係を内部で持つ
                //   ので、現在サンプルが最後に計算されるよう配置する
                float s0 = adaaProcess(p2 * drive) / drive;  // 過去 75%
                float s1 = adaaProcess(p1 * drive) / drive;  // 過去 50%
                float s2 = adaaProcess(p0 * drive) / drive;  // 過去 25%
                float s3 = adaaProcess(p3 * drive) / drive;  // 現在

                saturated = (s0 + s1 + s2 + s3) * 0.25f;
            }

            lastIn = input;
            return dryInput * dryMix + saturated * wetMix;
        }

    private:
        inline float adaaProcess(float x) noexcept {
            float F_x = std::sqrt(1.0f + x * x);
            float dx = x - prevInput;
            float y;

            constexpr float kTolerance = 1e-5f;
            if (std::abs(dx) < kTolerance) {
                float xAvg = (x + prevInput) * 0.5f;
                y = xAvg / std::sqrt(1.0f + xAvg * xAvg);
            }
            else {
                y = (F_x - prevF) / dx;
            }

            prevInput = x;
            prevF = F_x;
            return y;
        }

        float prevInput{ 0.0f };
        float prevF{ 1.0f };
        float lastIn{ 0.0f };

        float currentAmount{ 0.0f };
        float drive{ 1.0f };
        float wetMix{ 0.0f };
        float dryMix{ 1.0f };
        int osFactor{ 1 };
    };

} // namespace FDNReverb