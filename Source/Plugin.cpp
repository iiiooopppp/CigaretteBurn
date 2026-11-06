#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <vector>

static const char* kBurnID = "burn";

class BurnProcessor : public juce::AudioProcessor
{
public:
    BurnProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "STATE", createLayout())
    {
    }

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { kBurnID, 1 }, "Burn",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.0001f), 0.0f));
        return layout;
    }

    const juce::String getName() const override { return "Cigarette Burn"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void prepareToPlay (double sampleRate, int) override
    {
        sr = sampleRate;
        burnSmooth.reset (sampleRate, 0.03);
        burnSmooth.setCurrentAndTargetValue (apvts.getRawParameterValue (kBurnID)->load());
        for (int c = 0; c < 2; ++c) { lp[c] = 0.f; held[c] = 0.f; holdCnt[c] = 0; crackEnv[c] = 0.f; }
    }
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        auto out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
            return false;
        return out == layouts.getMainInputChannelSet();
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int n = buffer.getNumSamples();
        const int chs = juce::jmin (buffer.getNumChannels(), 2);
        for (int c = chs; c < buffer.getNumChannels(); ++c)
            buffer.clear (c, 0, n);

        burnSmooth.setTargetValue (apvts.getRawParameterValue (kBurnID)->load());

        float peak = 0.f;

        for (int i = 0; i < n; ++i)
        {
            const float b = burnSmooth.getNextValue();

            const float drive    = 1.f + 2.f * b + 26.f * b * b;
            const float bias     = 0.18f * b;
            const int   holdN    = 1 + (int) (b * b * b * 9.f);
            const float cutoff   = 18000.f * std::pow (0.12f, b);
            const float a        = 1.f - std::exp (-juce::MathConstants<float>::twoPi * cutoff / (float) sr);
            const float mix      = juce::jmin (1.f, b * 5.f);
            const float outGain  = 1.f - 0.45f * b;
            const float crackleP = 0.0009f * b * b * (float) (44100.0 / sr);

            for (int c = 0; c < chs; ++c)
            {
                float* d = buffer.getWritePointer (c);
                const float dry = d[i];

                if (holdCnt[c] <= 0) { held[c] = dry; holdCnt[c] = holdN; }
                --holdCnt[c];

                float y = std::tanh (held[c] * drive + bias) - std::tanh (bias);

                lp[c] += a * (y - lp[c]);
                y = lp[c];

                if (rng.nextFloat() < crackleP)
                    crackEnv[c] = (rng.nextFloat() * 2.f - 1.f) * 0.5f * (0.3f + b);
                crackEnv[c] *= 0.97f;
                y += crackEnv[c];
                y += (rng.nextFloat() * 2.f - 1.f) * 0.004f * b * b;

                float out = dry * (1.f - mix) + y * outGain * mix;
                out = juce::jlimit (-2.f, 2.f, out);
                d[i] = out;
                peak = juce::jmax (peak, std::abs (out));
            }
        }
        level.store (peak);
    }

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, dest);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            if (xml->hasTagName (apvts.state.getType()))
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> level { 0.f };

private:
    double sr = 44100.0;
    juce::SmoothedValue<float> burnSmooth;
    juce::Random rng;
    float lp[2] {}, held[2] {}, crackEnv[2] {};
    int holdCnt[2] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BurnProcessor)
};

class BurnLook : public juce::LookAndFeel_V4
{
public:
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startA, float endA, juce::Slider&) override
    {
        auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (8.f);
        auto c = b.getCentre();
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff6a00).withAlpha (0.45f * pos), c.x, c.y,
                                                 juce::Colours::transparentBlack, c.x, c.y - r * 1.7f, true));
        g.fillEllipse (c.x - r * 1.7f, c.y - r * 1.7f, r * 3.4f, r * 3.4f);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff463e38), c.x, c.y - r,
                                                 juce::Colour (0xff0d0b0a), c.x, c.y + r, false));
        g.fillEllipse (c.x - r, c.y - r, r * 2.f, r * 2.f);
        g.setColour (juce::Colour (0xff7a6b5e));
        g.drawEllipse (c.x - r, c.y - r, r * 2.f, r * 2.f, 2.f);

        juce::Path track;
        track.addCentredArc (c.x, c.y, r * 0.82f, r * 0.82f, 0.f, startA, endA, true);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.strokePath (track, juce::PathStrokeType (5.f));

        const float a = startA + pos * (endA - startA);
        juce::Path val;
        val.addCentredArc (c.x, c.y, r * 0.82f, r * 0.82f, 0.f, startA, a, true);
        g.setColour (juce::Colour (0xffff5a10).interpolatedWith (juce::Colour (0xffffd25a), pos));
        g.strokePath (val, juce::PathStrokeType (5.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Point<float> p1 (c.x + std::sin (a) * r * 0.25f, c.y - std::cos (a) * r * 0.25f);
        juce::Point<float> p2 (c.x + std::sin (a) * r * 0.62f, c.y - std::cos (a) * r * 0.62f);
        g.setColour (juce::Colour (0xffffe9c4));
        g.drawLine (p1.x, p1.y, p2.x, p2.y, 3.f);
    }
};

class BurnEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BurnEditor (BurnProcessor& p) : AudioProcessorEditor (&p), proc (p)
    {
        setSize (640, 430);

        knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        knob.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f,
                                  juce::MathConstants<float>::pi * 2.8f, true);
        knob.setDoubleClickReturnValue (true, 0.0);
        knob.setLookAndFeel (&look);
        addAndMakeVisible (knob);
        attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, kBurnID, knob);

        juce::Random r (42);
        for (int i = 0; i < 16; ++i) wrinkles.push_back ({ r.nextFloat(), (r.nextFloat() - 0.5f) * 8.f });
        for (int i = 0; i < 6; ++i)  ashWob[i] = r.nextFloat();

        burnV = (float) knob.getValue();
        startTimerHz (60);
    }

    ~BurnEditor() override
    {
        stopTimer();
        knob.setLookAndFeel (nullptr);
    }

    void resized() override { knob.setBounds (getWidth() / 2 - 60, 270, 120, 120); }

    void timerCallback() override
    {
        t += 1.f / 60.f;
        burnV += ((float) knob.getValue() - burnV) * 0.25f;
        lv += (proc.level.load() - lv) * 0.3f;

        flick = 0.8f + 0.12f * std::sin (t * 13.f) + 0.08f * std::sin (t * 29.f + 1.f)
                + juce::jmin (lv, 1.f) * 0.35f + (rnd.nextFloat() - 0.5f) * 0.1f;

        const float tx = tipX() + ashLen() + 4.f;

        for (int i = 0; i < 2; ++i)
            if (rnd.nextFloat() < 0.15f + burnV * 0.5f)
                particles.push_back ({ tx + (rnd.nextFloat() - 0.5f) * 6.f, cy - 6.f,
                                       (rnd.nextFloat() - 0.3f) * 0.5f,
                                       -(0.5f + rnd.nextFloat() * 0.8f + burnV * 0.8f),
                                       0.f, 90.f + rnd.nextFloat() * 110.f, 4.f + rnd.nextFloat() * 4.f, 0 });

        if (burnV > 0.3f && rnd.nextFloat() < (burnV - 0.3f) * 0.6f)
            particles.push_back ({ tx, cy, (rnd.nextFloat() - 0.5f) * 3.f, -(1.f + rnd.nextFloat() * 2.5f),
                                   0.f, 30.f + rnd.nextFloat() * 35.f, 1.5f + rnd.nextFloat() * 1.5f, 1 });

        if (burnV > 0.5f && rnd.nextFloat() < 0.04f)
            particles.push_back ({ tx - 6.f, cy + 8.f, (rnd.nextFloat() - 0.5f) * 0.6f, 0.2f,
                                   0.f, 110.f, 1.5f + rnd.nextFloat() * 2.f, 2 });

        for (auto& p : particles)
        {
            p.life += 1.f;
            p.x += p.vx;
            p.y += p.vy;
            if (p.kind == 0) { p.vx += 0.012f * std::sin (t * 2.f + p.y * 0.05f); p.size += 0.13f; }
            if (p.kind == 1) { p.vy += 0.04f; }
            if (p.kind == 2) { p.vy += 0.06f; }
        }
        particles.erase (std::remove_if (particles.begin(), particles.end(),
                                         [] (const P& p) { return p.life >= p.maxLife; }),
                         particles.end());
        if (particles.size() > 500) particles.erase (particles.begin(), particles.begin() + 100);

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const float b = burnV;
        const auto W = (float) getWidth();
        const auto H = (float) getHeight();
        const float x0 = 70.f, h = 28.f;
        const float tip = tipX();
        const float al = ashLen();
        const float top = cy - h * 0.5f;
        const float bot = cy + h * 0.5f;

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a1d16), 0.f, 0.f,
                                                 juce::Colour (0xff060403), 0.f, H, false));
        g.fillAll();

        {
            const float rad = 130.f + 280.f * b;
            const float a = juce::jlimit (0.f, 1.f, (0.14f + 0.38f * b) * flick);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff5a10).withAlpha (a), tip + al, cy,
                                                     juce::Colours::transparentBlack, tip + al + rad, cy, true));
            g.fillEllipse (tip + al - rad, cy - rad, rad * 2.f, rad * 2.f);
        }

        juce::Rectangle<float> body (x0, top, juce::jmax (4.f, tip - x0), h);
        juce::Path bodyPath;
        bodyPath.addRoundedRectangle (body, 6.f);

        juce::Path twist;
        twist.startNewSubPath (x0, top);
        twist.quadraticTo (x0 - 14.f, top + 4.f, x0 - 30.f, cy - 3.f);
        twist.lineTo (x0 - 36.f, cy - 6.f);
        twist.lineTo (x0 - 33.f, cy + 4.f);
        twist.lineTo (x0 - 30.f, cy + 3.f);
        twist.quadraticTo (x0 - 14.f, bot - 4.f, x0, bot);
        twist.closeSubPath();

        auto paper = juce::ColourGradient (juce::Colour (0xfffff3dc), 0.f, top,
                                           juce::Colour (0xffb9a47c), 0.f, bot, false);
        paper.addColour (0.5, juce::Colour (0xffe9d8b4));
        g.setGradientFill (paper);
        g.fillPath (twist);
        g.fillPath (bodyPath);
        g.setColour (juce::Colour (0xff6b5a3c).withAlpha (0.6f));
        g.strokePath (twist, juce::PathStrokeType (1.2f));

        g.saveState();
        g.reduceClipRegion (bodyPath);

        g.setColour (juce::Colour (0xffb08d57));
        g.fillRect (x0 + 2.f, top, 38.f, h);
        g.setColour (juce::Colour (0xff7d5f33).withAlpha (0.55f));
        for (float xx = x0 + 6.f; xx < x0 + 40.f; xx += 5.f)
            g.drawLine (xx, top, xx, bot, 1.f);

        g.setColour (juce::Colour (0xff6b5a3c).withAlpha (0.16f));
        for (auto& w : wrinkles)
        {
            const float xx = x0 + 44.f + w.t * juce::jmax (1.f, tip - x0 - 44.f);
            g.drawLine (xx, top + 2.f, xx + w.skew, bot - 2.f, 1.f);
        }
        g.setColour (juce::Colour (0xff6b5a3c).withAlpha (0.2f));
        g.drawLine (x0, cy - h * 0.18f, tip, cy - h * 0.18f, 1.f);

        {
            const float cw = 16.f + 44.f * b;
            g.setGradientFill (juce::ColourGradient (juce::Colour (0x00301a0c), tip - cw, cy,
                                                     juce::Colour (0xf0180d07), tip, cy, false));
            g.fillRect (tip - cw, top, cw, h);

            auto ember = juce::ColourGradient (juce::Colour (0x00ff3b00), tip - 14.f, cy,
                                               juce::Colour (0xffffc040).withAlpha (juce::jlimit (0.f, 1.f, flick)), tip, cy, false);
            ember.addColour (0.55, juce::Colour (0xffff3b00).withAlpha (juce::jlimit (0.f, 1.f, flick)));
            g.setGradientFill (ember);
            g.fillRect (tip - 14.f, top, 14.f, h);
        }
        g.restoreState();

        {
            juce::Path ash;
            ash.startNewSubPath (tip, top + 1.f);
            ash.lineTo (tip + al * 0.5f,  top - 2.f + ashWob[0] * 3.f);
            ash.lineTo (tip + al * 0.95f, top + 3.f + ashWob[1] * 3.f);
            ash.lineTo (tip + al,         cy - 4.f + ashWob[2] * 3.f);
            ash.lineTo (tip + al * 0.9f,  cy + 6.f + ashWob[3] * 4.f);
            ash.lineTo (tip + al * 0.97f, bot - 3.f);
            ash.lineTo (tip + al * 0.5f,  bot + 2.f - ashWob[4] * 2.f);
            ash.lineTo (tip,              bot - 1.f);
            ash.closeSubPath();

            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff7a736b), tip, cy,
                                                     juce::Colour (0xffd9d4cc), tip + al, cy, false));
            g.fillPath (ash);

            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff6a10).withAlpha (juce::jlimit (0.f, 1.f, 0.7f * flick)), tip, cy,
                                                     juce::Colours::transparentBlack, tip + al * 0.7f, cy, false));
            g.fillPath (ash);

            g.setColour (juce::Colour (0xff4a443e).withAlpha (0.5f));
            g.drawLine (tip + al * 0.35f, top + 3.f, tip + al * 0.5f, cy + 2.f, 1.f);
            g.drawLine (tip + al * 0.6f,  cy - 6.f,  tip + al * 0.75f, bot - 2.f, 1.f);
            g.strokePath (ash, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
        }

        for (auto& p : particles)
        {
            const float k = p.life / p.maxLife;
            if (p.kind == 0)
            {
                g.setColour (juce::Colour (0xffc4bdb5).withAlpha (std::sin (k * juce::MathConstants<float>::pi) * 0.2f));
                g.fillEllipse (p.x - p.size, p.y - p.size, p.size * 2.f, p.size * 2.f);
            }
            else if (p.kind == 1)
            {
                g.setColour (juce::Colour (0xffff8a20).withAlpha ((1.f - k) * 0.35f));
                g.fillEllipse (p.x - p.size * 2.5f, p.y - p.size * 2.5f, p.size * 5.f, p.size * 5.f);
                g.setColour (juce::Colour (0xffffe08a).withAlpha (1.f - k));
                g.fillEllipse (p.x - p.size * 0.5f, p.y - p.size * 0.5f, p.size, p.size);
            }
            else
            {
                g.setColour (juce::Colour (0xff9c958d).withAlpha (1.f - k));
                g.fillRect (p.x, p.y, p.size, p.size);
            }
        }

        g.setColour (juce::Colour (0xffe8d9c0).withAlpha (0.85f));
        g.setFont (juce::Font (22.f, juce::Font::bold));
        g.drawText ("CIGARETTE BURN", 0, 16, (int) W, 28, juce::Justification::centred);

        const char* stage = b < 0.15f ? "FRESH" : b < 0.4f ? "SMOLDERING" : b < 0.7f ? "BURNING"
                          : b < 0.9f ? "CHAIN-SMOKING" : "NOTHING BUT ASH";
        g.setFont (juce::Font (13.f, juce::Font::bold));
        g.setColour (juce::Colour (0xffff8a30).withAlpha (0.9f));
        g.drawText (juce::String (stage) + "  -  " + juce::String ((int) std::round (b * 100.f)) + "%",
                    0, 246, (int) W, 18, juce::Justification::centred);

        g.setColour (juce::Colour (0xffe8d9c0).withAlpha (0.6f));
        g.drawText ("BURN", 0, 400, (int) W, 18, juce::Justification::centred);

        g.setGradientFill (juce::ColourGradient (juce::Colours::transparentBlack, W * 0.5f, H * 0.5f,
                                                 juce::Colours::black.withAlpha (0.6f), 0.f, 0.f, true));
        g.fillAll();
    }

private:
    struct P { float x, y, vx, vy, life, maxLife, size; int kind; };
    struct W { float t, skew; };

    float tipX() const   { return 70.f + 440.f * (1.f - 0.72f * burnV); }
    float ashLen() const { return 20.f + 26.f * burnV; }

    BurnProcessor& proc;
    BurnLook look;
    juce::Slider knob;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;

    std::vector<P> particles;
    std::vector<W> wrinkles;
    float ashWob[6] {};
    juce::Random rnd;

    const float cy = 150.f;
    float burnV = 0.f, lv = 0.f, t = 0.f, flick = 1.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BurnEditor)
};

juce::AudioProcessorEditor* BurnProcessor::createEditor() { return new BurnEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new BurnProcessor(); }
