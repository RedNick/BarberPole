// knob_sequencer - plays the Barber Pole test sequence on a MIDI CC so Traktor and
// VirtualDJ get identical, precisely timed knob moves.
//
// Windows: sends through the built-in Windows MIDI API (winmm). Pair it with a virtual
//          MIDI cable such as loopMIDI so Traktor / VirtualDJ can receive it.
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
//   --log <file>              timing log (default knob_sequence_log.csv)
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
// MIDI output (Windows winmm, or dry run)
class MidiOut {
public:
    ~MidiOut() { close(); }

    static std::vector<std::string> ports() {
        std::vector<std::string> out;
#ifdef _WIN32
        const UINT n = midiOutGetNumDevs();
        for (UINT i = 0; i < n; ++i) {
            MIDIOUTCAPSA caps;
            if (midiOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) out.push_back(caps.szPname);
            else out.push_back("(unavailable)");
        }
#else
        out.push_back("dry-run (prints messages; real MIDI needs the Windows build)");
#endif
        return out;
    }

    // spec: a port number, or part of a port name (case-insensitive)
    bool open(const std::string &spec) {
        const std::vector<std::string> names = ports();
        int idx = -1;
        char *end = nullptr;
        const long asNum = std::strtol(spec.c_str(), &end, 10);
        if (!spec.empty() && *end == '\0') {
            if (asNum >= 0 && asNum < (long)names.size()) idx = (int)asNum;
        } else {
            for (size_t i = 0; i < names.size() && idx < 0; ++i)
                if (lower(names[i]).find(lower(spec)) != std::string::npos) idx = (int)i;
        }
#ifndef _WIN32
        if (idx < 0) idx = 0;   // dry run always "opens"
#endif
        if (idx < 0) {
            std::fprintf(stderr, "No MIDI output port matches \"%s\". Ports found:\n", spec.c_str());
            for (size_t i = 0; i < names.size(); ++i) std::fprintf(stderr, "  %zu: %s\n", i, names[i].c_str());
            if (names.empty()) std::fprintf(stderr, "  (none - is loopMIDI running?)\n");
            return false;
        }
        name_ = names[(size_t)idx];
#ifdef _WIN32
        if (midiOutOpen(&h_, (UINT)idx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            std::fprintf(stderr, "Could not open MIDI port \"%s\" (in use by another program?)\n", name_.c_str());
            h_ = nullptr;
            return false;
        }
#endif
        return true;
    }

    void cc(int channel, int controller, int value) {
        value = value < 0 ? 0 : (value > 127 ? 127 : value);
#ifdef _WIN32
        const DWORD msg = (DWORD)(0xB0 | ((channel - 1) & 0x0F)) | ((DWORD)(controller & 0x7F) << 8) |
                          ((DWORD)(value & 0x7F) << 16);
        if (h_) midiOutShortMsg(h_, msg);
#else
        if (verbose) std::printf("    [dry-run] ch %d CC %d = %d\n", channel, controller, value);
#endif
    }

    void close() {
#ifdef _WIN32
        if (h_) { midiOutReset(h_); midiOutClose(h_); h_ = nullptr; }
#endif
    }

    const std::string &name() const { return name_; }
    bool verbose = false;

private:
    static std::string lower(std::string s) {
        for (auto &c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }
    std::string name_;
#ifdef _WIN32
    HMIDIOUT h_ = nullptr;
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

static int toCC(double knob) { return (int)std::lround(knob * 127.0); }

// Knob positions: 0 = full left, 0.5 = centre (12 o'clock), 1 = full right.
// MIDI 64 is the centre value (64/127 = 0.504, inside the plugin's +-3% dead zone).
static std::vector<Step> buildSequence(double hold) {
    const double g = 0.5;   // glide time between positions (s)
    std::vector<Step> s = {
        {"centre",        64.0 / 127.0, 0.0, hold},
        {"right 1/4",     0.625, g, hold},
        {"right 1/2",     0.750, g, hold},
        {"right 3/4",     0.875, g, hold},
        {"right full",    1.000, g, hold},
        {"centre",        64.0 / 127.0, g, 4.0},
        {"left 1/4",      0.375, g, hold},
        {"left 1/2",      0.250, g, hold},
        {"left 3/4",      0.125, g, hold},
        {"left full",     0.000, g, hold},
        {"centre",        64.0 / 127.0, g, 4.0},
        // slow sweeps through the whole range: shows the full cutoff curve
        {"sweep to right full", 1.000, 16.0, 1.0},
        {"centre",        64.0 / 127.0, 2.0, 2.0},
        {"sweep to left full",  0.000, 16.0, 1.0},
        {"centre",        64.0 / 127.0, 2.0, 2.0},
    };
    return s;
}

class Clock {
public:
    explicit Clock(bool fast) : fast_(fast), t0_(std::chrono::steady_clock::now()) {}
    double now() const {
        if (fast_) return virt_;
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    }
    void sleepUntil(double t) {
        if (fast_) { if (t > virt_) virt_ = t; return; }
        for (;;) {
            const double left = t - now();
            if (left <= 0) return;
            std::this_thread::sleep_for(std::chrono::duration<double>(left > 0.02 ? left - 0.01 : left));
        }
    }
private:
    bool fast_;
    std::chrono::steady_clock::time_point t0_;
    double virt_ = 0.0;
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

static bool parseOptions(int argc, char **argv, int first, Options &o) {
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char *what) -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); return nullptr; }
            return argv[++i];
        };
        const char *v = nullptr;
        if (a == "--port") { if (!(v = need("--port"))) return false; o.port = v; }
        else if (a == "--channel") { if (!(v = need("--channel"))) return false; o.channel = std::atoi(v); }
        else if (a == "--cc") { if (!(v = need("--cc"))) return false; o.cc = std::atoi(v); }
        else if (a == "--hold") { if (!(v = need("--hold"))) return false; o.hold = std::atof(v); }
        else if (a == "--countdown") { if (!(v = need("--countdown"))) return false; o.countdown = std::atof(v); }
        else if (a == "--log") { if (!(v = need("--log"))) return false; o.log = v; }
        else if (a == "--fast") o.fast = true;
        else { std::fprintf(stderr, "Unknown option: %s\n", a.c_str()); return false; }
    }
    if (o.channel < 1 || o.channel > 16) { std::fprintf(stderr, "--channel must be 1-16\n"); return false; }
    if (o.cc < 0 || o.cc > 127) { std::fprintf(stderr, "--cc must be 0-127\n"); return false; }
    if (o.hold < 1 || o.hold > 120) { std::fprintf(stderr, "--hold must be 1-120 seconds\n"); return false; }
    if (o.countdown < 0 || o.countdown > 60) { std::fprintf(stderr, "--countdown must be 0-60 seconds\n"); return false; }
    return true;
}

