#pragma once
#include "PluginProcessor.h"

namespace lumo
{
// green -> yellow -> red, by level
inline juce::Colour colourForDb (float db)
{
    const juce::Colour green (0xff2ecc71), yellow (0xfff1c40f), red (0xffe74c3c);
    if (db <= -18.f) return green;
    if (db <= -6.f)  return green.interpolatedWith (yellow, (db + 18.f) / 12.f);
    if (db <= -1.f)  return yellow.interpolatedWith (red, (db + 6.f) / 5.f);
    return red;
}
}

// Segmented RGB bar: lit segments = RMS, one bright segment = peak.
class LevelBar : public juce::Component
{
public:
    void setLevels (float rms, float peak, bool isActive = true)
    {
        if (rms != rmsDb || peak != peakDb || isActive != active)
        {
            rmsDb = rms; peakDb = peak; active = isActive;
            repaint();
        }
    }
    void paint (juce::Graphics&) override;

private:
    static constexpr float minDb = -60.f, maxDb = 0.f;
    float rmsDb = -120.f, peakDb = -120.f;
    bool active = true;
};

class LumoEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit LumoEditor (LumoProcessor&);
    ~LumoEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void applyMode();

    struct Column
    {
        juce::TextEditor name;
        LevelBar bar;
        bool active = false;
        float rms = -120.f, peak = -120.f;
    };

    LumoProcessor& proc;
    juce::ComboBox modeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttach;
    juce::TextButton resetButton { "RESET" };

    LevelBar masterL, masterR;
    std::array<Column, 3> cols;

    LevelBar tapBar;
    juce::TextEditor tapName;
    float tapRms = -120.f, tapPeak = -120.f;
    bool tapLayout = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LumoEditor)
};
