// knob_sequencer: plays the Barber Pole test sequence on a MIDI CC, so Traktor and VirtualDJ
// get identical, precisely timed knob moves to record.
//
// Windows: sends through the built-in Windows MIDI API (winmm), by default to Windows' own
//          "Default App Loopback (A)" port (a virtual MIDI cable such as loopMIDI works too).
// Other platforms: builds as a dry run that prints what it would send (for testing).
//
// Usage:
//   knob_sequencer list                      list MIDI output ports
//   knob_sequencer learn [options]           wiggle the knob for 20 s (for MIDI Learn)
//   knob_sequencer run   [options]           countdown, then the full test sequence
//   knob_sequencer set <0-127> [options]     send a single value (e.g. 64 = centre)
//
// Options:
//   --port <name-or-number>   output port (default: "Default App Loopback (A)", the Windows MIDI loopback)
//   --channel <1-16>          MIDI channel (default 1)
//   --cc <0-127>              controller number (default 20)
//   --hold <seconds>          hold time per knob position (default 8)
//   --countdown <seconds>     countdown before the sequence starts (default 5)
//   --log <file>              timing log, read by compare_recordings (default knob_sequence_log.csv)
//   --fast                    dry-run/test only: don't actually wait

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#endif

// ---------------------------------------------------------------------------
// MIDI output (Windows winmm, or a dry run that prints instead)
class MidiOut {
public:
    ~MidiOut() { close(); }

    static std::vector<std::string> ports() {
        std::vector<std::string> names;
#ifdef _WIN32
        const UINT count = midiOutGetNumDevs();
        for (UINT i = 0; i < count; ++i) {
            MIDIOUTCAPSA caps;
            if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) names.push_back(caps.szPname);
            else names.push_back("(unavailable)");
        }
#else
        names.push_back("dry-run (prints messages; real MIDI needs the Windows build)");
#endif
        return names;
    }

    // portSpec: a port number, or part of a port name (case-insensitive)
    bool open(const std::string &portSpec) {
        const std::vector<std::string> names = ports();
        int index = -1;
        char *end = nullptr;
        const long asNumber = std::strtol(portSpec.c_str(), &end, 10);
        if (!portSpec.empty() && *end == '\0') {
            if (asNumber >= 0 && asNumber < (long)names.size()) index = (int)asNumber;
        } else {
            for (size_t i = 0; i < names.size() && index < 0; ++i)
                if (lower(names[i]).find(lower(portSpec)) != std::string::npos) index = (int)i;
        }
#ifndef _WIN32
        if (index < 0) index = 0;   // the dry run always "opens"
#endif
        if (index < 0) {
            std::fprintf(stderr, "No MIDI output port matches \"%s\". Ports found:\n", portSpec.c_str());
            for (size_t i = 0; i < names.size(); ++i) std::fprintf(stderr, "  %zu: %s\n", i, names[i].c_str());
            if (names.empty()) std::fprintf(stderr, "  (none - is loopMIDI running?)\n");
            return false;
        }
        name_ = names[(size_t)index];
#ifdef _WIN32
        if (midiOutOpen(&handle_, (UINT)index, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            std::fprintf(stderr, "Could not open MIDI port \"%s\" (in use by another program?)\n", name_.c_str());
            handle_ = nullptr;
            return false;
        }
#endif
        return true;
    }

    // Sends a Control Change message. channel 1..16, controller and value 0..127.
    void controlChange(int channel, int controller, int value) {
        value = value < 0 ? 0 : (value > 127 ? 127 : value);
#ifdef _WIN32
        // short message: status byte (0xB0 = control change | channel), controller, value
        const DWORD message = (DWORD)(0xB0 | ((channel - 1) & 0x0F)) | ((DWORD)(controller & 0x7F) << 8) |
                              ((DWORD)(value & 0x7F) << 16);
        if (handle_) midiOutShortMsg(handle_, message);
#else
        if (verbose) std::printf("    [dry-run] ch %d CC %d = %d\n", channel, controller, value);
#endif
    }

    void close() {
#ifdef _WIN32
        if (handle_) { midiOutReset(handle_); midiOutClose(handle_); handle_ = nullptr; }
#endif
    }

    const std::string &name() const { return name_; }
    bool verbose = false;   // dry run: print every message

private:
    static std::string lower(std::string text) {
        for (auto &c : text) c = (char)std::tolower((unsigned char)c);
        return text;
    }
    std::string name_;
#ifdef _WIN32
    HMIDIOUT handle_ = nullptr;
#endif
};

// ---------------------------------------------------------------------------
struct Options {
    std::string port = "Default App Loopback (A)";
    int channel = 1;
    int cc = 20;
    double hold = 8.0;
    double countdown = 5.0;
    std::string log = "knob_sequence_log.csv";
    bool fast = false;
};

// One step of the sequence: glide to `target` (0..1 knob position) over `glide` seconds,
// then hold for `hold` seconds. `label` names the position in the log.
struct Step {
    const char *label;
    double target;
    double glide;
    double hold;
};

