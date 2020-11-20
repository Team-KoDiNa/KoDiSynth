#pragma once

#include <JuceHeader.h>

//==============================================================================
/*
    This component lives inside our window, and this is where you should put all
    your controls and content.
*/
struct  SineWaveSound : public juce::SynthesiserSound
{
    SineWaveSound() {}

    bool appliesToNote(int) override {return  true;}
    bool appliesToChannel(int) override {return true;}
};

struct SineWaveVoice   : public juce::SynthesiserVoice
{
    SineWaveVoice() {}

    bool canPlaySound (juce::SynthesiserSound* sound) override
    {
        return dynamic_cast<SineWaveSound*> (sound) != nullptr;
    }

    void startNote (int midiNoteNumber, float velocity,
                    juce::SynthesiserSound*, int /*currentPitchWheelPosition*/) override
    {
        currentAngle = 0.0;
        level = velocity * 0.15;
        tailOff = 0.0;

        auto cyclesPerSecond = juce::MidiMessage::getMidiNoteInHertz (midiNoteNumber);
        auto cyclesPerSample = cyclesPerSecond / getSampleRate();

        angleDelta = cyclesPerSample * 2.0 * juce::MathConstants<double>::pi;
    }

    void stopNote (float /*velocity*/, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            if (tailOff == 0.0)
                tailOff = 1.0;
        }
        else
        {
            clearCurrentNote();
            angleDelta = 0.0;
        }
    }

    void pitchWheelMoved (int) override      {}
    void controllerMoved (int, int) override {}

    void renderNextBlock (juce::AudioSampleBuffer& outputBuffer, int startSample, int numSamples) override
    {
        if (angleDelta != 0.0)
        {
            if (tailOff > 0.0) // [7]
            {
                while (--numSamples >= 0)
                {
                    auto currentSample = (float) (std::sin (currentAngle) * level * tailOff);

                    for (auto i = outputBuffer.getNumChannels(); --i >= 0;)
                        outputBuffer.addSample (i, startSample, currentSample);

                    currentAngle += angleDelta;
                    ++startSample;

                    tailOff *= 0.99; // [8]

                    if (tailOff <= 0.005)
                    {
                        clearCurrentNote(); // [9]

                        angleDelta = 0.0;
                        break;
                    }
                }
            }
            else
            {
                while (--numSamples >= 0) // [6]
                {
                    auto currentSample = (float) (std::sin (currentAngle) * level);

                    for (auto i = outputBuffer.getNumChannels(); --i >= 0;)
                        outputBuffer.addSample (i, startSample, currentSample);

                    currentAngle += angleDelta;
                    ++startSample;
                }
            }
        }
    }

private:
    double currentAngle = 0.0, angleDelta = 0.0, level = 0.0, tailOff = 0.0;
};

class SynthAudioSource : public juce::AudioSource {
public:
    SynthAudioSource(juce::MidiKeyboardState &keyState) : keyboardState(keyState) {
        for (auto i = 0; i < 4; i++) synth.addVoice(new SineWaveVoice());
        synth.addSound(new SineWaveSound());
    }

    void setUsingSineWaveSound() {
        synth.clearSound();
    }

    void prepareToPlay(int /*smaplesPerBlockExpected*/, double sampleRate) override {
        synth.setCurrentPlaybackSampleRate(sampleRate);
        midiCollector.reset(sampleRate);
    }

    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo &bufferToFill) override {
        bufferToFill.clearActiveBufferRegion();

        juce::MidiBuffer incomingMidi;
        midiCollector.removeNextBlockOfMessages(incomingMidi, bufforToFill.numSample);

        keyboardState.processNextMidiBuffer(incomingMidi, bufferToFill.startSample, bufferToFill.numSamples, true);

        synth.renderNextBlock(*bufferToFill.buffer, incomingMidi, bufferToFill.startSample, bufferToFill.numSamples);
    }

    juce::MidiMessageCollector* getMidiCollector()
    {
        return &midiCollector;
    }

private:
    juce::MidiKeyboardState &keyboardState;
    juce::Synthesiser synth;
    juce::MidiMessageCollector midiCollector;
};

