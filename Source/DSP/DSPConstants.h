#pragma once
#include <array>
namespace FDNReverb {
    // ── Compile-time constants ────────────────────────────────────────────────────
    static constexpr int FDN_N = 8;          // FDN order (チャンネル数, 旧定義/参考用)
    static constexpr int SAPF_STAGES = 3;    // allpass stages per delay line
    static constexpr int ABSO_STAGES = 3;    // Stage 1 用: Jot 1次 + LF/HF ユーザー補正
    static constexpr int ER_TAPS = 16;       // early-reflection FIR taps

    // Stage 2 (Välimäki–Liski 累積バイカッドGEQ) 用の段数:
    //   1 段: プリシェルフ (Two-Stage Attenuation Filter の第1段, Nyquist 端補正)
    //  10 段: 10 オクターブバンド GEQ (Interaction Matrix + WLS でフィット)
    //   1 段: ユーザー LF Absorption 補正シェルフ
    //   ─ 合計 12 段
    //
    // 注: Stage 2 では HF Damping ユーザー操作は GEQ の高域端で吸収するため、
    //     独立した HF Shelf 段は持たない (過剰減衰回避)。
    static constexpr int ABSO_STAGES_S2 = 12;

    // Mutually-prime base delays (samples @ 48 kHz), log-distributed 30–130 ms
    static constexpr std::array<int, FDN_N> BASE_PRIMES_48K = {
        1451, 1693, 1979, 2311, 2683, 3067, 3491, 3923
    };
} // namespace FDNReverb