// knob position 0..1 -> 7-bit MIDI value
static int toMidiValue(double knob) { return (int)std::lround(knob * 127.0); }

// Knob positions: 0 = full left, 0.5 = centre (12 o'clock), 1 = full right.
// MIDI 64 is the centre value (64/127 = 0.504, inside the plugin's +-3 % dead zone).
static std::vector<Step> buildSequence(double hold) {
    const double glide = 0.5;   // seconds between positions
    const double centre = 64.0 / 127.0;
    std::vector<Step> steps = {
        {"centre",        centre, 0.0, hold},   // untouched reference for compare_recordings
        {"right 1/4",     0.625, glide, hold},
        {"right 1/2",     0.750, glide, hold},
        {"right 3/4",     0.875, glide, hold},
        {"right full",    1.000, glide, hold},
        {"centre",        centre, glide, 4.0},
        {"left 1/4",      0.375, glide, hold},
        {"left 1/2",      0.250, glide, hold},
        {"left 3/4",      0.125, glide, hold},
        {"left full",     0.000, glide, hold},
        {"centre",        centre, glide, 4.0},
        // slow sweeps through the whole range: shows the full cutoff curve
        {"sweep to right full", 1.000, 16.0, 1.0},
        {"centre",        centre, 2.0, 2.0},
        {"sweep to left full",  0.000, 16.0, 1.0},
        {"centre",        centre, 2.0, 2.0},
    };
    return steps;
}

// Seconds since construction. In fast (dry-run) mode, time is virtual and sleeps are instant.
class Clock {
public:
    explicit Clock(bool fast) : fast_(fast), start_(std::chrono::steady_clock::now()) {}
    double now() const {
        if (fast_) return virtualTime_;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    }
    void sleepUntil(double seconds) {
        if (fast_) { if (seconds > virtualTime_) virtualTime_ = seconds; return; }
        for (;;) {
            const double remaining = seconds - now();
            if (remaining <= 0) return;
            // sleep most of the way, then the rest in short steps for accuracy
            std::this_thread::sleep_for(std::chrono::duration<double>(remaining > 0.02 ? remaining - 0.01 : remaining));
        }
    }
private:
    bool fast_;
    std::chrono::steady_clock::time_point start_;
    double virtualTime_ = 0.0;
};

static void usage() {
    std::printf(
        "knob_sequencer - Barber Pole test knob over MIDI\n\n"
        "  knob_sequencer list                   list MIDI output ports\n"
        "  knob_sequencer learn [options]        wiggle the knob for 20 s (for MIDI Learn)\n"
        "  knob_sequencer run   [options]        countdown, then the full test sequence\n"
        "  knob_sequencer set <0-127> [options]  send one value (64 = centre)\n\n"
        "Options: --port <name|number> (default Default App Loopback (A))  --channel <1-16> (default 1)\n"
        "         --cc <0-127> (default 20)  --hold <s> (default 8)  --countdown <s> (default 5)\n"
        "         --log <file> (default knob_sequence_log.csv)\n");
}

static bool parseOptions(int argc, char **argv, int firstArg, Options &options) {
    for (int i = firstArg; i < argc; ++i) {
        const std::string arg = argv[i];
        auto nextValue = [&](const char *option) -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", option); return nullptr; }
            return argv[++i];
        };
        const char *value = nullptr;
        if (arg == "--port") { if (!(value = nextValue("--port"))) return false; options.port = value; }
        else if (arg == "--channel") { if (!(value = nextValue("--channel"))) return false; options.channel = std::atoi(value); }
        else if (arg == "--cc") { if (!(value = nextValue("--cc"))) return false; options.cc = std::atoi(value); }
        else if (arg == "--hold") { if (!(value = nextValue("--hold"))) return false; options.hold = std::atof(value); }
        else if (arg == "--countdown") { if (!(value = nextValue("--countdown"))) return false; options.countdown = std::atof(value); }
        else if (arg == "--log") { if (!(value = nextValue("--log"))) return false; options.log = value; }
        else if (arg == "--fast") options.fast = true;
        else { std::fprintf(stderr, "Unknown option: %s\n", arg.c_str()); return false; }
    }
    if (options.channel < 1 || options.channel > 16) { std::fprintf(stderr, "--channel must be 1-16\n"); return false; }
    if (options.cc < 0 || options.cc > 127) { std::fprintf(stderr, "--cc must be 0-127\n"); return false; }
    if (options.hold < 1 || options.hold > 120) { std::fprintf(stderr, "--hold must be 1-120 seconds\n"); return false; }
    if (options.countdown < 0 || options.countdown > 60) { std::fprintf(stderr, "--countdown must be 0-60 seconds\n"); return false; }
    return true;
}

