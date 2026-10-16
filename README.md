# Barber Pole: Traktor-style Color FX for VirtualDJ (v2.4)

This is a "dual-pole" filter effect, like Traktor's Barber Pole Mixer FX.

| Knob position | What you hear |
|---|---|
| **12 o'clock** | Nothing. The output is bit-identical to the input, with a ±3 % dead zone so a jittery knob stays off. |
| **Right** | A high-pass filter rises while notches climb forever. At full right the high-pass sits at about 640 Hz, so the mids and highs keep sweeping. Good for build-ups. |
| **Left** | A low-pass filter falls while notches fall forever. At full left the low-pass sits at about 900 Hz. Good for breakdowns. |
| Further from centre | The cutoff moves further (28 Hz → 640 Hz on the right, 12 kHz → 900 Hz on the left) the sweep fades in over the first third of the turn, and the level drops a little more (about −5 dB at full right, −3.5 dB at full left). The sweep speed stays the same. |

These ranges, speeds and the notch depth were matched to recordings of Traktor's Barber Pole (see "Comparing with Traktor" below). Like Traktor's, it never cuts to silence.

The extra parameters appear in the effect's own panel:

| Parameter | Default | |
|---|---|---|
| **Speed** | 2.8 beats per octave up, 1.8 down | Synced to the track's BPM. Like Traktor's, it climbs a little slower than it falls. The slider scales both by 4× slower to 4× faster. |
| **Resonance** | 40 % | Sets notch sharpness, from soft and wide to narrow and vocal. |

How it works: there are 8 notches spaced one octave apart, all gliding together like a phaser, in two identical stages for extra depth, with a gentle resonant peak half way between each pair (like Traktor's, so the sweep is deep without scooping out the mids). Each notch and peak fades in at the bottom of the range and out at the top (the Shepard/Risset illusion), so the motion never ends and never jumps. The peaks are moderate (at most about +8 dB on a single frequency, and the overall level still drops as you turn). The DJ filter is a 24 dB/oct (4th-order Butterworth) state-variable filter with a gentle resonance bump mid-travel. It uses about 3 % of one CPU core per deck.

## 1. Get the SDK headers

1. Download the official SDK: <https://virtualdj.com/developers/VirtualDJ8_SDK_20211003.zip>
2. Copy **`vdjPlugin8.h`** and **`vdjDsp8.h`** into the `SDK/` folder.

(`test/sdk_stub/` contains stand-ins used only for testing on Linux. Don't build the real plugin with them.)

## 2. Build

**Windows** (Visual Studio 2019/2022 with "Desktop development with C++"):
```
cmake -S . -B build -A x64
cmake --build build --config Release
```
This produces `build\Release\BarberPole.dll`.

**macOS**:
```
cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## 3. Install

Close VirtualDJ, then copy the DLL (it replaces the old version) into:

- Windows: `Documents\VirtualDJ\Plugins64\SoundEffect\`
- Mac (Intel): `Documents/VirtualDJ/Plugins64/SoundEffect/`
- Mac (Apple Silicon): `Documents/VirtualDJ/PluginsArm/SoundEffect/`

## 4. Use it as the Color FX

The Color knob is registered with the SDK's `DeclareParameterColorFX` type, so VirtualDJ treats it as a real Color FX. Right-click the Color FX selector on the mixer and pick **Barber Pole**. If it doesn't appear there, assign it with VDJScript instead:

```
filter_selectcolorfx "Barber Pole"
```

After that, the Filter/Color knob drives it. Don't also add the built-in filter on top: this effect *is* the filter.

## Comparing with Traktor: automated knob test

`tools/knob_sequencer.cpp` plays the same timed knob sequence into both programs over MIDI, so the two recordings line up exactly. Record the same test file in each program, then compare the recordings.

**One-time setup**

1. Install **loopMIDI** (free, by Tobias Erichsen) and create a port. The default name, "loopMIDI Port", is fine. Leave loopMIDI running.
2. Build. The normal `cmake --build build --config Release` now also produces `build\Release\knob_sequencer.exe`.
3. Check the port shows up:
   ```
   build\Release\knob_sequencer.exe list
   ```

**Map the knob in Traktor**

1. Go to Preferences → Controller Manager → Add… → Generic MIDI. Set **In-Port** to `loopMIDI Port`.
2. Click Add In… → Mixer → Filter → **Filter Adjust**. Set Assignment to Deck A, Type of Controller to Fader/Knob, and Interaction Mode to Direct.
3. Click **Learn**, then run `knob_sequencer.exe learn`. Stop learning once Traktor shows `Ch01.CC.020`.
4. On Deck A's mixer channel, select **Barber Pole** as the Mixer FX and switch Mixer FX on. The FILTER knob then drives it.

**Map the knob in VirtualDJ**

1. Go to Settings → Controllers. The loopMIDI port appears as a MIDI device.
2. Click **Learn**, run `knob_sequencer.exe learn`, and give it the action `deck 1 filter`.
3. Select **Barber Pole** as deck 1's Color FX (see section 4).
4. VirtualDJ and Traktor shouldn't be listening to the same port at the same time. Close one before testing the other, or make a second loopMIDI port and use `--port`.

**Run the test** (once per program)

1. Load `barberpole_test_126bpm.wav` on deck 1 / A. Check the BPM shows 126, and keep pitch at 0 %.
2. Start the program's recorder, then press play.
3. Run:
   ```
   knob_sequencer.exe run
   ```
   It counts down 5 s, then moves the knob through: centre, ¼, ½, ¾ and full right, then back through centre to ¼, ½, ¾ and full left. It holds each position for 8 s and finishes with slow full-range sweeps each way. That's 127 s in total, which fits inside the 160 s test file.
4. Stop the recorder. Save the recordings in this folder, along with `knob_sequence_log.csv` (the timing log).
5. Compare them (from this folder):
   ```
   build\Release\compare_recordings.exe "Traktor recording.wav" "VirtualDJ recording.flac" --log build\Release\knob_sequence_log.csv
   ```
   It follows each recording through `barberpole_test_126bpm.wav`, so gaps are handled (VirtualDJ's recorder skips silence). Add `--timeline` for a second-by-second view.

Options: `--port <name|number>`, `--channel 1-16`, `--cc 0-127` (default 20), `--hold <s>`, `--countdown <s>`. `knob_sequencer.exe set 64` puts the knob back at centre.

## Files

- `src/barberpole_dsp.h`: the DSP core, plain C++ with no SDK dependency
- `src/BarberPole.h/.cpp`: the VirtualDJ wrapper (`IVdjPluginDsp8`)
- `test/analyze.py` + `test/capi.cpp`: tests for exact bypass and dead zone, filter direction and strength, sweep direction and tempo sync, loudness, no harsh boosts, no clicks when crossing centre, and stability at 44.1/48/96 kHz
- `test/host_sim.cpp`: loads the built plugin through `DllGetClassObject` like a host would
- `tools/knob_sequencer.cpp`: MIDI knob sequencer for the Traktor comparison (Windows)
- `tools/compare_recordings.cpp`: compares two recordings of the knob test position by position (Windows; reads WAV and FLAC)
- The audio files below aren't in the git repository (they're large). Keep them next to the code locally.
- `barberpole_test_126bpm.wav`: 126 BPM pink-noise test file
- `demo_barberpole.wav`: a 126 BPM loop. It's off for 3 s, then the knob turns right slowly (build), returns to centre, then turns left (breakdown).
- `demo_spectrogram.png`: a picture of the notches climbing and falling
