#include "PluginProcessor.h"
#include "PluginEditor.h"

LumoProcessor::LumoProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout())
{
    modeParam = apvts.getRawParameterValue ("mode");

    auto& st = apvts.state;
    if (! st.hasProperty ("c0")) st.setProperty ("c0", "Kick",  nullptr);
    if (! st.hasProperty ("c1")) st.setProperty ("c1", "Bass",  nullptr);
    if (! st.hasProperty ("c2")) st.setProperty ("c2", "Vocal", nullptr);
    if (! st.hasProperty ("tap")) st.setProperty ("tap", "", nullptr);
    loadNames();
}

LumoProcessor::~LumoProcessor()
{
    TapRegistry::get().release (tapSlot, tapCurrentId);
}

juce::AudioProcessorValueTreeState::ParameterLayout LumoProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "mode", 1 }, "Mode", juce::StringArray { "Master", "Tap" }, 0));
    return layout;
}

void LumoProcessor::loadNames()
{
    tapId.store (TapRegistry::hashName (getTapName()));
    for (int i = 0; i < 3; ++i)
        colId[(size_t) i].store (TapRegistry::hashName (getColumnName (i)));
}

juce::String LumoProcessor::getTapName() const { return apvts.state.getProperty ("tap").toString(); }

void LumoProcessor::setTapName (const juce::String& n)
{
    apvts.state.setProperty ("tap", n, nullptr);
    tapId.store (TapRegistry::hashName (n));
}

juce::String LumoProcessor::getColumnName (int i) const
{
    return apvts.state.getProperty ("c" + juce::String (i)).toString();
}

void LumoProcessor::setColumnName (int i, const juce::String& n)
{
    apvts.state.setProperty ("c" + juce::String (i), n, nullptr);
    colId[(size_t) i].store (TapRegistry::hashName (n));
}

void LumoProcessor::prepareToPlay (double sr, int maxBlock)
{
    sampleRate = sr;
    analyzer.prepare (sr, maxBlock);
    tapPeakHold = 0.f; tapEnv = 0.0;
}

bool LumoProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LumoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;   // audio passes through untouched
    const int n = buffer.getNumSamples();
    if (n == 0 || buffer.getNumChannels() == 0) return;

    const float* l = buffer.getReadPointer (0);
    const float* r = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : l;

    if (isTapMode()) processTap (l, r, n);
    else             analyzer.process (l, r, n);
}

void LumoProcessor::processTap (const float* l, const float* r, int n)
{
    auto& reg = TapRegistry::get();
    const uint64_t id = tapId.load();

    if (id != tapCurrentId)
    {
        reg.release (tapSlot, tapCurrentId);
        tapSlot = -1;
        tapCurrentId = id;
    }
    if (id != 0 && (tapSlot < 0 || reg.slots[tapSlot].id.load() != id))
        tapSlot = reg.claim (id);

    float bp = 0.f; double e = 0.0;
    for (int i = 0; i < n; ++i)
    {
        bp = std::max (bp, std::max (std::abs (l[i]), std::abs (r[i])));
        e += 0.5 * ((double) l[i] * l[i] + (double) r[i] * r[i]);
    }

    const float decay = (float) std::pow (std::pow (10.0, -20.0 / (20.0 * sampleRate)), (double) n); // 20 dB/s
    tapPeakHold = std::max (bp, tapPeakHold * decay);
    const double alpha = 1.0 - std::exp (-(double) n / (sampleRate * 0.3));
    tapEnv += alpha * (e / (double) n - tapEnv);

    tapPeakDb.store (lumo::linToDb (tapPeakHold));
    tapRmsDb.store (lumo::powToDb (tapEnv));

    if (tapSlot >= 0)
    {
        auto& s = reg.slots[tapSlot];
        s.peak.store (tapPeakHold);
        s.rmsSq.store ((float) tapEnv);
        s.stamp.store (juce::Time::getMillisecondCounter());
    }
}

juce::AudioProcessorEditor* LumoProcessor::createEditor() { return new LumoEditor (*this); }

void LumoProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void LumoProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            loadNames();
        }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LumoProcessor(); }