// Slow sine wiggle around centre, so a host's MIDI Learn can pick up the controller.
static int runLearn(MidiOut &midi, const Options &options) {
    std::printf("Sending a slow wiggle on channel %d CC %d for 20 s.\n"
                "Click Learn on the knob you want in Traktor / VirtualDJ now.\n", options.channel, options.cc);
    Clock clock(options.fast);
    const double duration = 20.0, messagesPerSecond = 50.0;
    for (int k = 0; k <= (int)(duration * messagesPerSecond); ++k) {
        const double t = k / messagesPerSecond;
        const double knob = 0.5 + 0.45 * std::sin(2.0 * 3.14159265358979 * t / 4.0);   // 4 s per wiggle
        clock.sleepUntil(t);
        midi.controlChange(options.channel, options.cc, toMidiValue(knob));
    }
    midi.controlChange(options.channel, options.cc, 64);
    std::printf("Done - knob left at centre (64).\n");
    return 0;
}

// Countdown, then the full sequence, writing the timing log as it goes.
static int runSequence(MidiOut &midi, const Options &options) {
    const std::vector<Step> sequence = buildSequence(options.hold);
    double totalSeconds = options.countdown;
    for (const Step &step : sequence) totalSeconds += step.glide + step.hold;

    FILE *log = std::fopen(options.log.c_str(), "w");
    if (!log) std::fprintf(stderr, "Warning: could not write log file %s\n", options.log.c_str());
    else std::fprintf(log, "step,label,start_s,end_s,knob,midi_value\n");

    std::printf("Port: %s   channel %d   CC %d\n", midi.name().c_str(), options.channel, options.cc);
    std::printf("Sequence length %.0f s (the test file is 160 s). Start playback and recording now.\n", totalSeconds);

    Clock clock(options.fast);
    midi.controlChange(options.channel, options.cc, 64);   // make sure we start at centre
    for (int remaining = (int)std::ceil(options.countdown); remaining > 0; --remaining) {
        std::printf("  starting in %d...\n", remaining);
        std::fflush(stdout);
        clock.sleepUntil(options.countdown - remaining + 1.0);
    }
    clock.sleepUntil(options.countdown);

    // times below are clock times; the log records them relative to the sequence start
    double t = options.countdown;
    double knob = 64.0 / 127.0;
    int lastSent = 64;
    const double glideMessagesPerSecond = 100.0;
    for (size_t i = 0; i < sequence.size(); ++i) {
        const Step &step = sequence[i];
        const double stepStart = t;
        std::printf("[%6.1f s] %-20s -> MIDI %3d\n", stepStart - options.countdown, step.label, toMidiValue(step.target));
        std::fflush(stdout);
        // glide (linear in knob position), sending only when the 7-bit value changes
        const int glideMessages = (int)std::ceil(step.glide * glideMessagesPerSecond);
        for (int k = 1; k <= glideMessages; ++k) {
            const double messageTime = t + step.glide * k / glideMessages;
            clock.sleepUntil(messageTime);
            const double position = knob + (step.target - knob) * (double)k / glideMessages;
            const int value = toMidiValue(position);
            if (value != lastSent) { midi.controlChange(options.channel, options.cc, value); lastSent = value; }
        }
        t += step.glide;
        knob = step.target;
        if (toMidiValue(knob) != lastSent || i == 0) { midi.controlChange(options.channel, options.cc, toMidiValue(knob)); lastSent = toMidiValue(knob); }
        t += step.hold;
        clock.sleepUntil(t);
        if (log) std::fprintf(log, "%zu,%s,%.3f,%.3f,%.4f,%d\n", i, step.label, stepStart - options.countdown,
                              t - options.countdown, step.target, toMidiValue(step.target));
    }
    midi.controlChange(options.channel, options.cc, 64);
    if (log) std::fclose(log);
    std::printf("[%6.1f s] done - knob back at centre. Timing log: %s\n", t - options.countdown, options.log.c_str());
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const std::string command = argv[1];
#ifdef _WIN32
    timeBeginPeriod(1);   // 1 ms timer resolution for accurate timing
#endif
    int exitCode = 1;
    if (command == "list") {
        const std::vector<std::string> names = MidiOut::ports();
        if (names.empty()) std::printf("No MIDI output ports found. Is loopMIDI running?\n");
        for (size_t i = 0; i < names.size(); ++i) std::printf("  %zu: %s\n", i, names[i].c_str());
        exitCode = 0;
    } else if (command == "learn" || command == "run" || command == "set") {
        Options options;
        int firstOption = 2;
        int setValue = -1;
        if (command == "set") {
            if (argc < 3) { usage(); return 1; }
            setValue = std::atoi(argv[2]);
            firstOption = 3;
        }
        if (!parseOptions(argc, argv, firstOption, options)) { usage(); return 1; }
        MidiOut midi;
        midi.verbose = options.fast && command != "learn";
        if (!midi.open(options.port)) return 1;
        if (command == "learn") exitCode = runLearn(midi, options);
        else if (command == "run") exitCode = runSequence(midi, options);
        else {
            midi.controlChange(options.channel, options.cc, setValue);
            std::printf("Sent %d on channel %d CC %d (%s)\n", setValue, options.channel, options.cc, midi.name().c_str());
            exitCode = 0;
        }
        midi.close();
    } else {
        usage();
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return exitCode;
}
