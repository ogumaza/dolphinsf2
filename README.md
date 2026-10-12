# dolphinsf2

dolphinsf2 converts the music and sound effects of Nintendo GameCube games to MIDI, SoundFont 2
and SFZ. It reads games that play their sound through JAudio2. JAudio2 is the second generation
of the sound library in Nintendo EAD's JSystem. Each sequence gets a MIDI file and a SoundFont
containing its instruments. The SFZ files hold the same instruments at the game's level (see
[SFZ files](#sfz-files)).

Games known to work:

- The Legend of Zelda: Four Swords Adventures

Other games that use JAudio2 may convert too. Some play music from streamed audio files
rather than sequences. dolphinsf2 leaves those out. Games with the first JAudio, such as The
Wind Waker, keep their sound in an `.aaf` file. dolphinsf2 doesn't read those.

dolphinsf2 reads a game's disc image or its start-up archive:

- **A disc image:** a plain `.iso` or `.gcm` file or an NKit image. dolphinsf2 can't read
  compressed images (RVZ, WIA, GCZ and CISO). Convert them to ISO with Dolphin first:
  right-click the game and choose Convert File.
- **A start-up archive:** the game's `.baa` file. dolphinsf2 reads the sequence archive from a
  `Seqs` folder beside it and the wave archives from a `Waves` folder. To get these files,
  right-click the game in Dolphin, choose Properties and extract the disc on the Filesystem tab.

Drop inputs onto `dolphinsf2.exe` on Windows or `Dolphinsf2.app` on macOS. Output goes in a
folder beside each input. From a terminal:

```
dolphinsf2 [options] <archive.baa | disc image> [...]

  --output DIR   write to DIR (one input only)
  --only REGEX   convert matching sequence names (for example --only BGM)
  --loops N      repeat the main loop N times (default 1)
                 MIDI loop markers are included for players that support them
  --seed N       initial game random state (default 0)
  --port N=V     set port N (0-15) of each sequence to V before it starts;
                 repeat for more ports
  --voices N     drop the notes that the game drops for lack of a voice,
                 with N voices (the game has 64). The result only
                 approximates the game.
  --voices off   keep every note (default)
  --list         list sequences without converting
  --quiet        print errors only
  --version      show the version
```

Games write ports to steer their music. A sequence can branch on a port's value or wait until
the game writes the port. `--port` writes a port before each sequence starts. As in the game,
ports start at 0. Use `--only` to choose the sequences it applies to.

The game plays at most 64 notes at once. A note that finds every voice busy takes the voice of
an older note. By default, dolphinsf2 keeps every note. `--voices 64` drops the notes that the
game drops for lack of a voice. The result only approximates the game.

The sequences come from the game's sound table. They keep the table's order. Sound effects are
sequences too. Files take the names that the table gives the sounds. A sound without a name
becomes `SOUND_<section>_<group>_<index>`. Files of the sequence archive that no sound plays
come last. They keep the names of their files. Names get adjustments for Windows. Forbidden
characters and trailing dots or spaces become `_`. Reserved device names such as `CON` get an
added `_`. If names differ only in ASCII letter case, later sequences get `_<n>` added. Here n
is the sequence's place in the `--list` output. The first is 0. This avoids collisions on
Windows and macOS. The same applies to a sequence named `samples`. That name would collide with
the SFZ sample folder. The default output folder is `<input name>_dolphinsf2`. Its name follows
the same rules. Use `--output` to choose another location.

## Downloads

The Releases page has dolphinsf2 for 64-bit Windows. On macOS on Apple Silicon, build it from
source (see [Building](#building)).

## Drag and drop

- **Windows:** drop one or more `.baa` files or disc images onto `dolphinsf2.exe`. The console
  shows the results and waits for Enter before closing.
- **macOS:** drop them onto `Dolphinsf2.app` or open the app to choose files. The app shows the
  results and can open the output folders. The app wraps the command-line program. Finder can't
  drop files onto a bare executable. The app is part of the [build](#building).

dolphinsf2 reports an unreadable input and goes on with the others. If two inputs would use the
same output folder, dolphinsf2 skips the second. This protects the first one's files. It can
happen when names differ only in extension. It can also happen when they differ only in case on
a case-insensitive filesystem. Filesystems on Windows and macOS are usually case-insensitive.

## The conversion

### The game's files

The sound of a JAudio2 game comes in three kinds of file:

- **The start-up archive** (`.baa`) contains the sound table and the sounds' names, the
  instrument banks (IBNK) and the wave systems (WSYS). It can also contain a collection of
  sequences. These are usually the sound effects.
- **The sequence archive** (an RARC archive in `Seqs`) contains the music's sequences (BMS).
- **The wave archives** (`.aw` files in `Waves`) contain the samples. Each wave system names the
  archives that hold its waves.

dolphinsf2 reads both versions of JAudio2's banks. Waves can be 4-bit ADPCM, 8-bit PCM or 16-bit
PCM. dolphinsf2 doesn't decode 2-bit ADPCM. No known game uses it. A missing or damaged wave
archive silences only the notes that use it. dolphinsf2 reports it.

### Sequence playback

A sequence (BMS) is bytecode. It has registers, ports, conditions, calls, jumps, loops,
interrupts and tracks that open other tracks. dolphinsf2 executes it and records the resulting
notes and controllers. dolphinsf2 follows these parts of JAudio2's sequence player:

- All 96 commands. That includes their register forms and their jump and call tables.
- Timing in sub-frames of 80 samples at the DSP's output rate. A second holds about 400
  sub-frames. In each sub-frame, the voices move on first. Then the sequence runs the ticks that
  its tempo has reached. Like the game, dolphinsf2 computes tempo in single precision. Ticks
  therefore fall in the same sub-frames as in the game.
- Notes with a length, notes that end at a note-off, and waits for a note to finish. A note
  whose wave doesn't loop ends when the wave runs out. dolphinsf2 follows the note's pitch
  sub-frame by sub-frame to find that point. Key, bend, sweep, vibrato and pitch envelope all
  count.
- Oscillators: the envelopes that move a note's volume and pitch. They move on sub-frame by
  sub-frame. Release tables and direct releases take over at a note's release. A track can
  replace an instrument's oscillators. A note ends when its volume oscillator stops.
- The DSP's 64 voices and the game's priorities. A note takes a free voice. Without one, it
  takes the oldest voice whose priority is at or below the note's priority. A released note
  counts at its release priority.
- The game's random generator. It starts from the game's initial state.
- Ports, interrupts and the timer that raises them.
- Main-loop detection and MIDI loop markers. Tracks are taken in the order they opened. The main
  loop belongs to the first that can still advance. A pass counts only if earlier tracks
  contribute nothing during it. Tracks that wait for ever on held notes don't block this choice.
  A pass that changes nothing, such as one that only polls a port, counts only while no other
  track changes the sound or plays on. Writing another track's port counts as a change. A track
  that has sounded nothing and changed nothing for 10 seconds doesn't count as playing on. A
  pass that changes nothing and sounds nothing ends the sequence. Its tracks only wait from then
  on. Backward jumps to commands the track hasn't run aren't loops. Neither are passes without
  waits. The markers go on the first pass that a player looping on them repeats as the game
  plays the next pass: the same notes at the same times with the same settings and tempo. The
  first pass often differs. Setup before the loop can land in its first tick. The game can carry
  a value from pass to pass. If no pass so far repeats that way but the next pass does, the
  performance plays that pass too. Otherwise the markers stay on the first pass. They also stay
  there in a loop that draws random numbers. At the loop's start, the MIDI file sends each
  controller, pitch, preset and tempo that the game resets there when the loop ends with another
  value. Without those events, a player that loops on the markers would keep the loop end's
  value.

Short loops, such as repeating sound effects, run until the MIDI is at least 10 seconds long.
Sounds held without end stop 10 seconds after their last change. A wait of 9,600 ticks or more
counts as waiting for ever. So does a wait for a looping note that nothing releases. Games use
such waits for sounds that they stop themselves. A sequence that finishes ends when its last
sound does. So does one whose tracks all wait for ever with nothing sounding. The last release
can add up to 10 seconds. Performances stop after 15 minutes. One that has been silent for more
than 10 seconds by then ends with its last sound too. A tempo or timebase of 0 stops the
sequence. The performance ends there. dolphinsf2 reports the stop. A track that never waits
would hang the game. dolphinsf2 stops the track and reports it.

### MIDI

Each sequence track gets a MIDI track and channel of its own. Each MIDI track is named after its
sequence track: `Root` for the first, `Track <n>` for its children and `Track <n>.<m>` below
them. Tracks beyond the 16th share the last MIDI track. dolphinsf2 reports that. Channel 10,
reserved for drums in General MIDI, is allocated last. SoundFont synths use bank 128 for that
channel. dolphinsf2 copies that channel's presets to bank 128.

MIDI output keeps tempo changes. It uses at least 7680 ticks per quarter note and a whole number
of MIDI ticks per sequence tick. The game runs each tick at the start of a sub-frame. The DSP
plays the tick's changes from the next sub-frame. dolphinsf2 places events at the start of the
sub-frame that holds each tick's time. It rounds them to the nearest MIDI tick. This moves the
whole sequence a few milliseconds earlier but keeps the spacing of events. At 120 BPM, the
rounding error is at most 1/30 ms. Events never cross a loop marker when moved. The loop keeps
its contents.

JAudio2 gives each note its track's settings in every sub-frame. A note therefore starts with
its tick's final settings. MIDI controller changes come before notes at the same tick.

| Game setting | MIDI output |
|---|---|
| Velocity | Note velocity. SoundFont and SFZ players square it. JAudio2 squares it too. |
| Track volume | CC7. Like a track volume in JAudio2, CC7 acts squared. |
| The volumes of the tracks above and the sound's volume from the sound table | CC11. It acts squared too. Values are scaled for sequences exceeding MIDI's range (see [Differences from the game](#differences-from-the-game)). |
| Track pan | CC10 |
| Track bend, and a note's sweep or tied key | Pitch bend. RPN 0 sets each track's range. |
| Vibrato depth | CC1 = 128 × depth / 100 cents. It sets the depth of the preset's vibrato. |
| Effect send | CC91 (reverb) |
| Bank and program, the track's oscillators and direct release, and the vibrato's speed and delay | Bank select (CC0) and program change |

A MIDI channel can hold only one note per key. Repeated strikes that overlap are common in
drums. They get extra channels with matching controllers. These are called layers. Layers stop
at the 16-channel limit.

A MIDI channel has one set of controllers for all its notes. In the game, a note follows its
track's settings until the track detaches it. A detached note, such as a release tail, keeps its
last settings. While such notes sound and channels remain, the track's new notes go on another
channel. Each channel gets a MIDI track named after the sequence track. Layers get such tracks
too.

### SoundFont

Each distinct combination of bank program and track settings becomes a preset. The program's
regions become zones with:

- Key and velocity ranges that follow JAudio2's lookup. A key plays the first key region whose
  high key is at or above it. A velocity plays the first velocity region in that key region
  whose high velocity is at or above it. A drum set gives each key a zone.
- The region's wave, root key, tuning, volume and pan. A drum plays its wave at the wave's rate.
  The drum's key is the zone's root key.
- The instrument's effects. Effects that follow the key get a zone per key. Random effects keep
  their base value. Effects that follow the velocity keep their value at the top of each
  velocity region.
- A volume envelope fitted to the curve that the note's oscillators trace. Like JAudio2's linear
  segments, the attack rises linearly in amplitude. Decays and releases fall linearly in
  decibels. Each is fitted to reach the level that the game's curve reaches when the curve has
  fallen half its range in decibels. A curve that falls to silence is fitted at -20 dB.
  The release is fitted from the level that the curve holds at. Notes on a curve that stops or
  falls to silence are released before its end. Their release is fitted from the curve's peak.
  SoundFont decays and releases fall 100 dB in 101.6 s at the slowest. Slower ones play at that
  rate. The SFZ files keep the game's rate.
- A vibrato LFO at the track's speed and delay. CC1 sets its depth.
- Loops repeated as needed to meet SoundFont padding rules: 8 samples before, at least 32
  within, and 8 after. Playback doesn't change.

An ADPCM wave's loop repeats from the decoder history that the wave system stores. Where that
history differs from the decoder's state at the loop start, dolphinsf2 unrolls the loop once.
The first pass and the repeats then play as in the game.

SoundFont zones can't amplify their samples. If a zone plays above full level, dolphinsf2
attenuates every zone equally and reports it. SFZ keeps the level.

## SFZ files

dolphinsf2 also writes SFZ files with the same instruments:

- `sfz/<sequence>/<track>.sfz`: the instruments of one MIDI track and its layers. Each file is
  named after its MIDI track.
- `sfz/samples/`: 16-bit WAV samples shared by all sequences in the game.

SFZ players ignore MIDI channels. Each file contains one instrument. Load the MIDI in a DAW and
assign an SFZ player, such as sfizz, to each track with its matching file. Output targets sfizz
1.2.

Presets use program change. A sequence with more than 128 presets uses bank select (CC0) too.
SFZ regions differ from SoundFont zones in these ways:

- **Envelopes.** As in the game, sfizz decays and releases fall linearly in dB. They fall by
  78.17 dB over their specified duration. Times are fitted as for SoundFont. SoundFont's limit
  on them doesn't apply.
- **LFO.** The vibrato is a sine wave. The game uses a sine too.
- **Pan.** A curve maps CC10 to `(value - 64) / 64`. That's the SoundFont convention that the
  MIDI output uses.
- **Level.** SFZ region `volume` restores any gain lost when scaling MIDI CC11. It also keeps
  zones above full level. Playback then keeps the game's level.
- **Controllers.** CC7, CC10 and CC11 act at once. sfizz's default 10 ms smoothing is off. A
  note whose track sets its volume or pan in the note's starting tick begins at that level and
  pan.
- **Loops.** sfizz crossfades the loop end with data before the loop start over at least 1 ms.
  Samples include 4 ms of repeated loop data before the start. The crossfade is then inaudible.

During offline rendering, sfizz 1.2.3 can silence streamed samples after their preloaded
portion. The files request full loading with `hint_ram_based=1`. sfizz prints "Unsupported value
for hint_stealing: 1" for this. The message is harmless.

## Differences from the game

dolphinsf2 reports approximations for each sequence. Some behavior can't be represented exactly
in MIDI, SoundFont or SFZ:

- **Per-note pitch.** In the game, a note can sweep from the last key to its key, and a tied
  note moves on to the next key. MIDI pitch bend plays both, but it affects the whole channel.
  While channels last, a note that bends goes on a channel where no released note of its track
  sounds. Otherwise those released notes bend with it. dolphinsf2 reports that. A sweep stops at
  the note's release in MIDI. In the game, it goes on through the release. An oscillator can
  move a note's pitch too. dolphinsf2 leaves those pitch envelopes out and reports them.
- **Tied notes.** A tied note goes on with the sounding note at a new key and velocity. The MIDI
  note bends to the new key. MIDI can't change a sounding note's velocity. CC11 follows the new
  velocity instead. A note that its ties make louder starts at the highest velocity they reach,
  with CC11 lower until then. Where that velocity would play another wave, the note starts at its
  first velocity. CC11 alone then makes it louder.
- **Envelopes.** JAudio2's oscillators are tables of points joined by linear, square,
  square-root and other curves. SoundFont and SFZ envelopes have one attack, hold, decay,
  sustain and release. The fitted envelope can't follow a table that rises again or loops.
- **Random instruments.** An instrument's random effects vary the volume or pitch of each note.
  SoundFont presets can't vary from note to note. dolphinsf2 uses the effects' base values and
  reports them.
- **Voices.** The game plays at most 64 voices at once. When they run out, a new note takes a
  voice from an older note. The older note stops at once in the game. In MIDI, it gets a
  note-off and plays its release. Notes that get no voice are left out. dolphinsf2 reports both.
- **Vibrato.** Notes keep the vibrato speed and delay that their track has when they start.
  Depth changes work through CC1 up to a semitone. A deeper vibrato is kept to a semitone and
  reported. SoundFont's LFO is a triangle wave. The game uses a sine.
- **Omitted features.** dolphinsf2 leaves these out and reports them: tremolo, track filters,
  notes on the DSP's built-in oscillators (programs 0xf0 and up), sample skips and notes on a
  missing bank.
- **Effect send.** Synths play CC91 as reverb. In the game, the send reaches whatever effect the
  game has set up. There may be none.
- **Volume limits.** JAudio2 multiplies the volumes of a track, the tracks above it and the
  sound. MIDI CC7 holds the track's volume. CC11 holds the rest. CC11 stops at 127. If
  necessary, all CC11 values are scaled equally. This keeps the balance of the tracks but lowers
  the level of the sequence. dolphinsf2 reports the reduction. SFZ restores the MIDI gain.
- **Resampling.** Synths resample waves differently from the GameCube's DSP. High frequencies
  can differ as a result.

SoundFont output follows the specification and targets FluidSynth. Player differences remain:

- **Pan.** JAudio2's pan law is FluidSynth's sin/cos law. TinySoundFont uses a square-root law.
  Its pans come out slightly narrower.
- **Envelopes.** Times follow the specification's 100 dB range. FluidSynth uses 96 dB. Decays
  and releases then fall 4% slower in decibels. Sustain levels are 4% shallower: -11.9 dB plays
  as -11.4 dB. FluidSynth also slows any release faster than 15.6 ms (-7200 timecents) to that
  time.
- **Attenuation.** Like E-mu hardware, FluidSynth applies 0.4 of a zone's initial attenuation.
  The specification applies all of it.
- **Pitch.** FluidSynth rounds pitch down to whole cents. Fractional bends can be almost a cent
  flat. The beating of notes detuned by less than a cent changes too.
- **Modulators.** TinySoundFont lacks SoundFont modulator support. CC1 vibrato depth has no
  effect there.
- **Tempo changes.** FluidSynth rounds its position to the nearest MIDI tick at each change and
  restarts its clock there. This can advance playback by half a tick per change: 1/30 ms at 120
  BPM with 7680 ticks per quarter note. TinySoundFont rounds tempo-change times down to
  milliseconds. Its playback runs ahead by about half a millisecond per change on average.
- **Note timing.** FluidSynth applies MIDI events at the start of each 64-sample render block.
  Notes can start up to 64 samples late. That's about 1.5 ms at 44.1 kHz.
- **Short notes.** FluidSynth holds every note for at least 10 ms (`synth.min-note-length`).
  Shorter notes therefore sound longer than in the game.

## Building

Building requires C++20 and CMake 3.20 or later. It also requires three libraries that the
converters and the PSF players share: seqlib (MIDI, SoundFont 2 and SFZ), xsflib (common code)
and ntdlib (Nintendo's formats). They're submodules in `externals/`. Clone with
`git clone --recursive`, or run `git submodule update --init` in a checkout. There are no other
build dependencies. dolphinsf2 supports macOS on Apple Silicon and Windows x64 with clang-cl.
CI builds and tests both.

- **Windows:** install Visual Studio 2022 with "Desktop development with C++". That workload
  includes CMake. Add the "C++ Clang tools for Windows" component. Use Developer PowerShell or
  Developer Command Prompt.
- **macOS:** install Xcode command line tools (`xcode-select --install`) and CMake
  (`brew install cmake` or the installer from cmake.org).

From the repository root:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

On Windows, the first command is `cmake -S . -B build -A x64 -T ClangCL`. It selects clang-cl
and 64-bit output.

The executable is `build/dolphinsf2`. With Visual Studio, it's `build/Release/dolphinsf2.exe`.
Xcode and other multi-configuration generators also use `build/Release`. The Windows build links
the runtime statically. The executable requires no runtime DLLs. CI uploads the Windows
executable as an artifact. macOS builds also produce `build/Dolphinsf2.app`. The app contains a
copy of the program. You can move the app elsewhere.

C++ formatting uses clang-format with the repository's `.clang-format`.

## Tests and tools

`ctest` runs unit tests without game data. The tests build archives, banks, wave systems and
sequences in memory. Coverage includes:

- Wave decoding, and what banks of both versions play for a note: instruments, drum sets,
  velocity regions and effects.
- Oscillators and the envelope fit.
- The player: tempo and timing, sweeps and ties, loops and their markers, ports and registers,
  drums and volume.
- SoundFont and MIDI output checked by independent readers, and SFZ output.

Other checks and tools:

- `-DDOLPHINSF2_TEST_ARCHIVE=path/to/archive.baa` adds conversion of every sequence of that
  game. Each output file is validated with independent readers.
- seqlib's `seqlib_check` program checks SoundFont and MIDI files. seqlib's README says how to
  build it.

The conversion test requires game files supplied separately. dolphinsf2 doesn't distribute any.

## Licence

[MIT](LICENSE). The program contains the libraries seqlib, xsflib and ntdlib. They're
MIT-licensed too. The one exception is a table in ntdlib made from Unicode's data. It's under
the Unicode License v3. `THIRD-PARTY-NOTICES.md` gives their licence texts, and the release zip
includes it. Game data is not covered by this licence.
