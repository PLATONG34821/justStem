#pragma once

// Shared by juststem_cli's commands (cli.cpp) and its self-tests (selftest.cpp).
#include "../src/core/Separator.h"

#include <string>

/** Developer tools keep their own CoreML cache, so they don't add copies to the plug-in's. */
std::string devCoremlCache();

/** A separation's progress, on one line of stderr. */
void printProgress (const juststem::SeparationProgress&);

/** `selftest`: separates a synthetic track and scores each stem against the part it came from. */
int separationSelftest (const std::string& model, int seconds, float overlap);

/** `analyze-selftest`: the resampler, loudness, key, tempo, beats, chords and pitch shifting. */
int analysisSelftest();
