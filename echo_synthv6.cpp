#include <iostream>
#include <cmath>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <vector>
#include <portaudio.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>

constexpr double PI = 3.14159265358979323846;
constexpr int SAMPLE_RATE = 44100;
constexpr float VOLUME = 0.65f;
constexpr int NUM_VOICES = 121;
constexpr int DELAY_SIZE = SAMPLE_RATE * 2;
constexpr float REVERB_MIX = 0.5f;  // Adjust reverb intensity (can change)
constexpr float REVERB_DECAY = 0.5f;  // Feedback decay for reverb

// Envelope timings
constexpr float ATTACK_TIME = 0.01f;
constexpr float DECAY_TIME = 0.1f;
constexpr float SUSTAIN_LEVEL = 0.8f;
constexpr float RELEASE_TIME = 0.5f;

std::unordered_map<char, int> keyNoteMap = {
    {'A', 57}, {'W', 58}, {'S', 59}, {'E', 60}, {'D', 61}, {'F', 62}, {'T', 63}, {'G', 64},
    {'Y', 65}, {'H', 66}, {'U', 67}, {'J', 68}, {'K', 69}, {'O', 70}, {'L', 71}, {'P', 72},
    {';', 73}, {'\'', 74}, {'Q', 72}, {'Z', 48}, {'X', 50}, {'C', 52}, {'V', 53}, {'B', 55}, {'N', 57}
};

std::unordered_map<int, std::atomic<bool>> activeNotes;
std::atomic<bool> running(true);

// Envelope stage definition
enum EnvelopeStage { OFF, ATTACK, DECAY, SUSTAIN, RELEASE };

struct Voice {
    int note = 0;
    double phase = 0;
    double time = 0;
    EnvelopeStage stage = OFF;
    float envelope = 0.0f;
    bool keyDown = false;
};

std::unordered_map<int, Voice> voices;
std::vector<float> reverbDelayBuffer(DELAY_SIZE, 0.0f);  // Reverb buffer
int reverbIndex = 0;
float reverbFeedback = REVERB_DECAY;  // Feedback decay for reverb

double midiToFreq(int midiNote) {
    return 440.0 * pow(2.0, (midiNote - 69) / 12.0);
}

// Analog saw wave generator with saturation (using tanh for warmth)
float getSaw(double t, double freq) {
    // Generate a saw wave
    double rawSample = 2.0 * (t * freq - floor(t * freq + 0.5));
    
    // Apply saturation effect
    return (float)tanh(rawSample * 6.0);  // Increase the multiplier for more saturation (increase 'fatness')
}

static int audioCallback(const void*, void* outputBuffer,
                         unsigned long framesPerBuffer,
                         const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    float* out = static_cast<float*>(outputBuffer);
    static double t = 0;
    double dt = 1.0 / SAMPLE_RATE;

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        double mixed = 0.0;
        int active = 0;

        for (auto& noteVoice : voices) {  // Using iterator instead of structured bindings for compatibility
            auto& voice = noteVoice.second;
            if (voice.stage == OFF) continue;

            double freq = midiToFreq(voice.note);
            voice.time += dt;

            // Envelope processing
            switch (voice.stage) {
                case ATTACK:
                    voice.envelope += 1.0f / (ATTACK_TIME * SAMPLE_RATE);
                    if (voice.envelope >= 1.0f) {
                        voice.envelope = 1.0f;
                        voice.stage = DECAY;
                    }
                    break;
                case DECAY:
                    voice.envelope -= (1.0f - SUSTAIN_LEVEL) / (DECAY_TIME * SAMPLE_RATE);
                    if (voice.envelope <= SUSTAIN_LEVEL) {
                        voice.envelope = SUSTAIN_LEVEL;
                        voice.stage = SUSTAIN;
                    }
                    break;
                case SUSTAIN:
                    break;
                case RELEASE:
                    voice.envelope -= SUSTAIN_LEVEL / (RELEASE_TIME * SAMPLE_RATE);
                    if (voice.envelope <= 0.0f) {
                        voice.envelope = 0.0f;
                        voice.stage = OFF;
                        continue;
                    }
                    break;
                default:
                    break;
            }

            double phase = voice.time;
            double sample = 0.0;

            // 777 voices, detuned saws
            for (int i = -13; i < 14; ++i) {
                sample += getSaw(phase, freq + i * 0.005); // Slight detuning for thickness
            }

            sample /= NUM_VOICES;
            mixed += sample * voice.envelope;
            active++;
        }

        if (active > 0) mixed = (mixed / active) * VOLUME;

        // Reverb processing
        // Add feedback from reverb buffer for the reverb effect
        float reverbSample = reverbDelayBuffer[reverbIndex] * reverbFeedback;
        mixed += reverbSample;

        // Store the current sample into the reverb delay buffer
        reverbDelayBuffer[reverbIndex] = mixed;

        // Update reverb index
        reverbIndex = (reverbIndex + 1) % DELAY_SIZE;

        // Output the final mixed signal
        *out++ = mixed;
        *out++ = mixed;  // Stereo output

        t += dt;
    }

    return paContinue;
}

void keyboardListener() {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    while (running) {
        char c = getchar();
        c = toupper(c);

        if (c == 27) {
            running = false;
        } else if (keyNoteMap.count(c)) {
            int note = keyNoteMap[c];
            auto& voice = voices[note];
            voice.note = note;
            voice.stage = ATTACK;
            voice.keyDown = true;
            voice.time = 0.0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        for (auto& [note, voice] : voices) {
            if (voice.keyDown && !keyNoteMap.count(c)) {
                voice.stage = RELEASE;
                voice.keyDown = false;
            }
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

int main() {
    Pa_Initialize();

    PaStream* stream;
    Pa_OpenDefaultStream(&stream, 0, 2, paFloat32, SAMPLE_RATE, 256, audioCallback, nullptr);
    Pa_StartStream(stream);

    std::thread listener(keyboardListener);

    std::cout << "Analog Synth Ready!\n";
    std::cout << "Q = C5 (FL Studio layout)\n";
    std::cout << "Press ESC to quit\n";

    while (running)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    listener.join();
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    Pa_Terminate();
    return 0;
}