class MainComponent  : public juce::AudioAppComponent
{
public:
    //==============================================================================
    MainComponent() : synthAudioSource(keyboardState), keyboardComponent(keyboardState, juce::MidiKeyBoardComponent::horizontalKey)
    {
        addAndMakeVisible (midiInputListLabel);
        midiINputLIstLabel.setText("MIDI Input:", juec::dontSendNotification);
        midiInputListLabel.attachToComponent (&midiInputList, true);

        auto midiInputs = juce::MidiInput::getAvailableDevices();
        addAndMakeVisible (midiInputList);
        midiInputList.setTextWhenNoChoicesAvailable ("No MIDI INputs Enabled");

        juce::StringArray midiInputNames;
        for (auto input : midiInputs) midiInputNames.add(input.name);

        midiInputList.addItemList(midiInputNames, 1);
        midiInputList.onChange = [this] { setMidiInput(midiInputList.getSelectedItemIndex()); };

        for(auto input : midiInputs)
        {
            if (deviceManager.isMidiInputDeviceEnabled (input.identifier))
            {
                setMidiInput(midiInputs.indexOf(input));
                break;
            }
        }

        if(midiInputList.getSelectedId() == 0) setMidiInput(0);
        // Make sure you set the size of the component after
        // you add any child components.
        addAndMakeVisible (keyboardComponent);
        setAudioChannels(0,2);

        setSize (600, 160);
        startTimer(400);

        // Some platforms require permissions to open input channels so request that here
        if (juce::RuntimePermissions::isRequired (juce::RuntimePermissions::recordAudio)
            && ! juce::RuntimePermissions::isGranted (juce::RuntimePermissions::recordAudio))
        {
            juce::RuntimePermissions::request (juce::RuntimePermissions::recordAudio,
                                               [&] (bool granted) { setAudioChannels (granted ? 2 : 0, 2); });
        }
        else
        {
            // Specify the number of input and output channels that we want to open
            setAudioChannels (2, 2);
        }
    }

    ~MainComponent() override
    {
        // This shuts down the audio device and clears the audio source.
        shutdownAudio();
    }

    //==============================================================================
    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override
    {
        // This function will be called when the audio device is started, or when
        // its settings (i.e. sample rate, block size, etc) are changed.

        // You can use this function to initialise any resources you might need,
        // but be careful - it will be called on the audio thread, not the GUI thread.

        // For more details, see the help for AudioProcessor::prepareToPlay()
        synthAudioSource.prepareToPlay(samplesPerBlockExpected, sampleRate);
    }

    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        // Your audio-processing code goes here!

        // For more details, see the help for AudioProcessor::getNextAudioBlock()

        // Right now we are not producing any data, in which case we need to clear the buffer
        // (to prevent the output of random noise)

        synthAudioSource.getNextAudioBlock
    }

    void releaseResources() override
    {
        // This will be called when the audio device stops, or when it is being
        // restarted due to a setting change.

        // For more details, see the help for AudioProcessor::releaseResources()
        synthAudioSource.releaseResources();
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        // (Our component is opaque, so we must completely fill the background with a solid colour)
        g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

        // You can add your drawing code here!
    }

    void resized() override
    {
        midiInputList.setBounds(200, 10, getWidth() - 210, 20);
        keyboardComponent.setBounds(10, 40, getWidth() - 20, getHeight() - 50);
        // This is called when the MainContentComponent is resized.
        // If you add any child components, this is where you should
        // update their positions.
    }



private:
    //==============================================================================
    // Your private member variables go here...
    void timerCallback() override
    {
        keyboardComponent.grabKeyboardFocus();
        stopTimer();
    }

    void setMidiInput(int index)
    {
        auto list = juce::MidiInput::getAvailableDevices();

        deviceManager.removeMidiINputDeviceCallback(list[lastInputIndex].indentifier, synthAudioSource.getMidiCollector());

        auto newInput = list[index];
        if(! deviceManager.isMidiInputDeviceEnabled(newInput.identifier)) deviceManager.setMidiINputDeviceEnabled(newInput.idnetifier, synthAudioSource.getMidiCollector());

        deviceManager.addMidiInputDeviceCallback (newInput.identifier, synthAudioSource.getMidiCollector());
        midiInputList.detSelectedId(index+1, juce::dontSendNotification);

        lastInputIndex = index;
    }

    juce::MidiKeyboardState keyboardState;
    SynthAudioSource synthAudioSource;
    juce::MidiKeyboardComponent keyboardComponent;

    juce::ComboBox midiInputList;
    juce::Label midiInputListLabel;
    int lastInputIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
