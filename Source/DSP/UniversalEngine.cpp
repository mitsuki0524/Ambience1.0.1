#include "UniversalEngine.h"

namespace FDNReverb {

    // ─────────────────────────────────────────────────────────────────────────────
    //  コンストラクタ
    // ─────────────────────────────────────────────────────────────────────────────
    UniversalEngine::UniversalEngine() {
        fbVec.fill(0.0f);

        // ─────────────────────────────────────────────────────────────────────────
        //  BandlimitedNoiseLFO の初期化 (黄金比 Weyl 列によるレート係数)
        // ─────────────────────────────────────────────────────────────────────────
        //   黄金比 φ = (1+√5)/2 ≈ 1.618... は「最も無理数らしい数」として知られ、
        //   任意の有理数分の 1 でも近似しにくい性質を持つ。
        //
        //   Weyl 列: a[i] = frac(i * φ) は [0, 1) 上で「超一様分布」を示す。
        //   これを rateMultiplier [0.80, 1.20] にマッピングすることで、
        //   16 チャンネルのレートが互いに絶対に同期しない非周期的配置を保証する。
        //
        //   例:
        //     ch 0: rateMultiplier = 1.000  (φ の倍数の小数部が 0.618... → 1.047)
        //     ch 1: rateMultiplier = 1.047  → ch 0 と 4.7% 違う
        //     ch 7: rateMultiplier = 0.872  → ch 0 と 12.8% 違う
        //     etc.
        // ─────────────────────────────────────────────────────────────────────────
        constexpr float phi = 1.6180339887f;  // 黄金比 φ

        for (int i = 0; i < FDN_ORDER; ++i) {
            // 各チャンネルに固有の PRNG seed (0 は禁止: XOR shift が止まる)
            lfos[i].state = 12345u + static_cast<uint32_t>(i) * 9876u;
            lfos[i].smoothed = 0.0f;

            // Weyl 列: frac(i * φ) を [0.80, 1.20] にマッピング
            const float angle = static_cast<float>(i) * phi;
            const float frac = angle - std::floor(angle);       // 小数部 [0, 1)
            lfos[i].rateMultiplier = 0.80f + frac * 0.40f;       // [0.80, 1.20]
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  prepare()
    // ─────────────────────────────────────────────────────────────────────────────
    void UniversalEngine::prepare(double sampleRate, int /*maxBlockSize*/) {
        fs = sampleRate;

#if AMBIENCE_USE_STAGE2_ABSORPTION
        MagnitudeResponseFitter::precomputeInteractionMatrix(sampleRate);
#endif

        // ── メモリプール割り当て ──
        auto getPow2 = [](size_t s) -> size_t {
            size_t p = 1;
            while (p < s) p *= 2;
            return p;
            };

        size_t totalMemoryNeeded =
            getPow2(static_cast<size_t>(fs * 1.0))
            + getPow2(static_cast<size_t>(fs * 0.05)) * 4
            + getPow2(static_cast<size_t>(fs * 0.5)) * FDN_ORDER
            + getPow2(static_cast<size_t>(fs * 0.1)) * FDN_ORDER;

        memoryPool.allocate(totalMemoryNeeded);

        // ── 遅延ラインの割り当て ──
        int mask = 0;
        float* ptr = nullptr;

        ptr = memoryPool.requestMemory(static_cast<size_t>(fs * 1.0), mask);
        erDelay.init(ptr, mask);

        for (int i = 0; i < 4; ++i) {
            ptr = memoryPool.requestMemory(static_cast<size_t>(fs * 0.05), mask);
            inputDiffusers[i].init(ptr, mask);
        }

        for (int i = 0; i < FDN_ORDER; ++i) {
            ptr = memoryPool.requestMemory(static_cast<size_t>(fs * 0.5), mask);
            fdnDelays[i].init(ptr, mask);

            ptr = memoryPool.requestMemory(static_cast<size_t>(fs * 0.1), mask);
            nestedAllpassDelays[i].init(ptr, mask);
        }

        // ── AcousticMetrics 初期化 ──
        acousticMetrics.prepare(sampleRate, 2000.0f);

        // ── ER パターン初期化 ──
        currentERTapCount = 0;
        currentERDelaySamples.fill(0.0f);
        currentERGains.fill(0.0f);

        // ── Phase 3-1: OutputLimiter 初期化 ──
        outputLimiter.prepare(sampleRate);

        // ── Phase 3-1: Ducking 係数初期化 (デフォルト値) ──
        duckingAttackCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(fs) * 0.010f));
        duckingReleaseCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(fs) * 0.200f));
        duckingEnvelope = 0.0f;

        reset();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  reset()
    // ─────────────────────────────────────────────────────────────────────────────
    void UniversalEngine::reset() {
        memoryPool.clear();
        fbVec.fill(0.0f);

#if AMBIENCE_USE_STAGE2_ABSORPTION
        for (auto& lineFilters : absorptionFiltersS2) {
            for (auto& f : lineFilters) f.reset();
        }
#else
        for (auto& f : absorptionFilters) f.reset();
#endif

        acousticMetrics.reset();
        saturatorL.reset();
        saturatorR.reset();
        outputLimiter.reset();
        duckingEnvelope = 0.0f;

        // LFO の smoothed をリセット (クリック防止)
        for (auto& lfo : lfos) lfo.smoothed = 0.0f;
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  setParams()
    // ─────────────────────────────────────────────────────────────────────────────
    void UniversalEngine::setParams(const DSPParams& p) {
        activeParams = p;

        switch (p.algorithmIndex) {
        case 0: case 1: currentTopology = ReverbTopology::Room;     break;
        case 2: case 3: currentTopology = ReverbTopology::Hall;     break;
        case 4:         currentTopology = ReverbTopology::Plate;    break;
        case 5:         currentTopology = ReverbTopology::Spring;   break;
        case 6:         currentTopology = ReverbTopology::Goldfoil; break;
        }

        // Ducking 係数の動的更新
        const float attMs = juce::jmax(0.1f, activeParams.duckingAttackMs);
        const float relMs = juce::jmax(0.1f, activeParams.duckingRelMs);
        duckingAttackCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(fs) * attMs * 0.001f));
        duckingReleaseCoeff = 1.0f - std::exp(-1.0f / (static_cast<float>(fs) * relMs * 0.001f));

        updateTopologyAndRouting();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  素数べき乗遅延時間の算定
    // ─────────────────────────────────────────────────────────────────────────────
    void UniversalEngine::calculatePrimePowerDelays() {
        static constexpr std::array<int, FDN_ORDER> primes = {
            2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53
        };

        float baseSizeMs = 30.0f * (0.5f + activeParams.roomSizeScale);
        for (int i = 0; i < FDN_ORDER; ++i) {
            float targetSamples = (baseSizeMs + i * 5.0f) * 0.001f * static_cast<float>(fs);
            float m_i = std::round(std::log(targetSamples) / std::log(static_cast<float>(primes[i])));
            fdnBaseDelaySamples[i] = std::pow(static_cast<float>(primes[i]), m_i);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  動的トポロジー構成
    // ─────────────────────────────────────────────────────────────────────────────
    void UniversalEngine::updateTopologyAndRouting() {
        calculatePrimePowerDelays();

        auto& preset = *ALL_PRESETS[activeParams.algorithmIndex];

        // RT60 スケーリング (+ ProMode 拡張)
        std::array<float, NUM_BANDS> scaledRT60 = preset.acoustics.rt60;
        for (auto& v : scaledRT60) v *= activeParams.decayScale;

        if (activeParams.proMode) {
            scaledRT60[0] *= activeParams.tiltLow;
            scaledRT60[1] *= activeParams.tiltLow;
            scaledRT60[2] *= activeParams.tiltLow;
            scaledRT60[3] *= activeParams.tiltMid;
            scaledRT60[4] *= activeParams.tiltMid;
            scaledRT60[5] *= activeParams.tiltMid;
            scaledRT60[6] *= activeParams.tiltMid;
            scaledRT60[7] *= activeParams.tiltHigh;
            scaledRT60[8] *= activeParams.tiltHigh;
            scaledRT60[9] *= activeParams.tiltHigh;

            for (int b = 0; b < NUM_BANDS; ++b)
                scaledRT60[b] *= activeParams.rtBands[b];
        }

        effectiveRT60 = scaledRT60;

#if AMBIENCE_USE_STAGE2_ABSORPTION
        for (int i = 0; i < FDN_ORDER; ++i) {
            auto s2 = MagnitudeResponseFitter::designStage2(
                static_cast<int>(fdnBaseDelaySamples[i]), fs, scaledRT60,
                activeParams.hfDamping, activeParams.lfAbsorption);
            for (int b = 0; b < NUM_BANDS; ++b)
                currentAbsorptionCoeffsS2[i][b] = s2.geqStages[b];
        }
#else
        for (int i = 0; i < FDN_ORDER; ++i) {
            auto absoStages = FilterDesign::designAbsorption(
                static_cast<int>(fdnBaseDelaySamples[i]), fs, scaledRT60,
                activeParams.hfDamping, activeParams.lfAbsorption);
            currentAbsorptionCoeffs[i] = absoStages[0];
        }
#endif

        // AGC
        float rt60Mid = std::max(0.1f, scaledRT60[4]);
        constexpr float baseDB = 28.7f;
        float decayCompDB = 7.0f * std::log10(rt60Mid);

        static constexpr std::array<float, 7> algorithmOffsetDB = {
            +0.8f, +0.9f, +0.5f, +0.5f, +1.5f, +0.6f, +0.6f
        };
        float algoOffset = algorithmOffsetDB[juce::jlimit(0, 6, activeParams.algorithmIndex)];

        // トポロジー結線
        switch (currentTopology) {
        case ReverbTopology::Room:
            bypassER = false; bypassInputDiffusers = true;  apfGain = 0.3f;   break;
        case ReverbTopology::Hall:
            bypassER = false; bypassInputDiffusers = false; apfGain = 0.618f; break;
        case ReverbTopology::Plate:
            bypassER = true;  bypassInputDiffusers = false; apfGain = 0.7f;   break;
        case ReverbTopology::Spring:
            bypassER = true;  bypassInputDiffusers = false; apfGain = 0.5f;   break;
        case ReverbTopology::Goldfoil:
            bypassER = true;  bypassInputDiffusers = false; apfGain = 0.75f;  break;
        }

        // ER パターン更新
        const auto& erPattern = PRESET_ER_PATTERNS[juce::jlimit(0, 6, activeParams.algorithmIndex)];
        currentERTapCount = erPattern.numTaps;

        float erSizeScale = 0.5f + activeParams.roomSizeScale;
        for (int i = 0; i < erPattern.numTaps; ++i) {
            currentERDelaySamples[i] = erPattern.taps[i].delayMs * 0.001f
                * static_cast<float>(fs) * erSizeScale;
            currentERGains[i] = erPattern.taps[i].gain;
        }
        if (erPattern.numTaps == 0) bypassER = true;

        // EDT 理論計算
        float edtCoeff = 0.7f;
        switch (currentTopology) {
        case ReverbTopology::Room:     edtCoeff = 0.70f; break;
        case ReverbTopology::Hall:     edtCoeff = 0.95f; break;
        case ReverbTopology::Plate:    edtCoeff = 0.60f; break;
        case ReverbTopology::Spring:   edtCoeff = 0.50f; break;
        case ReverbTopology::Goldfoil: edtCoeff = 0.85f; break;
        }
        theoreticalEDT = rt60Mid * edtCoeff;

        // ─────────────────────────────────────────────────────────────────────────
        //  Saturator 設定 (v1.2: アルゴリズム別倍率を圧縮)
        // ─────────────────────────────────────────────────────────────────────────
        //   旧倍率: Room=0.6, Hall=0.7, Plate=1.0, Spring=1.2, Goldfoil=1.1
        //     → 同じノブ位置でも Spring は Room の 2 倍歪む (UX 不親切)
        //
        //   新倍率: Room=0.90, Hall=0.93, Plate=1.00, Spring=1.05, Goldfoil=1.02
        //     → 最大差は 15% に圧縮 (プリセット切り替えで急変しない)
        //     → キャラクタの違いは ProMode の SatType 選択と AlgorithmPresets の
        //       デフォルト saturation 値で表現する設計に移行
        // ─────────────────────────────────────────────────────────────────────────
        float satMultiplier = 1.0f;
        switch (currentTopology) {
        case ReverbTopology::Room:     satMultiplier = 0.90f; break;
        case ReverbTopology::Hall:     satMultiplier = 0.93f; break;
        case ReverbTopology::Plate:    satMultiplier = 1.00f; break;
        case ReverbTopology::Spring:   satMultiplier = 1.05f; break;
        case ReverbTopology::Goldfoil: satMultiplier = 1.02f; break;
        }

        float effectiveSatAmount = juce::jlimit(0.0f, 1.0f,
            activeParams.saturation * satMultiplier);

        saturatorL.setAmount(effectiveSatAmount);
        saturatorR.setAmount(effectiveSatAmount);
        saturatorL.setMode(activeParams.satTypeIdx);
        saturatorR.setMode(activeParams.satTypeIdx);

        // 最終ゲイン補正
        float totalLateMakeupDB = baseDB + decayCompDB + algoOffset;
        lateMakeupGainLinear = juce::Decibels::decibelsToGain(totalLateMakeupDB);
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  FWHT & Sign Flipping
    // ─────────────────────────────────────────────────────────────────────────────
    inline void UniversalEngine::fastWalshHadamardTransform(std::array<float, 16>& v) noexcept {
        for (int h = 1; h < 16; h *= 2) {
            for (int i = 0; i < 16; i += h * 2) {
                for (int j = i; j < i + h; ++j) {
                    float x = v[j];
                    float y = v[j + h];
                    v[j] = x + y;
                    v[j + h] = x - y;
                }
            }
        }
        for (int i = 0; i < 16; ++i) v[i] *= 0.25f;
    }

    inline void UniversalEngine::applySignFlipping(std::array<float, 16>& v) noexcept {
        static constexpr std::array<float, 16> flip = {
             1.f, -1.f,  1.f, -1.f, -1.f,  1.f, -1.f,  1.f,
             1.f,  1.f, -1.f, -1.f, -1.f, -1.f,  1.f,  1.f
        };
        for (int i = 0; i < 16; ++i) v[i] *= flip[i];
    }

    // ─────────────────────────────────────────────────────────────────────────────
    //  メイン DSP 処理ループ
    // ─────────────────────────────────────────────────────────────────────────────
    void UniversalEngine::processBlock(const float* inL, const float* inR,
        float* outL, float* outR, int numSamples) noexcept {
        // ─────────────────────────────────────────────────────────────────────────
        //  ブロック単位の事前計算 (per-sample ループの外側)
        // ─────────────────────────────────────────────────────────────────────────
        const float depthSamples = activeParams.modAmount * 0.002f * static_cast<float>(fs);
        const float wetGain = juce::Decibels::decibelsToGain(activeParams.wetDB);
        const float stereoWidth = activeParams.stereoWidth;
        const float crossFeedAmt = activeParams.crossFeed;
        const float erLevel = activeParams.erLevel;
        const float lateLevel = activeParams.lateLevel;
        const bool  erSolo = activeParams.erSolo;
        const float duckThreshLin = juce::Decibels::decibelsToGain(activeParams.duckingThreshDB);
        const float duckAmountDB = activeParams.duckingAmount;

        // ─────────────────────────────────────────────────────────────────────────
        //  LFO 係数のブロック単位事前計算 (exp() を per-sample から排除)
        // ─────────────────────────────────────────────────────────────────────────
        //   各チャンネルの LPF カットオフ = modRate × rateMultiplier
        //   rateMultiplier は黄金比 Weyl 列 [0.80, 1.20] なので、
        //   16ch が同じカットオフを持つことは絶対にない。
        //
        //   exp() は 1 ブロックにつき 16 回のみ (per-sample × FDN_ORDER 回ではない)。
        //   96kHz / 64 samples = 1500 ブロック/秒 → 1500 × 16 = 24000 exp/秒
        //   (per-sample なら 96000 × 16 = 1,536,000 exp/秒 → 64 倍の削減)
        // ─────────────────────────────────────────────────────────────────────────
        std::array<float, FDN_ORDER> lfoCoeffs;
        {
            const float fsf = static_cast<float>(fs);
            constexpr float twoPi = 6.28318530718f;
            for (int i = 0; i < FDN_ORDER; ++i) {
                const float fc = activeParams.modRate * lfos[i].rateMultiplier;
                // 1次 IIR LPF 係数: 0 に近い = 遅い変化、1 に近い = 速い変化
                lfoCoeffs[i] = juce::jlimit(0.0001f, 0.9999f,
                    1.0f - std::exp(-twoPi * fc / fsf));
            }
        }

        // ─── per-sample ループ ───
        for (int n = 0; n < numSamples; ++n) {
            const float leftIn = inL[n];
            const float rightIn = inR[n];
            const float midIn = (leftIn + rightIn) * 0.5f;
            const float sideIn = (leftIn - rightIn) * 0.5f;
            float erOutL = 0.0f, erOutR = 0.0f;

            // ── Ducking エンベロープ検出 ──
            const float inputPeak = juce::jmax(std::abs(leftIn), std::abs(rightIn));
            const float envCoeff = (inputPeak > duckingEnvelope)
                ? duckingAttackCoeff : duckingReleaseCoeff;
            duckingEnvelope += (inputPeak - duckingEnvelope) * envCoeff;

            float duckGainLinear = 1.0f;
            if (duckAmountDB > 0.001f && duckingEnvelope > duckThreshLin) {
                const float envDB = 20.0f * std::log10(juce::jmax(duckingEnvelope, 1e-6f));
                const float overDB = envDB - activeParams.duckingThreshDB;
                const float gainRedDB = -juce::jmin(overDB, duckAmountDB);
                duckGainLinear = juce::Decibels::decibelsToGain(gainRedDB);
            }

            // ── 1. Input Diffusers ──
            float fdnInputMid = midIn;
            if (!bypassInputDiffusers) {
                for (int i = 0; i < 4; ++i) {
                    float delaySmp = (3.0f + i * 2.0f) * 0.001f * static_cast<float>(fs);
                    float d = inputDiffusers[i].read(delaySmp);
                    float w = fdnInputMid + 0.618f * d;
                    inputDiffusers[i].write(w);
                    fdnInputMid = d - 0.618f * w;
                }
            }

            // ── 2. ER Tapped Delay ──
            if (!bypassER) {
                erDelay.write(midIn);
                float erTotalL = 0.0f, erTotalR = 0.0f;
                for (int t = 0; t < currentERTapCount; ++t) {
                    float tapValue = erDelay.read(currentERDelaySamples[t]);
                    float tapGain = currentERGains[t] * 0.5f;
                    if (t % 2 == 0) {
                        erTotalL += tapValue * tapGain;
                        erTotalR += tapValue * tapGain * 0.7f;
                    }
                    else {
                        erTotalR += tapValue * tapGain;
                        erTotalL += tapValue * tapGain * 0.7f;
                    }
                }
                erOutL = erTotalL;
                erOutR = erTotalR;
            }

            // ── 3. FDN + Nested Allpass (16ch) ──
            std::array<float, 16> currentFb = fbVec;
            fastWalshHadamardTransform(currentFb);
            applySignFlipping(currentFb);

            float fdnOutL = 0.0f, fdnOutR = 0.0f;
            std::array<float, 16> nextFb;

            for (int i = 0; i < FDN_ORDER; ++i) {
                // ──────────────────────────────────────────────────────────────
                //  BandlimitedNoiseLFO によるモジュレーション
                // ──────────────────────────────────────────────────────────────
                //   lfoCoeffs[i]: ブロック単位で事前計算済み (per-sample での exp() なし)
                //   各 ch が黄金比 Weyl 列の rateMultiplier を持つため、
                //   16ch の揺れが一切同期しない非周期的モジュレーションを実現。
                // ──────────────────────────────────────────────────────────────
                const float lfoVal = lfos[i].tick(lfoCoeffs[i]);
                const float delaySmp = fdnBaseDelaySamples[i] + lfoVal * depthSamples;
                float d = fdnDelays[i].read(delaySmp);

                // 吸収フィルタ
#if AMBIENCE_USE_STAGE2_ABSORPTION
                for (int s = 0; s < ABSO_STAGES_S2; ++s)
                    d = absorptionFiltersS2[i][s].tick(d, currentAbsorptionCoeffsS2[i][s]);
#else
                d = absorptionFilters[i].tick(d, currentAbsorptionCoeffs[i]);
#endif

                // ──────────────────────────────────────────────────────────────
                //  FDN ループ内マイクロサチュレーション (Layer 1, v1.2)
                // ──────────────────────────────────────────────────────────────
                //   v1.2 の係数で x=1 での圧縮は約 2% (v1.1 の 22% から激減)。
                //   和音 4 音でもほぼ感知不能。Valhalla 流「essentially-linear」。
                //   OutputLimiter が最終段でいかなる発散も捕捉するため、
                //   このサチュレーションは安全弁としての役割に徹する。
                // ──────────────────────────────────────────────────────────────
                d = processMicroSaturation(d);

                // Nested Allpass
                float apfDelaySmp = (1.5f + i * 0.3f) * 0.001f * static_cast<float>(fs);
                float apfD = nestedAllpassDelays[i].read(apfDelaySmp);
                float apfW = d + apfGain * apfD;
                nestedAllpassDelays[i].write(apfW);
                float apfOut = apfD - apfGain * apfW;

                nextFb[i] = apfOut;

                float sideForCh = (i % 2 == 0 ? +sideIn : -sideIn) * stereoWidth;
                float fdnInputForThisCh = (fdnInputMid + sideForCh) * 0.25f;
                fdnDelays[i].write(fdnInputForThisCh + currentFb[i]);

                if (i % 2 == 0) {
                    fdnOutL += apfOut;
                    fdnOutR += apfOut * (1.0f - stereoWidth);
                }
                else {
                    fdnOutR += apfOut;
                    fdnOutL += apfOut * (1.0f - stereoWidth);
                }
            }

            fdnOutL *= 0.125f;
            fdnOutR *= 0.125f;
            fbVec = nextFb;

            // ── CrossFeed ──
            const float xfeed = crossFeedAmt * 0.5f;
            const float origL = fdnOutL;
            const float origR = fdnOutR;
            fdnOutL = origL * (1.0f - xfeed) + origR * xfeed;
            fdnOutR = origR * (1.0f - xfeed) + origL * xfeed;

            // ── 4. ミックス ──
            float erMixL = bypassER ? 0.0f : erOutL * erLevel;
            float erMixR = bypassER ? 0.0f : erOutR * erLevel;
            float lateMixL = fdnOutL * lateMakeupGainLinear * lateLevel;
            float lateMixR = fdnOutR * lateMakeupGainLinear * lateLevel;

            float wetMono = (lateMixL + lateMixR) * 0.5f;
            acousticMetrics.processSample(wetMono);

            // ── 5. Saturation (Layer 2) ──
            float satL = saturatorL.processSample(lateMixL);
            float satR = saturatorR.processSample(lateMixR);

            // ── ER SOLO ──
            if (erSolo) { satL = 0.0f; satR = 0.0f; }

            // ── 6. 最終出力 (Ducking + OutputLimiter) ──
            const float finalWetGain = wetGain * duckGainLinear;
            outL[n] = (erMixL + satL) * finalWetGain;
            outR[n] = (erMixR + satR) * finalWetGain;

            outputLimiter.process(outL[n], outR[n]);
        }
    }

} // namespace FDNReverb