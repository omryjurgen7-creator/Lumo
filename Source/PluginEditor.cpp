#include "PluginEditor.h"

namespace
{
const juce::Colour kBg (0xff1a1a1a), kField (0xff262626), kLine (0xff3a3a3a),
                   kText (0xffd8d8d8), kDim (0xff7a7a7a);

juce::Font mono (float h)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), h, juce::Font::plain));
}

juce::String fmtDb (float v) { return v <= -100.f ? juce::String ("-inf") : juce::String (v, 1); }
juce::String fmtSigned (float v) { return (v >= 0.f ? "+" : "") + juce::String (v, 1); }

void styleEditor (juce::TextEditor& e, const juce::String& placeholder)
{
    e.setMultiLine (false);
    e.setJustification (juce::Justification::centred);
    e.setFont (mono (14.f));
    e.setTextToShowWhenEmpty (placeholder, kDim);
    e.setColour (juce::TextEditor::backgroundColourId, kField);
    e.setColour (juce::TextEditor::textColourId, kText);
    e.setColour (juce::TextEditor::outlineColourId, kLine);
    e.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xff6a6a6a));
    e.setSelectAllWhenFocused (true);
}
}

void LevelBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    constexpr int segs = 48;
    constexpr float gap = 1.5f;
    const float segH = b.getHeight() / (float) segs;
    const float step = (maxDb - minDb) / (float) segs;

    for (int s = 0; s < segs; ++s)
    {
        const float lo = minDb + step * (float) s, hi = lo + step, mid = lo + step * 0.5f;
        juce::Rectangle<float> r (b.getX(), b.getBottom() - (float) (s + 1) * segH + gap * 0.5f,
                                  b.getWidth(), segH - gap);
        float alpha = 0.10f;
        if (active)
        {
            if (mid <= rmsDb) alpha = 1.0f;
            else if (peakDb >= lo && (peakDb < hi || s == segs - 1)) alpha = 0.85f;
        }
        g.setColour (lumo::colourForDb (mid).withAlpha (alpha));
        g.fillRect (r);
    }
}

LumoEditor::LumoEditor (LumoProcessor& p)
    : juce::AudioProcessorEditor (&p), proc (p)
{
    modeBox.addItemList (juce::StringArray { "Master", "Tap" }, 1);
    modeBox.setColour (juce::ComboBox::backgroundColourId, kField);
    modeBox.setColour (juce::ComboBox::textColourId, kText);
    modeBox.setColour (juce::ComboBox::outlineColourId, kLine);
    addAndMakeVisible (modeBox);
    modeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "mode", modeBox);

    resetButton.setColour (juce::TextButton::buttonColourId, kField);
    resetButton.setColour (juce::TextButton::textColourOffId, kText);
    resetButton.onClick = [this] { proc.analyzer.requestReset(); };
    addAndMakeVisible (resetButton);

    addAndMakeVisible (masterL);
    addAndMakeVisible (masterR);

    for (size_t i = 0; i < cols.size(); ++i)
    {
        auto& c = cols[i];
        styleEditor (c.name, "name");
        c.name.setText (proc.getColumnName ((int) i), false);
        c.name.onTextChange = [this, i] { proc.setColumnName ((int) i, cols[i].name.getText()); };
        addAndMakeVisible (c.name);
        addAndMakeVisible (c.bar);
    }

    styleEditor (tapName, "name (e.g. Kick)");
    tapName.setText (proc.getTapName(), false);
    tapName.onTextChange = [this] { proc.setTapName (tapName.getText()); };
    addAndMakeVisible (tapName);
    addAndMakeVisible (tapBar);

    tapLayout = proc.isTapMode();
    applyMode();
    startTimerHz (60);
}

LumoEditor::~LumoEditor() { stopTimer(); }

void LumoEditor::applyMode()
{
    masterL.setVisible (! tapLayout);
    masterR.setVisible (! tapLayout);
    resetButton.setVisible (! tapLayout);
    for (auto& c : cols) { c.name.setVisible (! tapLayout); c.bar.setVisible (! tapLayout); }
    tapName.setVisible (tapLayout);
    tapBar.setVisible (tapLayout);

    if (getWidth() == (tapLayout ? 280 : 780) && getHeight() == (tapLayout ? 170 : 400))
        resized();
    else
        setSize (tapLayout ? 280 : 780, tapLayout ? 170 : 400);
}

void LumoEditor::resized()
{
    modeBox.setBounds (getWidth() - 112, 10, 100, 24);

    if (tapLayout)
    {
        tapName.setBounds (16, 50, 170, 28);
        tapBar.setBounds (getWidth() - 56, 50, 36, 100);
        return;
    }

    masterL.setBounds (16, 76, 22, 250);
    masterR.setBounds (44, 76, 22, 250);
    resetButton.setBounds (16, 346, 80, 24);

    for (size_t i = 0; i < cols.size(); ++i)
    {
        const int x = 320 + (int) i * 150;
        cols[i].name.setBounds (x, 44, 140, 26);
        cols[i].bar.setBounds (x + 40, 76, 60, 250);
    }
}

