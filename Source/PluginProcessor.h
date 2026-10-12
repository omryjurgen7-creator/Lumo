#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "MasterAnalyzer.h"
#include "TapRegistry.h"

class LumoProcessor : public juce::AudioProcessor
{
public:
    LumoProcessor();
    ~LumoProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Lumo"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    bool isTapMode() const { return modeParam->load() > 0.5f; }

    juce::String getTapName() const;
    void setTapName (const juce::String&);
    juce::String getColumnName (int i) const;
    void setColumnName (int i, const juce::String&);
    uint64_t getColumnId (int i) const { return colId[(size_t) i].load(); }

    juce::AudioProcessorValueTreeState apvts;
    lumo::MasterAnalyzer analyzer;

    std::atomic<float> tapPeakDb { -120.f }, tapRmsDb { -120.f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void loadNames();
    void processTap (const float* l, const float* r, int n);

    std::atomic<float>* modeParam = nullptr;
    std::atomic<uint64_t> tapId { 0 };
    std::array<std::atomic<uint64_t>, 3> colId {};

    // tap-mode state (audio thread)
    int tapSlot = -1;
    uint64_t tapCurrentId = 0;
    float tapPeakHold = 0.f;
    double tapEnv = 0.0, sampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LumoProcessor)
};
