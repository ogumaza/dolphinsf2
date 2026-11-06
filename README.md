# dolphinsf2

dolphinsf2 converts the music and sound effects of Nintendo GameCube games to MIDI, SoundFont 2
and SFZ. It reads games that play their sound through JAudio, the sound library in Nintendo
EAD's JSystem. Both generations convert: the first JAudio and JAudio2. So do the files of Wii
and Wii U games that use JAudio. dolphinsf2 also converts the music and sound effects of a few
games that play them through Factor 5's MusyX. It converts the music of the games that play it
through a port of Nintendo's sound library for the Nintendo 64, and of a game that plays it
through Square's Red driver. Each sequence gets a MIDI file and a SoundFont containing its
instruments. The SFZ files hold the same instruments at the game's level (see
[SFZ files](#sfz-files)).

Games known to work:

- The Legend of Zelda: Four Swords Adventures (JAudio2)
- The Legend of Zelda: Twilight Princess (JAudio2), on the GameCube and the Wii, and Twilight
  Princess HD (Wii U)
- Link's Crossbow Training (Wii, JAudio2)
- The Legend of Zelda: The Wind Waker (the first JAudio), on the GameCube, and The Wind Waker HD
  (Wii U)
- Luigi's Mansion (USA, the first JAudio): the music and sound effects, with the differences
  that [Differences from the game](#differences-from-the-game) gives
- The Legend of Zelda: Collector's Edition (JAudio2): the menu's sounds
- Mario Kart: Double Dash!! (USA, JAudio2): the music in sequences and the sound effects, with
  the voices
- Donkey Kong Jungle Beat (USA, JAudio2): the music and sound effects. Most of the effects that
  the game plays in the chords of its music come out silent (see [Game rules](#game-rules)).
- Super Mario Sunshine (USA, the first JAudio): the music and sound effects
- Pikmin (USA, the first JAudio): the music and sound effects
- Pikmin 2 (USA, the first JAudio), on the GameCube and the Wii: the music and sound effects. Many
  of the effects play quieter than in the game. The cave music's level often strays from the
  game's (see [Game rules](#game-rules)).
- Mario Golf: Toadstool Tour (USA, MusyX): the music and sound effects
- Mario Power Tennis (USA, MusyX): the music and sound effects
- Mario Party 4 (MusyX), in its American, European and Japanese releases: the music of the
  sound test and the sound effects
- Pokémon Colosseum (USA, MusyX): the music and sound effects
- WarioWare, Inc.: Mega Party Game$! (USA, MusyX): the music and sound effects
- Nintendo Puzzle Collection (Japan, MusyX): the music and sound effects of the menu, Dr. Mario,
  Panel de Pon and Yoshi's Cookie
- The NDDEMO disc (MusyX): the music and sound effects. The disc's header names it NDDEMO. It
  never went on sale.
- Star Fox Adventures (USA, disc version 1, MusyX): the music and sound effects
- Paper Mario: The Thousand-Year Door (USA, MusyX): the sequenced music and the sound effects
- Pokémon Channel (USA, MusyX): the music and sound effects
- Star Wars Rogue Squadron II: Rogue Leader (USA, MusyX): the sequenced music and the sound
  effects
- Star Wars Rogue Squadron III: Rebel Strike (USA, disc version 1, MusyX): the sound effects
- Resident Evil (USA, first disc, MusyX): the sequenced music
- Resident Evil Zero (USA, both discs, MusyX): the sequenced music and the sound effects
- Tales of Symphonia (USA, both discs, MusyX): the music and sound effects
- Metroid Prime (USA, disc version 2) and Metroid Prime 2: Echoes (USA), both MusyX: the
  sequenced music and the sound effects
- Animal Crossing (USA), Doubutsu no Mori+ and Doubutsu no Mori e+ (Japan, disc versions 0 and 1
  of each), with the Nintendo 64's sound library: the music
- Final Fantasy Crystal Chronicles (USA, Square's Red driver): the music

Other games that use JAudio may convert too. Some play music from streamed audio files rather
than sequences. dolphinsf2 leaves those out. The Wind Waker, The Wind Waker HD, Pac-Man Vs.,
Pikmin 2, Luigi's Mansion and Super Mario Sunshine play all their sound effects in one sequence.
Pikmin plays them in two. dolphinsf2 converts each effect as the game's sequences play it when
the game starts the effect. It leaves out the effects of other games that use the first JAudio (see
[The first JAudio's sound effects](#the-first-jaudios-sound-effects)).

A MusyX game lists its songs in a table in its code. dolphinsf2 reads that table in Mario Golf:
Toadstool Tour, Mario Power Tennis and Pokémon Colosseum. Mario Party 4 keeps its songs and
their volumes in the file of Hudson's sound manager. dolphinsf2 converts the songs that the
game's sound test lists and gives them the sound test's names. The sound test's streams are
recorded audio, not songs. dolphinsf2 leaves them out. WarioWare, Inc.: Mega Party Game$! lists
its music in its sound room. dolphinsf2 converts those songs and gives them the sound room's
names. Nintendo Puzzle Collection keeps its music in the modules of its code: one for the menu
and one for each game. dolphinsf2 converts the songs that the modules hold. Each song's name
starts with its module's name. Dr. Mario's and Panel de Pon's songs take the names that the
games' debug modules give them. The menu's songs take their numbers. Yoshi's Cookie's songs take
the names of their files. The NDDEMO disc's code lists the paths of its songs' files. dolphinsf2
names each song after its file. It recognizes each of these games by its `main.dol`. Other MusyX
games don't convert.

Animal Crossing and its Japanese releases list their music in a table in their code. dolphinsf2
reads that table and recognizes each release by its `main.dol`. Final Fantasy Crystal Chronicles
keeps each piece in a file. dolphinsf2 finds the game by the folder of those files on its disc
and names each piece after its file. It leaves out these games' sound effects (see
[The Nintendo 64's sound library](#the-nintendo-64s-sound-library) and
[Square's Red driver](#squares-red-driver)).

dolphinsf2 reads a game's disc image, an extracted disc or a JAudio game's start-up archive:

- **A disc image:** a plain `.iso` or `.gcm` file, an NKit image, or a compressed image in one
  of four formats: RVZ (`.rvz`), WIA (`.wia`), GCZ (`.gcz`) or CISO (`.ciso`). Dolphin reads
  all of them. Some discs that never went on sale leave the magic word out of their header.
  dolphinsf2 reads such a disc when the rest of it is intact.
- **An extracted disc:** its `sys/main.dol`. dolphinsf2 reads the game's other files from the
  `files` folder beside `sys`. To extract a disc in Dolphin, right-click the game and choose
  Properties. On the Filesystem tab, right-click the disc and choose Extract Entire Disc.
- **A start-up archive:** a JAudio2 game's `.baa` file, or the `.aaf` file of a game with the
  first JAudio. The archive can be compressed with Yaz0 (`.szs`). dolphinsf2 reads the sequence
  archive from a `Seqs` folder beside it. It reads the wave archives from a `Waves` folder
  (JAudio2) or a `Banks` folder (the first JAudio). Where that folder is missing, it reads them
  from the other one. An extracted disc has these files.

Super Mario Sunshine keeps its start-up archive inside another archive. Pikmin has no start-up
archive: its code holds the index of its sequences. dolphinsf2 reads these two games from a disc
image or an extracted disc.

dolphinsf2 doesn't read Wii discs or Wii U titles. Give it the game's start-up archive instead.
For a Wii game, Dolphin's Extract Entire Disc writes the game's files to a `files` folder. A Wii
U title keeps them in its `content` folder. The start-up archive is the `.baa` or `.aaf` file
among them, with its `Seqs` folder beside it.

Drop inputs onto `dolphinsf2.exe` on Windows or `Dolphinsf2.app` on macOS. Output goes in a
folder beside each input. From a terminal:

```
dolphinsf2 [options] <start-up archive | disc image | main.dol> [...]

  --output DIR   write to DIR (one input only)
  --only REGEX   convert matching sequence names (for example --only BGM)
  --loops N      repeat the main loop N times (default 1)
                 MIDI loop markers are included for players that support them
  --seed N       initial game random state (default 0; JAudio2 games,
                 Luigi's Mansion and the Animal Crossing games)
  --port N=V     set port N (0-15) of each sequence to V before it starts;
                 repeat for more ports (JAudio only)
  --voices N     drop the notes that the game drops for lack of a voice,
                 with N voices (the game has 64). The result only
                 approximates the game. (JAudio only)
  --voices off   keep every note (default)
  --list         list sequences without converting
  --quiet        print errors only
  --version      show the version
```

Games write ports to steer their music. A sequence can branch on a port's value or wait until
the game writes the port. `--port` writes a port before each sequence starts. As in the game,
ports start at 0. Use `--only` to choose the sequences it applies to. For a sound effect of the
first JAudio, `--port` writes a port of the effect's slot (see
[The first JAudio's sound effects](#the-first-jaudios-sound-effects)).

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

A game with the first JAudio names its sequences in a name file (`.asn`), where it has one. The
Wind Waker HD has one beside its start-up archive. Without a name file, a sequence takes the
name of its file in the sequence archive. A sound without a name can play a file that an earlier
sound plays at the same volume. Its files would be the same. dolphinsf2 lists the sequence once.
A sound that plays the file at another volume becomes `SOUND_<section>_<group>_<index>`. The
files of a sequence table have no names. One that no sound plays becomes `SEQUENCE_<number>`.
The sound effects come after the music, in the sound table's order. An effect takes its name
from the name file. Without one, it becomes `se<category>-<index>`, such as `se4-0123`. The
sound table gives the category and the index.

## Downloads

The Releases page has dolphinsf2 for 64-bit Windows. On macOS on Apple Silicon, build it from
source (see [Building](#building)).

## Drag and drop

- **Windows:** drop one or more `.baa` or `.aaf` files, disc images or `main.dol` files onto
  `dolphinsf2.exe`. The console shows the results and waits for Enter before closing.
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

Mario Kart: Double Dash!! has the earliest known JAudio2. It keeps its music in sequence blocks
of the start-up archive, not in a sequence archive. Another block of the start-up archive holds
three more start-up archives with the voices. The game loads one of them at a time. dolphinsf2
reads all three.

A game with the first JAudio has the same kinds of file in another layout:

- **The start-up archive** (`.aaf`) contains the sound table, the instrument banks and the wave
  systems.
- **The sequences** are in an RARC archive in `Seqs`. The Wind Waker HD keeps them in a sequence
  table (`.bdt`) there instead. Luigi's Mansion keeps them in one file, `JaiArcS.arc`. The start-up
  archive's sequence index divides it.
- **The wave archives** (`.aw` files in `Banks`) contain the samples.

The Wind Waker HD also has a name file (`.asn`) beside its start-up archive. It gives the
sounds' names. Super Mario Sunshine has one too, in the folder of its `Seqs` and `Banks`
folders. That game keeps its start-up archive in an RARC archive that Yaz0 compresses (`.szs`).

Pikmin has an earlier form of the first JAudio. A bank file (`.bx`) in `Banks` contains the
instrument banks and the wave systems. One file in `Seqs` holds every sequence. The index that
divides that file is in the game's code (`main.dol`).

Donkey Kong Jungle Beat compresses its start-up archive with Yaz0 (`.szs`). The game decodes it
as it loads it. The game keeps its wave archives in `Banks`, where other JAudio2 games have
`Waves`.

dolphinsf2 reads both versions of JAudio2's banks. Waves can be 4-bit ADPCM, 2-bit ADPCM, 8-bit
PCM or 16-bit PCM. Super Mario Sunshine's voices of its townsfolk use 2-bit ADPCM, and so do a few
of Pikmin's effects. A missing or damaged wave archive silences only the notes that use it.
dolphinsf2 reports it.

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
  replace an instrument's oscillators. A note ends when its volume oscillator stops. It also
  ends where the oscillator holds at zero and no release can raise it.
- The DSP's 64 voices and the game's priorities. A note takes a free voice. Without one, it
  takes the oldest voice whose priority is at or below the note's priority. A released note
  counts at its release priority.
- The game's random generator. It starts from the game's initial state. The first JAudio has
  another generator than JAudio2. Its state at the start is fixed, and `--seed` doesn't change
  it. Luigi's Mansion's generator reads the console's clock at each draw. No state gives its
  numbers. That game's sounds draw from JAudio2's generator, which `--seed` sets.
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

The first JAudio's sequences use the first command set. dolphinsf2 plays them with the same
player, by that command set's rules: its gate notes, its registers and conditions, and its
tracks' tempo and timebase. Three things differ from the game. A track that shares its parent's
voices gets separate voices. Oscillators that a track sets are left out. So are the tempos of
child tracks, except in Pikmin. dolphinsf2 reports all three. A track can also send its notes
through the DSP's automatic mixer. That mixer sets their gains from their volume and pan.
dolphinsf2 plays those notes at half the level of the others. That level approximates the game's
mix.

Pikmin's sequences use an earlier form of that command set. Some of its commands take other
arguments, and a few do nothing. Each track has 32 registers. A child track can set a tempo. It
then runs at that tempo, but no faster than its parent. A track plays its volume as it is, where
the later form squares it. CC7 then holds the volume's square root. dolphinsf2 follows these
rules in Pikmin's sequences.

Pikmin's code sets the level of its whole mix at start-up. The level comes to 0.549 of Super
Mario Sunshine's mix, about 5.2 dB below it. dolphinsf2 plays all of Pikmin's notes at that
level.

### The first JAudio's sound effects

The Wind Waker, The Wind Waker HD, Pac-Man Vs., Pikmin 2, Luigi's Mansion and Super Mario
Sunshine play all their sound effects in one sequence. In all but Pikmin 2, it's the first
sequence of the sound table. The sound library starts it when the game starts. Pikmin 2's code
names another sequence of the table. dolphinsf2 recognizes these games by their start-up
archives: those of The Wind Waker's USA, European and Japanese releases, of The Wind Waker HD's
USA, European and Japanese releases, of Pac-Man Vs.'s USA, European and Japanese releases, and
of Luigi's Mansion's USA release. For Pikmin 2, it recognizes the USA release's archives on the
GameCube and on the Wii. For Super Mario Sunshine, it recognizes the USA release's archive. A
release whose archive differs from all of these converts without its effects. Pikmin plays its
effects in another way (see below). dolphinsf2 converts the effects of no other game that uses
the first JAudio. Many of Pikmin 2's effects play quieter than in the game.

The effects sequence gives each category of effects a number of slots. The start-up archive's
table of scenes sets the counts. In The Wind Waker, each category has four. A slot is a track
that waits for an effect. To start an effect, the library writes four of the slot's ports: a
wait for the distance, the map's information, the effect's number and a request. The slot's
track then plays the effect. dolphinsf2 plays each effect that way, in the first slot of its
category. The library's writes come after the first tick of the effects sequence. An effect's
first note therefore comes a tick after the start of its file. Only the slot's track and the
tracks below it count for the loops and the end. The effect ends when its slot is free again. An
effect's volume is its volume in the sound table times the volume of its category. An effect
also plays at the pitch that the table gives it. The MIDI file holds that pitch as a pitch bend.
Luigi's Mansion's code gives its effects neither. They play at full volume and at their notes'
pitch.

`--port` writes ports of the effect's slot. The game's code does the same when it passes values
with an effect. A value for port 6 stands for the map's information. Some effects, such as
footsteps, play another sound for each value there. Without a value, some of them play
nothing. Ports 0, 3 and 4 take the library's values. `--port` doesn't change them. Some effects
play nothing in any case. Most of those end at once. Their files hold no notes.

Pikmin plays its effects in two sequences. The sound library starts both at start-up. The game's
code holds a catalogue of the effects. To start an effect of the system or of the player, the
library writes the effect's number to a port of a track of the first sequence. That track then
opens a track below it to play the effect. The effects of the game's events go to tracks of the
second sequence in the same way. dolphinsf2 plays each effect that way and names it after the
catalogue. The effect ends when the tracks below the port's track have closed. `--port` writes
other ports of that track. The game's code also plays six effects that the catalogue doesn't
list. Five belong to the system and the player. dolphinsf2 names them `system-<n>` and
`player-<n>`, with the effect's number in hexadecimal. The sixth is the sound of the formation.
The game plays it while the player moves the formation. Its tempo follows the length of the
move. dolphinsf2 plays it for a full move straight ahead and names it `formation`. dolphinsf2
leaves out four entries of the catalogue. One stops the effects. The sequence plays nothing for
another. A third plays a part of the cutscenes' music. dolphinsf2 converts that part with the
music. The fourth plays a stream. dolphinsf2 recognizes the USA release by its bank file and its
`main.dol`. Another release converts without its effects.

A track can set an envelope and route it to its notes. The envelope then takes the place of the
instrument's envelope. Many of The Wind Waker's effects set a simple envelope that rises, holds,
decays and releases. dolphinsf2 plays it, in The Wind Waker, Pac-Man Vs. and Pikmin 2. It leaves
out the track envelopes that come from a template or from a table in the sequence. It also leaves
out an envelope whose speed or depth a track parameter changes. dolphinsf2 reports both. Luigi's
Mansion's sound library handles the track envelopes in another way. dolphinsf2 leaves all of them
out there and reports them.

Four of The Wind Waker's effects pick the key of their note at random: se3-0049, se4-0254,
se4-0255 and se4-0327. Some of those keys make no sound. The first number of the game's random
generator picks one of them for each of the four. On the GameCube, dolphinsf2 converts each of
the four once for each key that makes a sound, in place of the effect. The file's name ends in
`_key_` and the key, such as `se4-0254_key_48`. Each file plays the effect after the random
numbers that come before the first one that picks its key. In The Wind Waker HD, nobody has
checked which keys make a sound. Each of the four converts there once, with the key that the
first number picks.

Three of Pikmin 2's effects draw a random number and end at once on some numbers:
PSSE_EN_FROG_VOICE, PSSE_EN_OOPAN_BOUND2 and PSSE_EN_OOPAN_VOICE_DOWN. The generator's first
number ends all three. dolphinsf2 converts each of them after the numbers that come before the
first one on which it plays.

A game keeps some effects going by starting them again and again. dolphinsf2 plays each effect
from one start: to its end, or with loop markers where it loops.

Short loops, such as repeating sound effects, run until the MIDI is at least 10 seconds long.
Sounds held without end stop 10 seconds after their last change. A wait of 9,600 ticks or more
counts as waiting for ever. So does a wait for a looping note that nothing releases. Games use
such waits for sounds that they stop themselves. A sequence that finishes ends when its last
sound does. So does one whose tracks all wait for ever with nothing sounding. In Four Swords
Adventures, Twilight Princess and Link's Crossbow Training, a sequence that sends to the game's
echo ends when the echo falls silent instead (see [Game rules](#game-rules)). The last release
can add up to 10 seconds. Performances stop after 15 minutes. One that has been silent for more
than 10 seconds by then ends with its last sound too. A tempo or timebase of 0 stops the
sequence. The performance ends there. dolphinsf2 reports the stop. A track that never waits
would hang the game. dolphinsf2 stops the track and reports it.

### Game rules

Some rules for a game's sounds lie in the game's code, not in its files. dolphinsf2 recognizes
Four Swords Adventures by its start-up archive, in the USA, European and Japanese releases.
dolphinsf2 then follows these rules of the game:

- **System sounds.** The game plays its system sounds, the sound table's first group, at 0.7 of
  their volume. dolphinsf2 does the same.
- **Notes without a release.** A note whose oscillator has neither a release table nor a direct
  release fades out over 26.7 ms when the game releases it. In the other JAudio2 games, such a
  note stops at once. Twilight Princess's code does that.
- **The echo.** The game sets up an echo at start-up. A sound that sends to it repeats about
  every 160 ms. Each repeat has half the level of the one before. MIDI and SoundFont can't
  describe an echo. The MIDI file of a sequence that sends to it ends when the echo falls
  silent. Looping and held sounds keep their ends.
- **Muted tracks.** The game plays one piece of music, JA_BGM_SELECT, with two sets of tracks.
  On the file select and the choice of mode, only two of its tracks play. On the screen where
  the players join and on the map where they pick a stage, every track but one of those two
  plays. The game's code mutes the other tracks. When the screen changes, the piece plays on
  with the other set. dolphinsf2 converts the piece twice and leaves out the muted tracks'
  notes. `JA_BGM_SELECT` holds the first set and `JA_BGM_SELECT_map` the second. Each file
  starts from the piece's start.

dolphinsf2 recognizes Donkey Kong Jungle Beat's start-up archive, from the USA release. The
game's code mutes some tracks of its music:

- **Muted tracks.** The game starts 44 pieces with some of their tracks turned off. It turns
  some of them on later, as play goes on. dolphinsf2 leaves out the notes of the tracks that are
  off when the piece starts. One piece, JA_BGM_OP_CEMEMONY, starts with every such track off. The
  game turns them on as the piece goes on. dolphinsf2 plays that piece with the tracks of its
  next state.
- **The beat.** In 62 pieces, one track gives the game the music's beat. The game mutes that
  track. dolphinsf2 leaves its notes out.
- **The chord effects.** The game plays 74 effects of its instruments in the chords of the
  music. The game's code works out their notes' keys from the music as it plays. dolphinsf2
  doesn't follow that code. 66 of those effects come out with no notes. The other 8 keep only
  the notes whose keys don't come from the chords.

dolphinsf2 recognizes Super Mario Sunshine's start-up archive, from the USA release. It follows
these rules of the game:

- **The music's volume.** The game plays its music at 0.75 of the volume that the sound table
  gives it. dolphinsf2 does the same.
- **Yoshi's track.** In 14 pieces of music, the game mutes one track as the piece starts. The
  game turns the track on while Mario rides Yoshi. dolphinsf2 leaves out the track's notes.
- **The effects' volumes.** At start-up, the game turns down six of its nine categories of
  sound effects. The effects of the pump and of the other characters play at 0.76 of the sound
  table's volume, objects at 0.80, system sounds at 0.83, the surroundings at 0.58 and water at
  0.90. dolphinsf2 does the same. The game changes some of these volumes later. dolphinsf2
  leaves that out.

dolphinsf2 recognizes Pikmin's bank file, from the USA release. It follows these rules of the
game:

- **Banks.** The game plays each piece of music with the bank whose number is the piece's number
  in the index. dolphinsf2 does the same.
- **The cutscenes' music.** One sequence holds the music of the cutscenes, in parts. A cutscene
  picks its part with a port of the sequence's first track. The game plays eight of the parts.
  dolphinsf2 converts each of them to a separate file. The file's name is the sequence's name
  with a hyphen and the part's number.
- **The tracks' volumes.** The game sets a volume for each track of a piece of music, other than
  the cutscenes' music. Most pieces play at half their volume, one at 0.55 and one at 0.35.
  dolphinsf2 does the same. Five pieces start with some of their tracks off. The game turns some
  of them on as play goes on. dolphinsf2 leaves out the notes of the tracks that are off when
  the piece starts.

dolphinsf2 also recognizes The Wind Waker's start-up archives: those of the USA, European and
Japanese releases on the GameCube, and of the USA, European and Japanese releases of The Wind
Waker HD. The game gives each of the sound table's eight categories of sound effects a volume.
Category 1 keeps the volume of the sound table. Categories 3, 6 and 7 play at 0.82 of it. The
others play at 0.88 of it. dolphinsf2 does the same.

The Wind Waker's code also changes what some of its music plays:

- **Muted tracks.** As the game starts 14 pieces (13 in The Wind Waker HD), its code mutes some
  of each piece's tracks. `JA_BGM_D_GANON_1`, for one, plays 3 of its 16. dolphinsf2 starts each
  piece with the tracks that the game first plays.
- **States.** The game plays some pieces in other states, with other tracks. Each state
  converts as another file, named after the piece with `_` and the state added. A state that
  the game starts by an ID of its own is named after the ID, such as
  `JA_BGM_KAMI_TOWER_80000125`, which also plays 1.3143 times as fast. Others follow the game:
  `SAILING` while the boat sails, `ON_BOARD` while Link is on the boat, `NEAR` while an enemy
  is near and `SWORD` while Link's sword is out.
- **Ports.** The game writes port 9 of some pieces while they play, as a fight moves on. Each
  value that changes what a piece plays converts as another file, named after the port and
  the value, such as `JA_BGM_MBOSS_PORT9_2`.
- **Fades.** The battle music and three pieces that play over the main music fade in over two
  seconds as they start. The game always starts them that way. dolphinsf2 does the same.

A spin attack brings in a track of the battle music for a moment. The files leave it out.

dolphinsf2 also recognizes the start-up archive of Luigi's Mansion's USA release. The game plays
one piece, LuigiSings, with some of its tracks. It picks them by where Luigi is and how much
health he has. Four of the root track's children hold the tune. One plays above 70 of Luigi's 100
points of health, one above 40, one below that, and one in a lit room. The game mutes the other
three. The hallways, the outdoors and the other rooms each play other children as the
accompaniment. Some rooms play none. The piece also plays slower as Luigi's health falls, at
0.6 + 0.4 × the health / 100 of its tempo. In a lit room the tempo follows Luigi's steps. It's
0.9 while he stands still. The game never plays every track at once. dolphinsf2 converts the
piece in 13 states: the hallway, a room, outdoors and a room without accompaniment, each at 100,
70 and 40 HP, and a lit room at 100 HP. A state at 70 or 40 HP takes the top of its band of
health. The files are named after the state, such as
`LuigiSings_hallway_70_HP` and `LuigiSings_lit_room`. There's no file with every track.

dolphinsf2 also recognizes the start-up archive of Mario Kart: Double Dash!!'s USA release. Its
sound table gives each sound's volume in 255ths. The game plays its five categories of sound
effects at 1.2 to 1.8 times that volume, and its music at 1.95 times. With some race settings,
it plays four of the categories at 0.8, 0.7 or 0.65 of those volumes. dolphinsf2 follows the
default setting. The disc also holds a sequence archive, `AudioRes/Seqs/JaiSeqs.arc`. The game
never reads it. It holds the music's sequences again. dolphinsf2 leaves it out. The award
ceremony's 20 effects of the characters wait until the game has loaded the ceremony's voices.
dolphinsf2 plays them as the game does once the voices are loaded. The courses stream their
music. dolphinsf2 leaves the streams out.

dolphinsf2 recognizes Twilight Princess's two start-up archives too. The USA, European and
Japanese releases on the GameCube share one. The Wii's release and Twilight Princess HD share
the other. The game gives its categories of sound effects their volumes at start-up. Category
1, Link's voice, keeps the volume of the sound table. The others play at 120/127 of it.
dolphinsf2 does the same. During a conversation and in the pause menu, the game plays most
categories quieter for a while. dolphinsf2 leaves that out.

As Twilight Princess starts five pieces of music, it mutes some of their tracks. Hyrule Field's
day music holds two arrangements. One plays while Link walks, and the other while he rides.
dolphinsf2 converts the piece twice and leaves out the muted tracks' notes.
`Z2BGM_FIELD_LINK_DAY` holds the walking arrangement and `Z2BGM_FIELD_LINK_DAY_riding` the
riding one. `Z2BGM_TOAL_VILLEGE`, `Z2BGM_HORSE_BATTLE`, `Z2BGM_BOSS_SNOWWOMAN_1` and
`Z2BGM_VS_GANON_02` leave out the tracks that the game mutes as they start. Later, the game's
scenes, fights and other code change the tracks of 17 pieces. Some states play a track at part
of its volume, such as Ordon Village's tracks at 0.65. The piece's file plays the state that the
game starts the piece in, such as the state of a dungeon's first room. dolphinsf2 converts each
later state as one more file of the piece. Its name adds the status that the game gives the
piece, such as `Z2BGM_TOAL_VILLEGE_status1`. The battle music's states follow the fight:
`_near`, `_searched`, `_near_searched` and `_port9_1`. Hyrule Field's day music also follows the
field's code: what Link does, the enemies near him and the time of day. dolphinsf2 converts each
way through the piece as one more file in each arrangement. `_status12` plays the sections of a
long walk or ride. `_status4` to `_status7` play a fight's sections by their four entries.
`_status10` and `_status11` play the two endings of a day. `_status8` and `_status9` play the
starts that the field's code makes once no music plays. `_riding_status3` plays the section of
Link standing on his horse. The game moves the tracks to a new state gradually. dolphinsf2 plays
each state from the start and leaves out the states in between.

Twilight Princess sets up the same echo as Four Swords Adventures at start-up, on the GameCube
and on the Wii. So does Link's Crossbow Training. The MIDI file of a sequence that sends to it
ends when the echo falls silent. Looping and held sounds keep their ends.

dolphinsf2 recognizes Pikmin 2's start-up archive, from the USA release on the GameCube and on
the Wii. The game plays each piece of music through an object of its sound layer. The object
answers the piece's calls to the game. dolphinsf2 follows these rules of the game:

- **The calls.** A piece's first track asks the game for the time between its beats, and a
  boss's music asks for its start. The game answers with 60 ticks, or 30 in two pieces. A middle
  boss starts m_boss at part 9, and the final boss starts l_boss at part 1. dolphinsf2 gives the
  same answers.
- **The directors' tracks.** Some tracks of a piece follow the game's action, such as an enemy
  nearby or a treasure carried. The game mutes them as they start. dolphinsf2 leaves out their
  notes, so each piece plays as it does with nothing going on.
- **The cave music.** A cave's piece has a conductor. The conductor picks the phrases and rests
  that each track plays, at random, and sets the track's instrument, volume and pan. dolphinsf2
  plays each piece with the first conductor of its family, such as caveconc_00_0. It draws the
  random numbers as the game does from the start. Such a piece plays until each track reaches
  its last part. The file has no loop: the game picks the phrases at random from then on. The
  game's echo leaves quiet tails between the notes. MIDI and SoundFont can't describe an echo,
  so the files don't hold them.
- **The volumes.** The game plays each piece at a volume from a list in its files. dolphinsf2
  does the same.

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
- The DSP's pitch limit. The DSP reads at most just under 8 samples of a wave for each sample of
  its output. Keys whose notes start past that limit play at the limit, as in the game. A
  separate zone plays them all at the limit, with a scale tuning of 0. A bend or a vibrato still
  moves such a note. In the game, the note stays at the limit for as long as its pitch would
  pass it.
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
- The track's filter. A music track's IIR filter becomes two zones for each region. One zone
  plays the region unfiltered. The other plays it through a low-pass. Their levels are fitted
  so that together they follow the filter's curve. The game's filter falls to a shelf above a
  few kilohertz, and a zone's low-pass alone goes on falling. The two zones double the voices
  of the filtered tracks. A track whose filter changes from note to note stays unfiltered. So
  does a filter that changes no band by 0.3 dB or more, and every sound effect's filter.
  dolphinsf2 reports the filters that it leaves out.
- Loops repeated as needed to meet SoundFont padding rules: 8 samples before, at least 32
  within, and 32 after. Playback doesn't change.

Programs 0xf0 and up play the DSP's built-in oscillators. The square wave, the sawtooth, the
pulse of 25% duty, the triangle and the sine become looping samples of one cycle, one for each
program. Each preset of such a program has one zone, for every key and velocity. The zone holds
the oscillators' envelope: full volume until the release, then a fall to silence over about
17 ms. The DSP changes its noise and its ramp as it plays. No sample can hold them.

An ADPCM wave's loop repeats from the decoder history that the wave system stores. Where that
history differs from the decoder's state at the loop start, dolphinsf2 unrolls the loop once.
The first pass and the repeats then play as in the game.

SoundFont zones can't amplify their samples. If a zone plays above full level, dolphinsf2
attenuates every zone equally and reports it. SFZ keeps the level.

### MusyX

Mario Golf: Toadstool Tour, Mario Power Tennis and Pokémon Colosseum play their music through
MusyX 2, Factor 5's sound system. Mario Party 4 plays its music through MusyX under Hudson's
sound manager. WarioWare, Inc.: Mega Party Game$!, Nintendo Puzzle Collection and the NDDEMO
disc play their music through MusyX too. The NDDEMO disc links an early build. A song group
holds the instruments. The group's pool holds the SoundMacros, keymaps, layers and tables.
The group's project gives the programs and each song's MIDI setup. A sample directory and a
sample file hold the samples. A song holds tracks of patterns: notes, controllers, program
changes and the pitch and mod wheels on 16 MIDI channels. dolphinsf2 follows these parts of
MusyX:

- The song table in the game's code. The table gives each song's file, MIDI setup and volume.
  The songs keep the table's order and names. Pokémon Colosseum's table gives each song's song
  group instead of a volume. It also gives each song one of the game's reverb presets. The game
  sets the reverb on aux A to that preset before the song starts. dolphinsf2 leaves the presets
  out. Synths play the songs' CC91 through their reverb, whatever the song. The game keeps its
  songs and song groups in FSYS archives. LZSS
  compresses some of the archives' files. dolphinsf2 names each song after its file in the
  archive.
- Mario Party 4's sound manager. One file holds every group and song. The sound manager's
  table gives each song's group, MIDI setup and volume. The sound test gives the songs' order
  and names. The Japanese release gives the names in Japanese. The sound manager keeps its
  base groups loaded. A song's macros can start their samples. dolphinsf2 adds those samples to
  the song's group.
- WarioWare's sound room. One file holds all of the game's songs. The game loads every song
  group of its MusyX project at once. The sound room's list gives each song's group, volume
  and name. The songs keep the list's order.
- Nintendo Puzzle Collection's modules. Yay0 packs each module of the game's code. A module
  holds its songs and the files of its song groups, or a table of where another file holds
  them. dolphinsf2 follows the module's relocations to the songs and the files. The menu gives
  each song a volume from a table. So does Panel de Pon's table of songs. Yoshi's Cookie plays
  its songs at the music volume that its options start at.
- The NDDEMO disc's table of paths. The disc's code lists the paths of its nine songs' files.
  One song group plays them all.
- The catalogue games: Star Fox Adventures, Paper Mario: The Thousand-Year Door, Pokémon
  Channel, the two Rogue Squadron games, Resident Evil, Resident Evil Zero, Tales of Symphonia and the two
  Metroid Prime games. Each one keeps its songs and groups in a layout of its own. dolphinsf2
  reads each game's list of songs: a table in the code, or a file of the game's. The list gives
  each song's file, group and volume. Star Fox Adventures takes each song's volume from the
  triggers that start it. Rogue Leader's music hall gives its songs' names and volumes. A song
  plays with the groups that the game has pushed when it starts the song. MusyX plays a song
  through the first pushed group with the song's group ID. It plays nothing if that group has
  no MIDI setup for the song. Resident Evil Zero's cutscenes also play some of their sound
  effects as songs. A table in the code gives each cutscene's group of effects and the ID of
  its first song. dolphinsf2 converts each of those songs and names it after the cutscene and
  the song's ID.
- Streams. Rogue Leader plays 68 of its songs from streamed audio through a MusyX voice. The
  stream's buffer is a sample of the bank. A MIDI file can't hold the stream. dolphinsf2
  leaves out the 60 songs that play only a stream and the 8 that play one along with their
  notes. Pokémon Channel's music is recorded audio that MusyX effects play. It converts as
  effects do. Paper Mario, Rebel Strike, Metroid Prime and Metroid Prime 2 stream most of
  their music. dolphinsf2 converts the songs that MusyX plays.
- The master volume. Mario Power Tennis sets MusyX's master volume for music to 101 of 127.
  WarioWare sets it to 100. MusyX multiplies each voice's volume by it, inside the volume
  curve. dolphinsf2 puts it in each CC7 the same way.
- Fade-ins. Star Fox Adventures fades each song in over 0.5 s as it starts the song.
  Resident Evil Zero's cutscenes fade their music in over 133 ms. The effects that they play
  as songs fade in the same way. MusyX's fader raises the song's volume from 0 in a line, a
  step every 5 ms. dolphinsf2 writes each step as a CC7.
- Rooms' mixes. Resident Evil's rooms set the levels and pans of some songs' channels while
  the songs play. sg7a starts with all its channels at 0 and makes no sound alone. Six more
  files play the mixes that rooms give two of the ambiences: `sg74 ch0`, `sg74 ch1`,
  `sg74 ch2`, `sg7a ch0-3+5`, `sg7a ch4` and `sg7a ch5`. Each file writes the room's CC7 and
  CC10 250 ms into the song. They come after the controllers that start the song and before
  its first note.
  Rogue Leader's music hall starts a song with a fade from the level that the song before
  left. dolphinsf2 starts each song of the hall at its volume.
- The volume table. MusyX plays a voice's volume through a table of levels in the game's code.
  A macro can pick the DLS curve instead: the volume squared. The table lies a little below
  that curve. dolphinsf2 reads the table from the game's code. For the voices on the table, it
  writes values of CC7, CC11 and the velocity whose squares follow the table.
- The pitch wheel's range. A macro can set the range over which the pitch wheel bends its
  voice. It's two semitones each way otherwise.
- The MIDI setup: each channel's program, volume, pan, reverb send and chorus send at the
  song's start.
- The sequencer. dolphinsf2 runs MusyX's sequencer once a millisecond, like the game. The
  sequencer follows the tracks and their patterns, the loops, the tempo table and the wheels.
  dolphinsf2 marks the main loop as it does for JAudio2. The NDDEMO disc's early build moves
  the sequencer's clock on in whole 65536ths of a tick. Its songs play 0.8% slower than their
  tempo says. dolphinsf2 plays them at the same speed.
- The song's volume. MusyX multiplies each channel's volume by it, inside the volume curve.
  dolphinsf2 puts it in each CC7 the same way.
- Timing and pitch. MusyX counts time and tunes its samples for an output of 32,000 Hz. The
  DSP's output runs at 32,028 Hz. In the game, songs therefore play 0.089% fast and 1.54 cents
  sharp. dolphinsf2 follows the output rate.
- The SoundMacros. A program's keymaps and layers start macros on a note. A macro is a small
  program that the voice runs. The macro's steps set up the voice: the sample, volume, pan,
  envelopes, vibrato, tremolo and key group, and what the release does. A macro can pan its
  voice by the key. It can also play its sample at a rate that it sets, whatever the key. Each
  run of keys that starts the same voice the same way becomes a zone. A macro's envelope is
  either a DLS envelope or a linear one. A DLS envelope's decay and release fall in decibels.
  A macro can repeat some of its steps (Loop), a number of times or until the note's release.
  MusyX follows at most 32 steps of a voice in each millisecond. A fade-in at the voice's start
  (FadeIn) becomes the delay and the attack of the zone's envelope. A voice that plays on after
  its note's release, until a KeyOff or until it stops, sounds until then. A fade before such a
  KeyOff and the release after it become the zone's release.
- The DSP's pitch limit. The DSP reads at most just under 4 frames of a sample for each frame
  of its output. A voice whose macro asks for more plays at that limit. Keys that start such a
  voice get a zone of their own. The zone plays at the limit, with a scale tuning of 0. A bend
  or a vibrato still moves such a note. In the game, the note stays at the limit for as long as
  its pitch would pass it.
- Pitch sweeps (PitchSweep1 and PitchSweep2). A song's note whose voice sweeps its pitch goes on
  a separate MIDI channel while channels last. That channel takes the song channel's controllers
  and bends along the sweep, through the note's release. MusyX adds the sweep to the pitch after
  the pitch wheel. The bends follow the wheel's moves the same way.
- Drums. MusyX plays MIDI channel 10 from the song group's drum programs. dolphinsf2 copies
  that channel's presets to bank 128. SoundFont synths play channel 10 from that bank.
- DSPADPCM and PCM samples, with their loops.

### MusyX's sound effects

A MusyX game starts a sound effect by its ID. The effect's row in its group's table of effects
names an object of the pool, a key, a volume and a pan. MusyX plays the object at the key. A
note of a song plays a program the same way. The game passes a volume and a pan, or takes the
row's. It can then set the effect's controllers, such as its volume or its reverb send. The
volume of the effect's volume group and the master volume for effects multiply the effect's
volume. dolphinsf2 lists the effects that each game's code starts. It plays each one as the
game starts it:

- Mario Party 4: the sound manager's table of effects. Its entries give each effect's volume,
  pan, sends and pitch. An effect's name gives its number in the table, such as `se-0036`. The
  sound test adds its name for the effect.
- Pokémon Colosseum: the effects of the sound table's five sets of effects. The game starts each
  one at full volume, with its pan in the centre. An effect's name gives its set and its ID.
- WarioWare, Inc.: Mega Party Game$!: the songs that the game plays as effects and that the
  sound room leaves out, then the effects of the game's table of effects. Each one takes the
  number that the game's play function takes for it, such as `se-0013`. The songs convert as the
  sound room's do.
- Nintendo Puzzle Collection: the effects of the menu, Dr. Mario, Panel de Pon and Yoshi's
  Cookie. Panel de Pon's and Yoshi's Cookie's tables give most effects a volume and a pan. An
  effect's name starts with its module's name and ends with its ID.
- The NDDEMO disc: the effects of the project's group of effects. The project's header names
  them.
- The catalogue games: every effect of the effects groups that the game pushes. Each one plays
  at the volume and pan that the game gives it, or else at its row's. Paper Mario and Star Fox
  Adventures name each effect after its row or its set and ID. Pokémon Channel's sound viewer
  names most of its sounds. Metroid Prime 2's string table names its effects. The others are
  named after their file or bank and their ID, such as `RebelStrike-0048`. An effect that the
  game starts only at a volume of 0 is left out.
- Mario Golf: Toadstool Tour and Mario Power Tennis: the tables of effects in the games' code.
  They list the effects of the group that the game loads at start-up, of each voice bank and of
  each narration. Mario Power Tennis's tables list the courts' effects too. That game keeps more
  effects in groups that its tables leave out. dolphinsf2 converts those as well. Each effect
  plays at its row's volume and pan. Mario Power Tennis plays its effects at a master volume of
  101 of 127 and gives each one a reverb send (CC91) of 64. An effect's name starts with its
  voice bank's name, `narration`, `court` or `se` and ends with its ID, such as `mario-0154`. A
  voice bank's macros can play objects and samples of the groups that the game loads at
  start-up. dolphinsf2 finds them there too.

The effects come after the songs. An effect's MIDI file plays every voice that MusyX starts for
it: the voices that the object starts, and the voices that their macros start. A voice's note
starts where its macro starts its sample. A macro that starts one sample after another plays
each sample in a note of its own, up to the next one's start. Each of those notes takes the
settings that the macro has made by its start. A macro can also move its voice's pitch while the
sample plays. It can change the key, or sweep the pitch (PitchSweep1 and PitchSweep2). Every
16 ms, a sweep adds a step in Hz to the rate at which the sample plays. MusyX updates the pitch
every 15 ms and at the end of each of the macro's waits. The DSP reads no more than just under 4
frames of a sample for each frame of its output. An effect's note follows its voice's pitch with
pitch bends. A macro can also glide a voice's pan (Panning). CC10 follows the pan at the same
updates. Voices that start at the same time and whose pitches and pans move together share a
note and a preset. Each note plays on a MIDI channel of its own. Synths play drums on MIDI
channel 10. The notes leave that channel free. An effect therefore plays at most 15 notes. CC7,
CC10, CC91, CC93 and the pitch wheel take the effect's controllers. A macro can also set its
voice's volume controller and sends. The voice plays at the volume controller that the macro
sets. CC91 and CC93 then take the largest send that the macros of a note's voices set. A macro
can also take its voice's reverb send from another source (ReverbSelect). MusyX takes the mod
wheel and a macro's variables as signed sources around half of full. An effect starts with its
mod wheel and variables at 0. A source of 20% of the mod wheel then sends 80% of half of full.
A variable sends half of full. CC91 takes the largest send of a note's voices. A note lasts
until its voices fall silent by themselves. A note whose voices sound on, such as a
looping sample, stops 10 seconds after the effect's last change. A JAudio sound held without
end stops the same way. The last change is the last start of one of the effect's notes, or the
last end of a note that falls silent by itself.

### The Nintendo 64's sound library

Animal Crossing and its two Japanese releases keep all their sound in one file, `audiorom.img`.
Tables in `main.dol` place the sequences, the banks and the samples in it. A sequence player runs
a script once a tick. The script starts up to 16 channels, and each channel starts up to four
layers. The layers' scripts play the notes. Each channel gets a MIDI track. A bank holds
instruments, drums and effects, and each plays a VADPCM sample with an envelope. dolphinsf2 follows
these parts of the game:

- The pieces. The game numbers its pieces, and a table in its code gives each number's sequence.
  Each piece's name is `bgm` and its number in three digits. A number that plays the same
  sequence as a lower one isn't listed again.
- The hours. A piece of the hour holds an arrangement for each kind of weather, and the game mutes
  the channels of the others. Each one gets a file: `bgm013-fine`, `bgm013-snow` and
  `bgm013-sakura`.
- The museum. Its piece holds an arrangement for each room: `bgm088-lobby`, `bgm088-paintings`,
  `bgm088-fish`, `bgm088-insects` and `bgm088-fossils`.
- The island. Doubutsu no Mori e+'s piece of the island holds an arrangement for when the islander
  is there and one for when he isn't: `bgm376-islander` and `bgm376-no-islander`.
- The music discs. The game plays them from a stereo, at a volume of 0.8 with every pan in the
  centre.
- Kapp'n's songs. Each one plays in two parts. The game tells the sequence which part follows.
- The rate. The library's updates run at about 228.8 a second in this port. They would run at 240
  a second at 60 frames a second. A sample at a tuning of 1 therefore plays at about 31,910 Hz on
  key 60.
- The envelopes and the releases. The library fades them in lines in amplitude. SoundFont's and
  SFZ's decays and releases are lines in decibels. A hold and a decay follow each fade down to
  20 dB. A release follows its line down to 14 dB. It counts from the level that an envelope
  holds at the end of the zone's notes. The median length of those notes in the piece gives that
  point. Many envelopes fall more slowly than SoundFont's slowest decay, 100 dB in 101.6 s. In the
  SoundFont files, such a zone holds for up to about 18 s and then falls at the slowest rate. The
  hold brings the zone closest to the game's fade. The SFZ files keep the fitted rate.
- Legato notes and sweeps. A layer's legato phrase plays as one MIDI note. A pitch bend on its
  channel follows the phrase's notes and the sweeps between them. A note of another layer that
  sounds at the same time bends with it.

The sound effects aren't converted. The player reports what it leaves out or plays differently
for each piece, such as a layer's bend.

### Square's Red driver

Final Fantasy Crystal Chronicles keeps each piece in a `.bgm` file in `dvd/sound/Music` and each
bank of waves in a `.wd` file in `dvd/sound/Wave`. A piece holds tracks of commands at 48 ticks a
beat. A delta time comes before each command. dolphinsf2 follows these parts of the driver:

- The tempo. The driver adds the tempo up once for each of the DSP's frames, 200.18 a second. It
  runs a tick each time that the sum passes 250.
- The notes. A note starts in the frame after its command. A key-off of its key in the same tick
  cancels it. A key-off releases each of the track's notes of its key.
- The loop. The driver saves every track at the end of the tick of the loop's start. It restores
  them at the end of the tick of the loop's end. Volumes, pans and instruments stay as they are.
  The loop's markers therefore lie one tick after those two.
- The levels. The piece's volume, the track's volume, the velocity and the region's volume are each
  lines in amplitude: a value of x gives (x + 1) / 128. A MIDI value gives the square of its
  fraction. Each value that dolphinsf2 sends is therefore a square root.
- The pans. The driver's table isn't a sine law. CC10 gives the ratio of its two sides, and CC11
  their total.
- The regions. Each one plays the keys above the region before it, up to its highest key. A stereo
  wave has a region for each side, and they play at the two sides. A region can also fix its pan.
  Its zone then leaves the track's pan out, through a CC10 modulator of 0 in place of the global
  zone's. In SFZ, its region sets `pan_oncc10=0`.
- The envelopes. An envelope starts at its first level and goes to the first of three stages with
  a time. A release falls from any level in the same time.
- The channels. A piece can have more tracks with notes than MIDI has channels. Tracks then share
  channels. The two groups of tracks that conflict least share one, until they fit. Two tracks
  conflict while their notes sound together with different levels, pans, bends or reverb sends, or
  on the same key. A preset change doesn't conflict: each note keeps the preset that it started
  with. Where tracks that conflict still share a channel, dolphinsf2 reports it.

A track's reverb depth goes in CC91. One piece, `music156`, plays its one note at a volume of 0 and
comes out silent. The driver's sound effects aren't converted.

## SFZ files

dolphinsf2 also writes SFZ files with the same instruments:

- `sfz/<sequence>/<track>.sfz`: the instruments of one MIDI track and its layers. Each file is
  named after its MIDI track.
- `sfz/samples/`: 16-bit WAV samples shared by all sequences in the game. A MusyX sample's file
  takes the sample's ID, such as `s27.wav`. A game's groups can give one ID different samples.
  The file of such a sample from a later group also gives a number for that group, such as
  `s7_27.wav`.

SFZ players ignore MIDI channels. Each file contains one instrument. Load the MIDI in a DAW and
assign an SFZ player, such as sfizz, to each track with its matching file. The player tested is
sfizz 1.2.

Presets use program change. A sequence with more than 128 presets uses bank select (CC0) too.
SFZ regions differ from SoundFont zones in these ways:

- **Envelopes.** As in the game, decays and releases fall linearly in dB. The documentation's
  curve falls by a factor of e^-8 over the specified time. That's 69.49 dB. Times are fitted as
  for SoundFont. SoundFont's limit on them doesn't apply.
- **LFO.** The vibrato is a sine wave. The game uses a sine too.
- **Pan.** A curve maps CC10 to `(value - 64) / 64`. That's the SoundFont convention that the
  MIDI output uses.
- **Level.** SFZ region `volume` restores any gain lost when scaling MIDI CC11. It also keeps
  zones above full level. Playback then keeps the game's level.
- **Track filters.** The two regions of a filtered track are fitted to SFZ's low-pass at a
  resonance of 0 dB. The documentation gives the resonance as the level at the cutoff. A
  SoundFont's resonance of 0 plays 3 dB down there in every player found.
- **Controllers.** CC7, CC10 and CC11 act at once. sfizz's default 10 ms smoothing is off. A
  note whose track sets its volume or pan in the note's starting tick begins at that level and
  pan.
- **Loops.** sfizz crossfades the loop end with data before the loop start over at least 1 ms.
  Samples include 4 ms of repeated loop data before the start. The crossfade is then inaudible.

sfizz 1.2 ends decays and releases sooner than the documentation's curve. Over the specified
time, sfizz falls by a factor of e^-9: 78.17 dB. Where the game has fallen 40 dB, sfizz has
fallen 45 dB.

During offline rendering, sfizz 1.2.3 can silence streamed samples after their preloaded
portion. The files request full loading with `hint_ram_based=1`. sfizz prints "Unsupported value
for hint_stealing: 1" for this. The message is harmless.

sfizz 1.2.3 pans a mono sample by the sin/cos law and then raises both sides by 3 dB. Every
sample of the files is mono. Every region therefore plays 3.0 dB above the game's level, at
every pan. The balance of the mix stays the same. Lowering sfizz's volume by 3 dB gives the
game's level.

A key group in sfizz 1.2.3 never cuts a note of the same key as the new note. The
documentation's `off_by` cuts it. A key of a group struck again therefore leaves the earlier
note playing in sfizz. In the game, the new note stops or releases the earlier one.

sfizz 1.2.3 makes each decay and release last at least 6 ms. The documentation sets no such
floor. A shorter time plays over 6 ms.

The opcodes follow their documentation at sfzformat.com, with these exceptions:

- **Long decays.** A decay can last longer than the documented limit of 100 s. The game's
  envelope falls that slowly. sfizz 1.2 plays the time as written.
- **Loud regions.** A region's `volume` can lie above the documented limit of 6 dB. sfizz 1.2
  takes volumes up to 48 dB.

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
  sustain and release. The fitted envelope can't follow a table that rises again or loops. A
  straight fall to silence becomes a fall in decibels. It reaches the game's level 20 dB down at
  the same time as the game's line. Before then it lies under the game's level, by up to 5.4 dB
  at about 60% of the fall. A short note that ends during such a fall plays quieter than in the
  game. Many of The Wind Waker's effects fall that way.
- **Random instruments.** An instrument's random effects vary the volume or pitch of each note.
  SoundFont presets can't vary from note to note. dolphinsf2 uses the effects' base values and
  reports them.
- **Voices.** The game plays at most 64 voices at once. When they run out, a new note takes a
  voice from an older note. The older note stops at once in the game. In MIDI, it gets a
  note-off and plays its release. Notes that get no voice are left out. dolphinsf2 reports both.
- **Vibrato.** Notes keep the vibrato speed and delay that their track has when they start.
  Depth changes work through CC1 up to a semitone. A deeper vibrato is kept to a semitone and
  reported. SoundFont's LFO is a triangle wave. The game uses a sine.
- **Omitted features.** dolphinsf2 leaves these out and reports them: tremolo, the track
  filters of sound effects and the filters that change from note to note, notes on the DSP's
  noise, ramp and other oscillators that no sample can hold, sample skips and notes on a
  missing bank.
- **Track filters.** The zones follow the game's filter within about 1 dB rms in a model of
  FluidSynth 2.6.1's low-pass. A filter that a track changes during a note keeps its value from
  the note's start.
- **Effect send.** Synths play CC91 as reverb. In the game, the send reaches whatever effect the
  game has set up. There may be none.
- **Volume limits.** JAudio2 multiplies the volumes of a track, the tracks above it and the
  sound. MIDI CC7 holds the track's volume. CC11 holds the rest. CC11 stops at 127. If
  necessary, all CC11 values are scaled equally. This keeps the balance of the tracks but lowers
  the level of the sequence. dolphinsf2 reports the reduction. SFZ restores the MIDI gain.
- **Resampling.** Synths resample waves differently from the GameCube's DSP. High frequencies
  can differ as a result.
- **The Wii's and the Wii U's rate.** JAudio times its music for the GameCube's rate of about
  32,028 Hz. The Wii and the Wii U play it at 32,000 Hz. There the music plays 0.089% slower and
  1.5 cents lower than in the files.
- **Luigi's Mansion.** After TelesaBattle's first 30 seconds, its balance lies to the right of
  the game's. Nobody has traced that yet. The game's code sets LuigiSings's tempo in the game's
  frames. In a lit room, the tempo rises from 0.5 over about ten frames. The files play the
  state's tempo from the start. Their notes come a little later than the game's at 70 and 40 HP,
  and earlier in a lit room.

For MusyX games, the files differ from the game in these ways too. dolphinsf2 reports the first
nine where a song has them:

- **Linear envelopes.** A linear envelope's decay and release fall in a straight line in
  amplitude. SoundFont and SFZ envelopes fall in decibels. A fall to silence, or to a sustain
  more than 40 dB down, is fitted to hold the same energy as the game's line. It falls about
  13 dB in the game's time. It lies up to 0.8 dB under the game's level until about 60% of the
  fall, and above it after that. A note whose decay falls that far ends where the game's decay
  does. A note whose release takes longer than its decay ends where the fitted decay has fallen
  silent. A release goes on falling after the game's has ended. Once the game's voices of a
  MIDI channel have all fallen silent, CC11 takes such a release down along the game's line,
  and CC120 (All Sound Off) stops it. CC11 then goes back to its value. A release that ends
  under a later note of its channel goes on falling. A decay to a higher sustain reaches the
  level halfway down in decibels when the game's line does. A macro's fade, while
  its note is held or after the note's release, is fitted to reach the game's level 20 dB down.
  A voice that plays on after its note's release goes on with its attack. A SoundFont note that
  ends during its attack releases from the level reached. Such a note lasts until the file's
  attack reaches the game's highest level.
- **Pitch envelopes** fall in a straight line in SoundFont and SFZ. MusyX's pitch envelopes fall
  in decibels of their range. The fit reaches half the range at the same time.
- **Volume curves.** MusyX looks up the product of a voice's volumes in its volume table.
  SoundFont and SFZ players square the velocity, CC7 and CC11 one by one and multiply the
  results. dolphinsf2 maps those values so that a note on the table starts at the table's level
  of its velocity times the channel's volumes. The zone takes the table's level of the macro's
  volume. The two levels multiply. Their product can differ slightly from the table's level of
  the whole product. Volume changes during a note follow the players' squares. In a channel
  whose notes play on both curves, CC7 and CC11 follow the DLS curve. There only the velocities
  of the notes on the table follow the table.
- **Fade-ins.** MusyX fades a voice in through its volume table. A SoundFont or SFZ attack
  rises in a straight line. The files start such an attack after a delay. It then reaches the
  level 20 dB below the fade's end when the game's fade does. With an envelope, the longer of
  the two attacks plays. A fade-in
  after the voice's start ramps from the voice's volume, not from silence. dolphinsf2 reports
  both.
- **Key groups.** In MusyX, a note of a key group releases the group's sounding voices.
  SoundFont and SFZ stop them at once.
- **Early releases.** A macro's steps can do something different for a note released before
  the macro's waits are over. dolphinsf2 plays every release as a late one.
- **Layers.** A MIDI channel holds one note per key. Overlapping notes of the same key take
  extra channels. When all 16 channels are in use, such notes end early.
- **Pitch wheel ranges.** In MusyX, each voice bends over its range. A MIDI channel bends all
  its notes over one range. dolphinsf2 gives a channel the range of its latest note. A note
  that still sounds then bends over the new range.
- **Controllers that no voice reads.** A song can send controllers that MusyX's voices ignore.
  dolphinsf2 sends them as MIDI controllers. A synth may act on them.
- **Other macro steps.** dolphinsf2 leaves out some rarer steps of a macro and approximates
  others. The report names each one.
- **A song's end.** In the game, a song's voices stop when the song ends. In MIDI, the last
  notes play their releases. These tails are quiet.
- **Envelopes from controllers.** Some macros take a voice's envelope from four of the song
  channel's controllers. Star Fox Adventures and Pokémon Colosseum have such macros. MusyX
  reads the controllers when the macro's step runs. The files take them at the note's start. A
  channel whose notes start at several sets of values plays a preset for each. An effect's
  voice leaves such an envelope out.
- **Splits by the mod wheel and the velocity.** A macro can go another way once the mod wheel
  or the note's velocity reaches a value. Star Wars Rogue Squadron II: Rogue Leader picks
  samples and envelopes that way. MusyX reads the mod wheel when the macro reaches the split.
  The files read it at the note's start. A channel whose notes start on both sides of a split
  plays a preset for each side.
- **LFOs on the pitch wheel.** A macro can swing a voice's pitch wheel with an LFO, with or
  without the channel's bends. MusyX's LFO goes on from where the voice's last LFO stopped. The
  files start it when the note starts, or after the macro's delay. A SoundFont's LFO is a
  triangle. MusyX's and SFZ's are sines. Where a macro switches the channel's bends on or off
  partway through a note, the files keep the macro's last choice for the whole note.
- **Sections that loop at different lengths.** A MusyX song can split its tracks into as many
  as 16 sections. Each section loops on its own. Where the loops differ in length, the song may
  take many passes to come back to the same place in every section, or never come back.
  dolphinsf2 marks a loop only where every section stands at the same place in its loop at the
  loop's end as at its start. It looks for that loop in the first passes of the section with
  the longest loop: as many passes as `--loops` asks for, and three more. Without such a loop,
  the file has no loop markers. It ends once every section that loops has looped as many times
  as `--loops` asks. The game plays the song on until it stops it.
- **Random choices in the Metroid Prime games.** Many of the effects of the two Metroid Prime
  games pick a random key or a random branch of their macro. dolphinsf2 plays the note's key and
  the first branch.
- **Effect sends.** CC91 and CC93 hold each channel's reverb and chorus sends. Synths play them
  as reverb and chorus. In the game, the sends reach the effects that the game sets up.
- **Sound effects.** An effect's files play it as MusyX does when the game starts it and leaves
  it alone. The game's code can change an effect's controllers while it plays, or stop it. A
  macro can change its volume while the sample plays. dolphinsf2 leaves that change out. A macro
  can also take its reverb send from a source that dolphinsf2 doesn't follow, such as a
  controller other than the mod wheel. dolphinsf2 leaves that source out and reports it. A
  song's note plays at the pitch that its macro sets last. A note whose voice sweeps its pitch
  bends on a separate channel. Without a free channel, the note leaves out its sweep. dolphinsf2
  reports that. A song's note also leaves out a change of key while its sample plays. In a song,
  a pan that a macro moves stays where it starts. dolphinsf2 reports these pans. A MIDI channel
  bends and pans all of its notes together.

SoundFont output follows the specification, apart from the zones' attenuation. Player differences
remain:

- **Level.** FluidSynth runs the SoundFont low-pass filter on every note. dolphinsf2's zones
  leave that filter open. The filter still divides the level by the square root of its Q.
  FluidSynth converts the default resonance of 0 dB to a Q of 0.707. Every note therefore plays
  1.5 dB louder than the SoundFont specifies. Multiplying FluidSynth's gain by 0.84 gives the
  specified level.
- **Pan.** JAudio2's pan law is FluidSynth's sin/cos law. TinySoundFont uses a square-root law.
  Its pans come out slightly narrower.
- **Envelopes.** Times follow the specification's 100 dB range. FluidSynth uses 96 dB. Decays
  and releases then fall 4% slower in decibels. Sustain levels are 4% shallower: -11.9 dB plays
  as -11.4 dB. FluidSynth also slows any release faster than 15.6 ms (-7200 timecents) to that
  time.
- **Attenuation.** FluidSynth, BASSMIDI and the E-mu chips apply 0.4 of a zone's initial
  attenuation. The specification calls for all of it. dolphinsf2 therefore writes 2.5 times a
  zone's attenuation in the generator, up to the generator's limit of 1440 cB. Those players
  play that limit as 57.6 dB. A modulator without a source holds any attenuation beyond 57.6 dB.
  FluidSynth 2.4.5 and later apply that modulator. BASSMIDI and earlier versions of FluidSynth
  ignore it. A player that follows the specification plays every attenuated zone too quietly.
  TinySoundFont plays a quarter of the attenuation. Before 27 January 2025, TinySoundFont played
  2.5 times the attenuation.
- **Pitch.** FluidSynth rounds pitch down to whole cents. Fractional bends can be almost a cent
  flat. The beating of notes detuned by less than a cent changes too.
- **Modulators.** BASSMIDI 2.4.16 applies only some SoundFont modulators. TinySoundFont applies
  none. Neither applies the CC1 and CC10 modulators. MusyX songs then pan wider than the
  SoundFont sets. In BASSMIDI, CC1 gives the specification's default vibrato: 50 cents at CC1
  127. In TinySoundFont, CC1 vibrato depth has no effect.
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
`git clone --recursive`, or run `git submodule update --init` in a checkout. Compressed disc
images require four more libraries: zlib, Zstandard, liblzma and bzip2. CMake uses the installed
copies where it finds them. It downloads the others when it configures the build. The build then
compiles them and links them into dolphinsf2. The download requires an internet connection.
dolphinsf2 supports macOS on Apple Silicon and Windows x64 with clang-cl. CI builds both. It
runs the tests on Windows.

- **Windows:** install Visual Studio 2022 with "Desktop development with C++". That workload
  includes CMake. Add the "C++ Clang tools for Windows" component. Use Developer PowerShell or
  Developer Command Prompt. CMake usually downloads all four compression libraries.
- **macOS:** install Xcode command line tools (`xcode-select --install`) and CMake
  (`brew install cmake` or the installer from cmake.org). macOS includes zlib and bzip2.
  CMake downloads Zstandard and liblzma unless they come from Homebrew (`brew install zstd xz`).

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

`ctest` runs unit tests without game data. The tests build MusyX songs in memory. They cover
song lists and file paths, DLS envelope times, the song's volume and a game's master volume,
MusyX's volume table, tempo tables, the early build's clock, and a song that loops. They also
unpack Yay0 data and read a module of code. They build a piece of the Nintendo 64's sound library
and pieces of Square's Red driver too, with their banks, and cover the fit of a fade. The JAudio
conversion is part of ntdlib, and ntdlib's tests cover it. Those tests build archives, banks, wave systems and
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
the Unicode License v3. For compressed disc images, the program uses zlib (zlib licence),
Zstandard (BSD-3-Clause), liblzma (0BSD) and bzip2 (a BSD-style licence). The Windows program
contains them. `THIRD-PARTY-NOTICES.md` gives their licence texts, and the release zip
includes it. Game data is not covered by this licence.