void LumoEditor::timerCallback()
{
    if (proc.isTapMode() != tapLayout)
    {
        tapLayout = proc.isTapMode();
        applyMode();
    }

    if (tapLayout)
    {
        tapRms  = proc.tapRmsDb.load();
        tapPeak = proc.tapPeakDb.load();
        tapBar.setLevels (tapRms, tapPeak);
    }
    else
    {
        const auto& r = proc.analyzer.results;
        masterL.setLevels (r.rmsL, r.peakL);
        masterR.setLevels (r.rmsR, r.peakR);

        auto& reg = TapRegistry::get();
        const auto now = juce::Time::getMillisecondCounter();
        for (size_t i = 0; i < cols.size(); ++i)
        {
            auto& c = cols[i];
            const int s = reg.find (proc.getColumnId ((int) i));
            c.active = s >= 0 && (uint32_t) (now - reg.slots[s].stamp.load()) < 500u;
            if (c.active)
            {
                c.peak = lumo::linToDb (reg.slots[s].peak.load());
                c.rms  = lumo::powToDb (reg.slots[s].rmsSq.load());
            }
            c.bar.setLevels (c.rms, c.peak, c.active);
        }
    }
    repaint();
}

void LumoEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (mono (16.f));
    g.drawText (tapLayout ? "LUMO  /  TAP" : "LUMO", 16, 8, 200, 26, juce::Justification::centredLeft);

    auto row = [&] (int x, int y, int w, const juce::String& label, const juce::String& value, juce::Colour c)
    {
        g.setFont (mono (12.f));
        g.setColour (kDim); g.drawText (label, x, y, w / 2, 18, juce::Justification::centredLeft);
        g.setColour (c);    g.drawText (value, x + w / 2, y, w / 2, 18, juce::Justification::centredRight);
    };

    if (tapLayout)
    {
        row (16, 100, 160, "RMS",  fmtDb (tapRms),  kText);
        row (16, 118, 160, "PEAK", fmtDb (tapPeak), lumo::colourForDb (tapPeak));
        return;
    }

    const auto& r = proc.analyzer.results;
    g.setFont (mono (12.f)); g.setColour (kDim);
    g.drawText ("MASTER", 16, 46, 100, 18, juce::Justification::centredLeft);

    int y = 76;
    auto m = [&] (const juce::String& label, const juce::String& value, juce::Colour c = kText)
    {
        row (84, y, 215, label, value, c);
        y += 18;
    };

    m ("PEAK L|R",  fmtDb (r.peakL) + " | " + fmtDb (r.peakR), lumo::colourForDb (std::max (r.peakL.load(), r.peakR.load())));
    m ("TRUE PK",   fmtDb (r.truePeakL) + " | " + fmtDb (r.truePeakR), lumo::colourForDb (std::max (r.truePeakL.load(), r.truePeakR.load())));
    m ("TP MAX",    fmtDb (r.maxTruePeak), lumo::colourForDb (r.maxTruePeak));
    m ("RMS L|R",   fmtDb (r.rmsL) + " | " + fmtDb (r.rmsR));
    m ("LUFS M",    fmtDb (r.momentary));
    m ("LUFS S",    fmtDb (r.shortTerm));
    m ("LUFS I",    fmtDb (r.integrated));
    m ("LRA",       juce::String (r.lra.load(), 1) + " LU");
    m ("CREST",     juce::String (r.crest.load(), 1) + " dB");
    m ("PLR",       juce::String (r.plr.load(), 1) + " dB");

    const float corr = r.correlation.load();
    m ("CORR", juce::String (corr, 2),
       corr > 0.3f ? juce::Colour (0xff2ecc71) : corr > 0.0f ? juce::Colour (0xfff1c40f) : juce::Colour (0xffe74c3c));

    const float bal = r.balance.load();
    m ("BAL", std::abs (bal) < 0.05f ? juce::String ("center")
                                     : (bal > 0 ? "L +" : "R +") + juce::String (std::abs (bal), 1) + " dB");

    // columns
    for (size_t i = 0; i < cols.size(); ++i)
    {
        const auto& c = cols[i];
        const int x = 320 + (int) i * 150;

        if (! c.active)
        {
            g.setFont (mono (12.f)); g.setColour (kDim);
            g.drawText (c.name.isEmpty() ? "NO NAME" : "NO TAP", x, 332, 140, 18, juce::Justification::centred);
            continue;
        }

        row (x + 10, 332, 120, "RMS",  fmtDb (c.rms),  kText);
        row (x + 10, 350, 120, "PEAK", fmtDb (c.peak), lumo::colourForDb (c.peak));

        if (i == 0)
            row (x + 10, 368, 120, "VS REF", "REF", kDim);
        else if (cols[0].active)
            row (x + 10, 368, 120, "VS REF", fmtSigned (c.rms - cols[0].rms) + " dB", kText);
        else
            row (x + 10, 368, 120, "VS REF", "--", kDim);
    }
}
