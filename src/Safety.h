// Safety decisions, kept free of hardware and UI so they can be tested with simulated failures
// (tests/Tests.cpp). The app calls these from its control loops and carries out the result.
#pragma once
#include <cstdint>

namespace safety {

constexpr unsigned kWatchdogC = 95;     // restore the vendor curve at this CPU/GPU temperature
constexpr int kTelemetryLossTicks = 5;  // consecutive control ticks without a temperature reading
constexpr int kEcErrorLimit = 5;        // consecutive EC errors before the custom curve is turned off

enum class AcerAction { None, RestoreHot, RestoreNoReadings };

// Acer backend, once per tick while a custom curve is enabled. Only a curve that cools less than
// Acer's ("quieter") is guarded: it is restored at kWatchdogC, or when temperature readings have been
// missing for kTelemetryLossTicks ticks in a row. `missingTicks` carries the count between calls.
inline AcerAction AcerWatchdog(int& missingTicks, bool applied, bool quieter, bool readingsOk, unsigned cpuC, unsigned gpuC) {
    if (!applied || !quieter) {
        missingTicks = 0;
        return AcerAction::None;
    }
    if (!readingsOk) {
        if (++missingTicks < kTelemetryLossTicks) return AcerAction::None;
        missingTicks = 0;
        return AcerAction::RestoreNoReadings;
    }
    missingTicks = 0;
    return (cpuC >= kWatchdogC || gpuC >= kWatchdogC) ? AcerAction::RestoreHot : AcerAction::None;
}

enum class NbfcAction { Run, Wait, HandBack };

// NBFC backend: the app runs the curve itself, so without a temperature it must not keep driving the
// fans. Waits a few ticks for the reading to return, then hands the fans back to the firmware.
inline NbfcAction NbfcTemperature(int& missingTicks, bool readingOk) {
    if (readingOk) {
        missingTicks = 0;
        return NbfcAction::Run;
    }
    return ++missingTicks >= kTelemetryLossTicks ? NbfcAction::HandBack : NbfcAction::Wait;
}

// Counts consecutive EC errors (a success resets the count). True once the limit is reached;
// the count then starts over.
inline bool EcErrorLimitReached(int& errors, bool failed) {
    if (!failed) {
        errors = 0;
        return false;
    }
    if (++errors < kEcErrorLimit) return false;
    errors = 0;
    return true;
}

// Acer backend: write the custom table again when it is not known to be applied, or when the EC
// reloaded its own table (the canary byte no longer holds our value).
inline bool NeedsReapply(bool applied, std::uint8_t canaryRead, std::uint8_t canaryExpected) {
    return !applied || canaryRead != canaryExpected;
}

// First check during Confirm on an untested Acer model: row 0 of a fan table is
// [0, flags, cpuUp, cpuDown, gpuUp, gpuDown, ...] with plausible temperatures. If the first 10 bytes
// do not look like that, Confirm stops before reading anything else.
inline bool LooksLikeFanTableRow0(const std::uint8_t* row) {
    if (row[0] != 0) return false;
    for (int fan = 0; fan < 2; ++fan) {
        const int up = row[2 + 2 * fan], down = row[3 + 2 * fan];
        if (up < 10 || up >= 0x7f || down > up) return false;
    }
    return true;
}

}  // namespace safety
