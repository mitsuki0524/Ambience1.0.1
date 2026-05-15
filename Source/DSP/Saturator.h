#pragma once

#include <cmath>
#include <algorithm>

namespace FDNReverb {

    // ─────────────────────────────────────────────────────────────────────────────
    //  SaturationMode: ProMode で選択可能な 4 種のサチュレーション特性
    // ─────────────────────────────────────────────────────────────────────────────
    enum class SaturationMode {
        Warm = 0,   // Vicanek x/√(1+x²) + ADAA。FET 系。均一な奇数次倍音。
        Tape = 1,   // Padé x(27+x²)/(27+9x²)。ADAA なし最軽量。テープ感。
        Tube = 2,   // 非対称 ADAA。偶数次倍音支配。真空管プリアンプ感。
        Hard = 3    // ハードクリッピング + ADAA。鋭く明るい高次倍音。
    };

    // ─────────────────────────────────────────────────────────────────────────────
    //  Saturator: Wet 出力に適用するキャラクターサチュレーター (Layer 2)
    // ─────────────────────────────────────────────────────────────────────────────
    //   v1.2 変更点 (ドライブカーブの全面再設計):
    //
    //   旧実装の問題:
    //     drive = 1 + amount² × 3 → amount=0.3 で drive=1.27、すでに可聴の歪み
    //     wetMix = amount → 線形比例で wet が強すぎる
    //     algorithmMultiplier が 0.6〜1.2 と広く、同じノブ位置でプリセットにより
    //     大きく挙動が変わる
    //
    //   新実装の設計目標:
    //     - amount = 0   : 完全バイパス (マイクロサチュレーション Layer 1 のみ)
    //     - amount = 0〜0.8 : 非常に緩やかな変化 (最初の 80% でソフトな味付け)
    //     - amount = 0.8〜1.0 : 明確に効くが音が割れない (OutputLimiter が最終保護)
    //
    //   ドライブカーブの変更:
    //     旧: drive = 1 + amount²  × 3.0  (最大 4.0 倍)
    //     新: drive = 1 + amount³  × 1.0  (最大 2.0 倍)
    //     → 3 乗カーブで序盤がさらに緩やか、最大値を半減
    //
    //   Wet/Dry 比の変更:
    //     旧: wetMix = amount、dryMix = 1 - amount * 0.5
    //     新: wetMix = amount² × 0.7、dryMix = 1 - amount × 0.25
    //     → wet が 2 乗カーブで序盤はほぼゼロ、dry はほとんど落ちない
    //     → amount=1 でも dry=0.75 が残り、原音の芯を保つ
    //
    //   アルゴリズム別倍率の圧縮:
    //     旧: 0.6〜1.2 (差が 2 倍)
    //     新: 0.90〜1.05 (差が 15%、プリセット切り替えで急変しない)
    // ─────────────────────────────────────────────────────────────────────────────
    class Saturator {
    public:
        Saturator() = default;

        // ─── 状態リセット ───
        void reset() noexcept {
            prevInput = 0.0f;
            switch (currentMode) {
            case SaturationMode::Warm: prevF = 1.0f; break;
            case SaturationMode::Tape: prevF = 0.0f; break;
            case SaturationMode::Tube: prevF = 1.0f; break;
            case SaturationMode::Hard: prevF = 0.0f; break;
            }
        }

        // ─── モード設定 ───
        void setMode(SaturationMode mode) noexcept {
            if (mode != currentMode) {
                currentMode = mode;
                reset();
            }
        }

        void setMode(int modeIndex) noexcept {
            setMode(static_cast<SaturationMode>(std::clamp(modeIndex, 0, 3)));
        }

        // ─────────────────────────────────────────────────────────────────────────
        //  ドライブ量設定 (v1.2 全面再設計)
        // ─────────────────────────────────────────────────────────────────────────
        //   amount 0.0 → 1.0 の各値での挙動:
        //
        //   amount | drive  | wetMix | dryMix | 効果
        //   ───────|────────|────────|────────|─────────────────────────────
        //   0.00   | 1.000  | 0.000  | 1.000  | 完全バイパス
        //   0.10   | 1.001  | 0.007  | 0.975  | ほぼ感知不能
        //   0.30   | 1.027  | 0.063  | 0.925  | 非常に微細な倍音感
        //   0.50   | 1.125  | 0.175  | 0.875  | 穏やかな温かみ
        //   0.70   | 1.343  | 0.343  | 0.825  | 明確なキャラクタ
        //   0.80   | 1.512  | 0.448  | 0.800  | 音楽的な歪み感
        //   0.90   | 1.729  | 0.567  | 0.775  | 強いキャラクタ
        //   1.00   | 2.000  | 0.700  | 0.750  | 最大 (割れない範囲の上限)
        // ─────────────────────────────────────────────────────────────────────────
        void setAmount(float amount) noexcept {
            amount = std::clamp(amount, 0.0f, 1.0f);
            currentAmount = amount;

            // ─ ドライブカーブ: 3 乗で序盤が極めて緩やか、最大 2.0 倍 ─
            // 旧の amount² × 3 (最大 4.0) から amount³ × 1.0 (最大 2.0) に変更
            // これにより、Vicanek/Padé が強い歪みを出し始める前に「味付け」が完了する
            drive = 1.0f + amount * amount * amount * 1.0f;

            // ─ Wet: 2 乗カーブで序盤はほぼゼロ ─
            // amount=0.3 で wetMix=0.063 (6%)、amount=0.8 で 0.448 (45%)
            wetMix = amount * amount * 0.7f;

            // ─ Dry: ほとんど落ちない (1.0 → 0.75 の範囲) ─
            // amount=1.0 でも dry=0.75 が残り、原音の芯が保たれる
            dryMix = 1.0f - amount * 0.25f;
        }

