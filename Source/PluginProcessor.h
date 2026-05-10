#pragma once
#include <JuceHeader.h>
#include "DSP/UniversalEngine.h" // ← ここを変更
#include "PluginParameters.h"

class FDNReverbAudioProcessor : public juce::AudioProcessor {
public:
    FDNReverbAudioProcessor();
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { engine.reset(); }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Ambience"; }
    double getTailLengthSeconds() const override { return 20.0; }

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    std::array<float, FDNReverb::NUM_BANDS> getRT60ForDisplay() const noexcept { return engine.getEffectiveRT60(); }
    float getInputRMSL()  const noexcept { return inputRMS_L.load(); }
    float getInputRMSR()  const noexcept { return inputRMS_R.load(); }
    float getOutputRMSL() const noexcept { return outputRMS_L.load(); }
    float getOutputRMSR() const noexcept { return outputRMS_R.load(); }
    // ─── 追加: AcousticMetrics の値を GUI に提供 ───
    float getD50() const noexcept { return engine.getD50(); }
    float getC50() const noexcept { return engine.getC50(); }
    float getC80() const noexcept { return engine.getC80(); }
    float getEDT() const noexcept { return engine.getEDT(); }


    // ─── 追加: UniversalEngine への直接アクセス（GUI 用） ───
    const FDNReverb::UniversalEngine& getEngine() const noexcept { return engine; }

    // ─── 追加: プリセット選択時のデフォルト値ロード ───
    void loadPresetDefaults(int algorithmIndex);

private:
    void updateEngineParams();

    FDNReverb::UniversalEngine engine; // ← ここを変更
    // ─── 追加: 前回のアルゴリズムインデックス（変更検出用） ───
    int lastAlgorithmIndex{ -1 };

    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> wetBuffer;
    juce::SmoothedValue<float> smoothWetGain, smoothDryGain;
    std::atomic<float> inputRMS_L{ 0.f }, inputRMS_R{ 0.f }, outputRMS_L{ 0.f }, outputRMS_R{ 0.f };
    double lastSampleRate{ 0.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FDNReverbAudioProcessor)
};