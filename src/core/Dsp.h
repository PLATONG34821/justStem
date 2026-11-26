#pragma once

#include <vector>

/** Signal-processing building blocks shared by the key, tempo and chord detectors. */
namespace juststem::dsp
{
std::vector<float> hann (int n);

/** Magnitudes of bins 0..n/2 of a power-of-two real frame x[0..n-1]. */
std::vector<float> spectrumMagnitudes (const std::vector<float>& frame);

struct Peak
{
    float freq, mag;
};

struct PeakFrames
{
    std::vector<std::vector<Peak>> frames; // frame i starts at sample i * hop; empty when silent
    int size = 0, hop = 0;
    float tuning = 0;                      // offset from A = 440 Hz in semitones (-0.5 .. 0.5)
};

/** Spectral peaks (parabolic-interpolated, >= 1 % of the frame's loudest) between minF and maxF,
    plus the tuning they share. */
PeakFrames spectralPeaks (const std::vector<float>& x, int rate, int size, int hop, float minF, float maxF);

/** Onset strength: half-wave rectified flux of 40 log band energies, minus its 1 s local mean.
    One value per onsetHop samples. */
constexpr int onsetSize = 2048, onsetHop = 512;
std::vector<float> onsetEnvelope (const std::vector<float>& x, int rate);
} // namespace juststem::dsp
