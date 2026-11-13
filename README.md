# Prime Target — static recompilation

Native Windows port of *Prime Target* (WizardWorks/MacSoft, 1996), a Mac
PowerPC game on a licensed Marathon 2 engine. The original PowerPC code is
translated to C ahead of time and runs against a reimplementation of the
classic Mac Toolbox. No emulator and no Apple ROM; system fonts and cursors
come from your own Mac OS disk image, copied into `system/` once.

Target binary: **Prime Target v1.2**, the version on the CD (`Prime Target`;
`Prime Target LM` has the same code). It has no symbols, so its function
boundaries and names come from `symbols/prime_target_v12.tsv`, carried over
from the v1.1 update, which still has CodeWarrior traceback tables naming every
function (see [v1.2 versus v1.1](#v12-versus-v11)). v1.1 itself still builds
from its tables alone.

## Status

Boots through the intro movie and main menu, opens Preferences and the file
dialogs, plays the prologue and runs the levels at the engine's native 30 ticks
per second with input, sound effects, the CD-audio soundtrack and QuickTime
movies. All 399 imported Toolbox routines have implementations.

Not done or deliberately absent:

* Network play: AppleTalk is reported as absent, so Gather/Join are unavailable.
* Printing: there is no printer.
* QuickDraw regions are rectangles (their bounding box); window overlap is
  handled separately with a per-pixel ownership map.
* Movies are mapped to the screen palette by nearest colour, not dithered.
* TextEdit and the List Manager are minimal (dialog edit fields work).
* Mouse look and a full level play-through are not yet verified.

## Layout

| Path | What |
|---|---|
| `tools/extract_game.py` | Copies the game off the CD (BIN/CUE or ISO, or an installed HFS volume) to `game/`: data forks, `.rsrc` resource forks, `finder.tsv`/`folders.tsv` with Mac names and type/creator |
| `tools/extract_system.py` | Copies the System file's resource fork and the `Fonts` suitcases from a System 7 image to `system/` (cursors, patterns, bitmap fonts) |
| `tools/rip_cd_audio.py` | Splits the BIN/CUE into `cdaudio/trackNN.wav` + `toc.tsv` (the soundtrack is Red Book audio) |
| `tools/pef.py` | PEF loader: sections, pattern-initialised data, relocations, imports |
| `tools/analyze.py` | Instantiates the image at fixed guest addresses and finds every function (traceback tables or a symbol file, plus the runtime library's call graph) → `build/image/` |
| `tools/make_symbols.py` | One-off: matches a stripped build against v1.1 and writes its symbol file |
| `symbols/` | Function boundaries and names for v1.2, with how each differs from v1.1 |
| `tools/recomp.py` | PowerPC → C lifter → `generated/` |
| `tools/ppcdis.py` | Disassembler for inspection (TOC loads and calls annotated) |
| `runtime/` | C++ runtime: guest memory, dispatch, and the Toolbox modules |
| `game/`, `system/`, `cdaudio/`, `user/` | Extracted game data, Mac OS resources, ripped music, writable "Macintosh HD" (prefs). Not in git. |

Runtime modules (`runtime/src`): `memory` (Memory Manager), `vfs`/`files`
(virtual HFS, File/Folder Manager), `resources` + `hfs_reader` (Resource
Manager; System file and font suitcases from `system/`, or a disk image),
`qd_core`/`qd_blit`/`qd_pict`/`qd_text` (Color QuickDraw, CopyBits, PICT,
fonts), `windows`, `menus`, `dialogs` (Dialog/Control/TextEdit/List/Scrap/
Print), `events`, `sound`, `drivers` (.AppleCD, video driver, Sound Input),
`movies` + `cinepak` (QuickTime), `sfile` (Standard File), `timer` (Time
Manager, VBL, Mixed Mode), `mathlib`, `os_misc`, `host` (SDL2).

### Guest address space (`runtime/include/pt/layout.h`)

| Range | Use |
|---|---|
| `0x00000000` | low-memory globals |
| `0x01000000` | code section (0x6D5B0 bytes) |
| `0x01100000` | data section (0x4E418 bytes; TOC at `0x01108000`) |
| `0x01200000` | import TVectors (8 bytes each) → magic code addresses at `0x01300000` |
| `0x02000000`–`0x0A000000` | Memory Manager heap |
| `0x0C000000` | runtime-owned guest objects (framebuffer, ports, devices) |
| `0x0F000000`–`0x0F100000` | stack |

## Build

```sh
python -m venv .venv && .venv/Scripts/python -m pip install machfs capstone
.venv/Scripts/python tools/extract_game.py ../PrimeTarget/Prime/PrimeTarget.cue game
.venv/Scripts/python tools/extract_system.py ../PrimeTarget/Prime/macos7.hfv system
.venv/Scripts/python tools/rip_cd_audio.py ../PrimeTarget/Prime/PrimeTarget.cue cdaudio
.venv/Scripts/python tools/analyze.py "game/Prime Target/Prime Target" build/image --symbols symbols/prime_target_v12.tsv
.venv/Scripts/python tools/recomp.py build/image generated
cmake -S . -B build/msvc -G "Visual Studio 18 2026" -A x64
cmake --build build/msvc --config Release --parallel
```

CMake downloads the official SDL2 2.30.9 Visual C++ package. `analyze.py`
checks the application's SHA-1 against the symbol file, so a different build
is refused rather than mistranslated.

To build v1.1 instead (from an installed copy that has the update), point
`analyze.py` at `Prime Target v11` without `--symbols`.

## Run

```sh
build/msvc/Release/primetarget.exe
```

The runtime finds `game/` next to the executable or in a parent folder, and
`system/` beside it. If `system/` is missing it reads the System file straight
from `../PrimeTarget/Prime/macos7.hfv`. Override with `PT_GAME_DIR`,
`PT_SYSTEM_DIR` and `PT_SYSTEM_HFV`. `PT_SCALE` sets the window
scale (default 2); Alt+Enter toggles full screen.

Keys map to the Apple Extended Keyboard: Left Alt is Command, Right Alt /
Windows is Option, Ctrl is Control. The game's default controls are
Marathon's keypad layout (8/5 move, 4/6 turn), changeable in Preferences.

### Debugging aids

| Variable | Effect |
|---|---|
| `PT_LOG` | log channels: `hle` (every Toolbox call), `unimpl` (default), `mem`, `file`, `res`, `gfx`, `snd`, `evt`, `all` |
| `PT_WATCH` | comma list of guest function names to log on entry (with r3–r6 and LR) |
| `PT_SHOT_DIR` | write a BMP screenshot every 2 s (plus palette statistics) |
| `PT_RUN_SECONDS` | exit after N seconds |
| `PT_INPUT` | scripted input: `"ms op args; ..."` with `move X Y`, `click X Y`, `down`, `up`, `key NAME`, `hold NAME`, `release NAME` (names: letters, `kp0`–`kp9`, arrows, `return`, `space`, `shift`, `ctrl`, `option`, `cmd`) |
| `PT_AUDIO_DUMP` | raw 44.1 kHz stereo s16 capture of the mixer output |

On a crash the runtime prints the faulting guest address and the last 32
guest functions entered.

## v1.2 versus v1.1

`make_symbols.py` places 1,267 of v1.1's 1,295 functions unchanged in v1.2
(the network code moved to the end of the link order), 28 changed or
rewritten, and finds one new helper. The `difference` column of the symbol
file records which. Reading the changed code, v1.2:

* clears the monster and weapon tables before reading a saved game (the save
  then restores its own; v1.1 could carry state over from the game in progress);
* checks doors, switches and access cards against the player who pressed the
  action key, not the local player (matters in network games);
* guards inventory cycling and item use when the player has no items;
* doubles the delay before a dead player can respawn (1 s to 2 s);
* reworks the per-damage-type multipliers applied to damage the player takes
  (which cases are halved or doubled, plus a new 4× case);
* removes dead monsters' and players' bodies outright in network games (the
  flag Marathon 2's source calls `game_is_networked`);
* drops an eye-height adjustment in the physics setup and rewrites
  `closest_point_on_circle`, `build_links_from_objects` and
  `initialize_player_weapons`;
* locks shape pixmaps high in the heap (`HLockHi`, its one new import), part
  of the low-memory work behind `Prime Target LM`.

## How it works

* **Function boundaries** come from the traceback tables (exact start via
  `tb_offset`) or, for v1.2, the symbol file. The 40 KB CodeWarrior runtime at the end of the code section
  has no tables; its entry points come from call targets and TVector pairs
  (code pointer + TOC) in the data section. 398 of those are import glue.
* **Control flow**: branches inside a function become `goto`; `bl` becomes a
  C call; a `b` to another function is a tail call; `blr` is `return`;
  `bctr` is a `switch` over the function's jump-table targets (code pointers
  relocated into the data section), falling back to indirect dispatch.
* **Imports** are called through CodeWarrior's glue like any indirect call;
  the TVector's code address is a magic value routed to the HLE routine.
* **Interrupts**: every loop back-edge is a safe point (`PT_SAFEPOINT`). A
  1 kHz host thread raises a flag and the game thread then runs Time Manager
  and VBL tasks, sound completions and input/presentation, much as the Mac
  ran them at interrupt time but single-threaded. The engine's 30 Hz
  heartbeat is a drift-free extended Time Manager task.
* **Display**: QuickDraw's device colour table and the hardware palette are
  separate, as on real hardware, so the engine's video-driver fades don't
  disturb colour mapping. Each present converts the framebuffer through the
  live hardware palette.