        // ─── サンプル単位の処理 ───
        inline float processSample(float input) noexcept {
            if (currentAmount < 1e-4f) return input;

            const float dryInput = input;
            const float driven = input * drive;
            float saturated = 0.0f;

            switch (currentMode) {
            case SaturationMode::Warm: saturated = processWarm(driven); break;
            case SaturationMode::Tape: saturated = processTape(driven); break;
            case SaturationMode::Tube: saturated = processTube(driven); break;
            case SaturationMode::Hard: saturated = processHard(driven); break;
            }

            saturated /= drive;
            return dryInput * dryMix + saturated * wetMix;
        }

    private:
        // ─────────────────────────────────────────────────────────────────────────
        //  Warm: Vicanek x/√(1+x²) + ADAA 1次
        //   原始関数: F(x) = √(1+x²)
        // ─────────────────────────────────────────────────────────────────────────
        inline float processWarm(float x) noexcept {
            const float F_x = std::sqrt(1.0f + x * x);
            const float dx = x - prevInput;
            float y;

            constexpr float kTol = 1e-5f;
            if (std::abs(dx) < kTol) {
                const float xAvg = (x + prevInput) * 0.5f;
                y = xAvg / std::sqrt(1.0f + xAvg * xAvg);
            }
            else {
                y = (F_x - prevF) / dx;
            }

            prevInput = x;
            prevF = F_x;
            return y;
        }

        // ─────────────────────────────────────────────────────────────────────────
        //  Tape: Padé x(27+x²)/(27+9x²) (ADAA なし、最軽量)
        //   x=±3 で正確に ±1.0 に到達 (Padé の設計点)
        // ─────────────────────────────────────────────────────────────────────────
        inline float processTape(float x) noexcept {
            if (x > 3.0f) { prevInput = x; return  1.0f; }
            if (x < -3.0f) { prevInput = x; return -1.0f; }

            const float xsq = x * x;
            prevInput = x;
            return x * (27.0f + xsq) / (27.0f + 9.0f * xsq);
        }

        // ─────────────────────────────────────────────────────────────────────────
        //  Tube: 非対称 ADAA (偶数次倍音支配)
        //   正側: f(x) = x/√(1+x²)        (ソフト)
        //   負側: f(x) = x/√(1+(1.5x)²)   (ハード)
        //   C¹ 連続性: F_neg(0) = F_pos(0) = 1 となるよう定数シフトを設計
        // ─────────────────────────────────────────────────────────────────────────
        inline float processTube(float x) noexcept {
            constexpr float kNeg = 1.5f;
            constexpr float kNeg2 = kNeg * kNeg;
            constexpr float invKneg2 = 1.0f / kNeg2;
            constexpr float fShift = 1.0f - invKneg2;

            float F_x;
            if (x >= 0.0f) {
                F_x = std::sqrt(1.0f + x * x);
            }
            else {
                const float kx = kNeg * x;
                F_x = invKneg2 * std::sqrt(1.0f + kx * kx) + fShift;
            }

            const float dx = x - prevInput;
            const bool  signChanged = (x >= 0.0f) != (prevInput >= 0.0f);
            float y;

            constexpr float kTol = 1e-5f;
            if (std::abs(dx) < kTol || signChanged) {
                if (x >= 0.0f) {
                    y = x / std::sqrt(1.0f + x * x);
                }
                else {
                    const float kx = kNeg * x;
                    y = x / std::sqrt(1.0f + kx * kx);
                }
            }
            else {
                y = (F_x - prevF) / dx;
            }

            prevInput = x;
            prevF = F_x;
            return y;
        }

        // ─────────────────────────────────────────────────────────────────────────
        //  Hard: ハードクリッピング + ADAA
        //   F(x) = x²/2 (|x|≤1), x-1/2 (x>1), -x-1/2 (x<-1)
        //   C¹ 連続: F(±1) = 1/2, F'(±1) = ±1
        // ─────────────────────────────────────────────────────────────────────────
        inline float processHard(float x) noexcept {
            float F_x;
            if (x > 1.0f) F_x = x - 0.5f;
            else if (x < -1.0f) F_x = -x - 0.5f;
            else                F_x = x * x * 0.5f;

            const float dx = x - prevInput;
            float y;

            constexpr float kTol = 1e-5f;
            if (std::abs(dx) < kTol) {
                y = std::clamp(x, -1.0f, 1.0f);
            }
            else {
                y = (F_x - prevF) / dx;
            }

            prevInput = x;
            prevF = F_x;
            return y;
        }

        // ─── ADAA 履歴 ───
        float prevInput{ 0.0f };
        float prevF{ 1.0f };   // Warm の F(0) = 1 に合わせて初期化

        // ─── 設定値 ───
        SaturationMode currentMode{ SaturationMode::Warm };
        float          currentAmount{ 0.0f };
        float          drive{ 1.0f };
        float          wetMix{ 0.0f };
        float          dryMix{ 1.0f };
    };

} // namespace FDNReverb