static int cmdLearn(MidiOut &m, const Options &o) {
    std::printf("Sending a slow wiggle on channel %d CC %d for 20 s.\n"
                "Click Learn on the knob you want in Traktor / VirtualDJ now.\n", o.channel, o.cc);
    Clock clk(o.fast);
    const double dur = 20.0, rate = 50.0;   // 50 messages per second
    for (int k = 0; k <= (int)(dur * rate); ++k) {
        const double t = k / rate;
        const double knob = 0.5 + 0.45 * std::sin(2.0 * 3.14159265358979 * t / 4.0);   // 4 s per wiggle
        clk.sleepUntil(t);
        m.cc(o.channel, o.cc, toCC(knob));
    }
    m.cc(o.channel, o.cc, 64);
    std::printf("Done - knob left at centre (64).\n");
    return 0;
}

static int cmdRun(MidiOut &m, const Options &o) {
    const std::vector<Step> seq = buildSequence(o.hold);
    double total = o.countdown;
    for (const Step &s : seq) total += s.glide + s.hold;

    FILE *log = std::fopen(o.log.c_str(), "w");
    if (!log) std::fprintf(stderr, "Warning: could not write log file %s\n", o.log.c_str());
    else std::fprintf(log, "step,label,start_s,end_s,knob,midi_value\n");

    std::printf("Port: %s   channel %d   CC %d\n", m.name().c_str(), o.channel, o.cc);
    std::printf("Sequence length %.0f s (the test file is 160 s). Start playback and recording now.\n", total);

    Clock clk(o.fast);
    m.cc(o.channel, o.cc, 64);   // make sure we start at centre
    for (int c = (int)std::ceil(o.countdown); c > 0; --c) {
        std::printf("  starting in %d...\n", c);
        std::fflush(stdout);
        clk.sleepUntil(o.countdown - c + 1.0);
    }
    clk.sleepUntil(o.countdown);

    double t = o.countdown;
    double knob = 64.0 / 127.0;
    int lastSent = 64;
    const double rate = 100.0;   // glide messages per second
    for (size_t i = 0; i < seq.size(); ++i) {
        const Step &s = seq[i];
        const double start = t;
        std::printf("[%6.1f s] %-20s -> MIDI %3d\n", start - o.countdown, s.label, toCC(s.target));
        std::fflush(stdout);
        // glide (linear in knob position), sending only when the 7-bit value changes
        const int steps = (int)std::ceil(s.glide * rate);
        for (int k = 1; k <= steps; ++k) {
            const double tk = t + s.glide * k / steps;
            clk.sleepUntil(tk);
            const double v = knob + (s.target - knob) * (double)k / steps;
            const int mv = toCC(v);
            if (mv != lastSent) { m.cc(o.channel, o.cc, mv); lastSent = mv; }
        }
        t += s.glide;
        knob = s.target;
        if (toCC(knob) != lastSent || i == 0) { m.cc(o.channel, o.cc, toCC(knob)); lastSent = toCC(knob); }
        t += s.hold;
        clk.sleepUntil(t);
        if (log) std::fprintf(log, "%zu,%s,%.3f,%.3f,%.4f,%d\n", i, s.label, start - o.countdown,
                              t - o.countdown, s.target, toCC(s.target));
    }
    m.cc(o.channel, o.cc, 64);
    if (log) std::fclose(log);
    std::printf("[%6.1f s] done - knob back at centre. Timing log: %s\n", t - o.countdown, o.log.c_str());
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const std::string cmd = argv[1];
#ifdef _WIN32
    timeBeginPeriod(1);   // 1 ms timer resolution for accurate timing
#endif
    int rc = 1;
    if (cmd == "list") {
        const std::vector<std::string> p = MidiOut::ports();
        if (p.empty()) std::printf("No MIDI output ports found. Is loopMIDI running?\n");
        for (size_t i = 0; i < p.size(); ++i) std::printf("  %zu: %s\n", i, p[i].c_str());
        rc = 0;
    } else if (cmd == "learn" || cmd == "run" || cmd == "set") {
        Options o;
        int first = 2;
        int setValue = -1;
        if (cmd == "set") {
            if (argc < 3) { usage(); return 1; }
            setValue = std::atoi(argv[2]);
            first = 3;
        }
        if (!parseOptions(argc, argv, first, o)) { usage(); return 1; }
        MidiOut m;
        m.verbose = o.fast && cmd != "learn";
        if (!m.open(o.port)) return 1;
        if (cmd == "learn") rc = cmdLearn(m, o);
        else if (cmd == "run") rc = cmdRun(m, o);
        else { m.cc(o.channel, o.cc, setValue); std::printf("Sent %d on channel %d CC %d (%s)\n", setValue, o.channel, o.cc, m.name().c_str()); rc = 0; }
        m.close();
    } else {
        usage();
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return rc;
}
