#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace FDNReverb;

FDNReverbAudioProcessor::FDNReverbAudioProcessor()
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true).withOutput("Output", juce::AudioChannelSet::stereo(), true)),
    apvts(*this, nullptr, "FDNReverbState", ParameterHelper::createLayout()) {
}

void FDNReverbAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    // ─── 修正：Oversamplerを無効化（1x固定）───
    // FDN/GEQは線形処理なのでOSは不要。CPU負荷を激減させる。
    int osIdx = 0;  // 強制的に1x（OSなし）
    oversampler = std::make_unique<juce::dsp::Oversampling<float>>(2, osIdx, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
    oversampler->initProcessing(static_cast<size_t>(samplesPerBlock));

    double osSampleRate = sampleRate;  // OSなしなので元のSRそのまま
    int osBlockSize = samplesPerBlock; // 同上

    engine.prepare(osSampleRate, osBlockSize);

    wetBuffer.setSize(2, osBlockSize);
    smoothWetGain.reset(sampleRate, 0.05);
    smoothDryGain.reset(sampleRate, 0.05);
    lastSampleRate = sampleRate;
}

void FDNReverbAudioProcessor::updateEngineParams() {
    // ─── 追加: アルゴリズム変更を検出してデフォルト値をロード ───
    int currentAlgo = (int)*apvts.getRawParameterValue(ParamID::Algorithm);
    if (currentAlgo != lastAlgorithmIndex) {
        if (lastAlgorithmIndex >= 0) {
            // 初回起動時 (lastAlgorithmIndex = -1) は除く
            // ステート復元時の意図しない上書きを防ぐ
            loadPresetDefaults(currentAlgo);
        }
        lastAlgorithmIndex = currentAlgo;
    }
    DSPParams p;
    p.algorithmIndex = (int)*apvts.getRawParameterValue(ParamID::Algorithm);
    p.preDelayMs = *apvts.getRawParameterValue(ParamID::PreDelay);
    p.roomSizeScale = *apvts.getRawParameterValue(ParamID::RoomSize) - 0.5f;
    // 注：ALL_PRESETS が AlgorithmPresets.h にある前提です
    p.decayScale = *apvts.getRawParameterValue(ParamID::DecayTime) / ALL_PRESETS[p.algorithmIndex]->acoustics.rt60[4];
    p.hfDamping = *apvts.getRawParameterValue(ParamID::HFDamping);
    p.lfAbsorption = *apvts.getRawParameterValue(ParamID::LFAbsorption);
    p.diffusion = *apvts.getRawParameterValue(ParamID::Diffusion);
    p.modAmount = *apvts.getRawParameterValue(ParamID::ModAmount);
    p.modRate = *apvts.getRawParameterValue(ParamID::ModRate);
    p.stereoWidth = *apvts.getRawParameterValue(ParamID::StereoWidth);
    p.crossFeed = *apvts.getRawParameterValue(ParamID::CrossFeed);
    p.erLevel = *apvts.getRawParameterValue(ParamID::ERLevel);
    p.saturation = *apvts.getRawParameterValue(ParamID::Saturation);
    p.wetDB = *apvts.getRawParameterValue(ParamID::WetLevel);
    p.dryDB = *apvts.getRawParameterValue(ParamID::DryLevel);
    p.duckingAmount = *apvts.getRawParameterValue(ParamID::DuckAmount);
    p.duckingAttackMs = *apvts.getRawParameterValue(ParamID::DuckAttack);
    p.duckingRelMs = *apvts.getRawParameterValue(ParamID::DuckRelease);
    p.duckingThreshDB = *apvts.getRawParameterValue(ParamID::DuckThresh);

    // ─── 追加: Oversampling 倍率を取得 ───
    // GUI ドロップダウン値 (0=1x, 1=2x, 2=4x, 3=8x) を UniversalEngine に渡す
    // UniversalEngine 内で 8x は 4x にクリップされる (資料の方針通り)
    p.oversamplingIdx = (int)*apvts.getRawParameterValue("oversampling");

    smoothWetGain.setTargetValue(juce::Decibels::decibelsToGain(p.wetDB));
    smoothDryGain.setTargetValue(juce::Decibels::decibelsToGain(p.dryDB));

    // 新エンジンへパラメータ送信
    engine.setParams(p);
}

void FDNReverbAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    updateEngineParams();

    inputRMS_L.store(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
    inputRMS_R.store(buffer.getRMSLevel(1, 0, buffer.getNumSamples()));

    juce::dsp::AudioBlock<float> block(buffer);
    auto osBlock = oversampler->processSamplesUp(block);

    int numSamples = static_cast<int>(osBlock.getNumSamples());
    wetBuffer.setSize(2, numSamples, false, false, true);

    // 新エンジンのオーディオ処理を呼び出し
    engine.processBlock(osBlock.getChannelPointer(0), osBlock.getChannelPointer(1),
        wetBuffer.getWritePointer(0), wetBuffer.getWritePointer(1), numSamples);

    for (int i = 0; i < numSamples; ++i) {
        float w = smoothWetGain.getNextValue();
        float d = smoothDryGain.getNextValue();
        osBlock.setSample(0, i, osBlock.getSample(0, i) * d + wetBuffer.getSample(0, i) * w);
        osBlock.setSample(1, i, osBlock.getSample(1, i) * d + wetBuffer.getSample(1, i) * w);
    }

    oversampler->processSamplesDown(block);

    outputRMS_L.store(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
    outputRMS_R.store(buffer.getRMSLevel(1, 0, buffer.getNumSamples()));
}

void FDNReverbAudioProcessor::getStateInformation(juce::MemoryBlock& d) {
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, d);
}
void FDNReverbAudioProcessor::setStateInformation(const void* d, int s) {
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(d, s));
    if (xml && xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* FDNReverbAudioProcessor::createEditor() { return new FDNReverbEditor(*this); }

// ─────────────────────────────────────────────────────────────────────────────
// プリセット選択時のデフォルト値ロード
// ─────────────────────────────────────────────────────────────────────────────
//   algorithmIndex 変更を検出したときに呼ばれる。
//   APVTS の各パラメータを、選ばれたプリセットの推奨値にセットする。
//   ユーザーが現在いじっているパラメータも上書きされるので注意。
// ─────────────────────────────────────────────────────────────────────────────
void FDNReverbAudioProcessor::loadPresetDefaults(int algorithmIndex) {
    if (algorithmIndex < 0 || algorithmIndex >= 7) return;

    const auto& def = PRESET_DEFAULTS[algorithmIndex];

    // APVTS パラメータを更新
    // sendNotificationSync で GUI に即座に反映
    auto setParam = [this](const juce::String& paramID, float value) {
        if (auto* param = apvts.getParameter(paramID)) {
            // パラメータの正規化された値に変換してセット
            float normalizedValue = param->convertTo0to1(value);
            param->setValueNotifyingHost(normalizedValue);
        }
        };

    setParam(ParamID::RoomSize, def.roomSize);
    setParam(ParamID::DecayTime, def.decayTime);
    setParam(ParamID::HFDamping, def.hfDamp);
    setParam(ParamID::LFAbsorption, def.lfAbsorb);
    setParam(ParamID::Diffusion, def.diffusion);
    setParam(ParamID::ModAmount, def.modAmount);
    setParam(ParamID::ModRate, def.modRate);
    setParam(ParamID::ERLevel, def.erLevel);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FDNReverbAudioProcessor(); }