# Prime Target — static recompilation

Native Windows port of *Prime Target* (WizardWorks/MacSoft, 1996), a Mac
PowerPC game on a licensed Marathon 2 engine. The original PowerPC code is
translated to C ahead of time and runs against a reimplementation of the
classic Mac Toolbox. No emulator, no Apple ROM.

Target binary: **Prime Target v1.1** (`Prime Target v11`), chosen because it
still carries CodeWarrior traceback tables with names for all 1,295 game
functions. The v1.2 builds are stripped.

## Layout

| Path | What |
|---|---|
| `tools/extract_game.py` | Copies the HFS volume (`prime.hfv`) to `game/`: data forks, `.rsrc` resource forks, `finder.tsv`/`folders.tsv` with Mac names and type/creator |
| `tools/rip_cd_audio.py` | Splits the BIN/CUE into `cdaudio/trackNN.wav` + `toc.tsv` (the soundtrack is Red Book audio) |
| `tools/pef.py` | PEF loader: sections, pattern-initialised data, relocations, imports |
| `tools/analyze.py` | Instantiates the image at fixed guest addresses, finds functions from traceback tables → `build/image/` |
| `tools/recomp.py` | PowerPC → C lifter → `generated/` |
| `tools/ppcdis.py` | Disassembler for inspection (TOC loads and calls annotated) |
| `runtime/` | C++ runtime: guest memory, dispatch, and the Toolbox HLE modules |
| `game/`, `cdaudio/`, `user/` | Extracted data, ripped music, writable "Macintosh HD" (prefs). Not in git. |

### Guest address space (`runtime/include/pt/layout.h`)

| Range | Use |
|---|---|
| `0x00000000` | low-memory globals |
| `0x01000000` | code section (0x6D5B0 bytes) |
| `0x01100000` | data section (0x4E418 bytes; TOC at `0x01108000`) |
| `0x01200000` | import TVectors (8 bytes each) → magic code addresses at `0x01300000` |
| `0x02000000`–`0x0A000000` | Memory Manager heap |
| `0x0C000000` | runtime-owned guest objects |
| `0x0F000000`–`0x0F100000` | stack |

## Build

```sh
python -m venv .venv && .venv/Scripts/python -m pip install machfs capstone
.venv/Scripts/python tools/extract_game.py ../PrimeTarget/Prime/prime.hfv game
.venv/Scripts/python tools/rip_cd_audio.py ../PrimeTarget/Prime/PrimeTarget.cue cdaudio
.venv/Scripts/python tools/analyze.py "game/Prime Target/Prime Target v11" build/image
.venv/Scripts/python tools/recomp.py build/image generated
cmake -S . -B build/msvc -G "Visual Studio 18 2026" -A x64
cmake --build build/msvc --config Release --parallel
```

Run with the extracted game:

```sh
PT_GAME_DIR="P:\Recomp\Prime Target\ptrecomp\game" build/msvc/Release/primetarget.exe
```

`PT_LOG` selects log channels: `hle` (every Toolbox call), `unimpl` (default),
`mem`, `file`, `res`, `gfx`, `snd`, `evt`, `all`. On a crash the runtime
prints the faulting guest address and the last 32 functions entered.

## How the recompiler works

* Function boundaries come from the traceback tables (exact start via
  `tb_offset`). The 40 KB CodeWarrior runtime at the end of the code section
  has no tables; its entry points come from call targets and from TVector
  pairs (code pointer + TOC) in the data section. 398 of those are the
  import-glue stubs.
* Branches inside a function become `goto`; `bl` becomes a C call; a `b` to
  another function is a tail call; `blr` is `return`.
* `bctr` becomes a `switch` over the function's jump-table targets (code
  pointers relocated into the data section that land inside the function),
  falling back to indirect dispatch. Every `bctr` in the game code has a table.
* Imports are called through CodeWarrior's glue (`lwz r12,…(r2); … bctr`) like
  any other indirect call; the TVector's code address is a magic value that
  the dispatcher routes to the HLE implementation.
* Anything the lifter cannot translate stops generation (`lift_errors.txt`).
  Currently: 0 errors over 1,799 functions.

## Toolbox status

Implemented: Memory Manager, File Manager (FSSpec, refNum and PB calls over a
virtual HFS), Folder Manager, Resource Manager (read + write-back), Gestalt,
SysEnvirons, time, string utilities, Process Manager info, `.AppleCD` driver
(TOC from the real disc, playback state), Sound Input Manager, Mixed Mode,
Time Manager (tasks fire at Toolbox-call safe points).

Not yet: QuickDraw / graphics devices and the display, Window / Menu / Dialog
/ Control managers, events and input, Sound Manager and CD-audio output,
QuickTime cutscenes, AppleTalk (reported absent, so network play is off).

Boot progress: runs through start-up, preferences, map/physics file opening
and CD-music setup, then stops at the display check ("requires a 13" monitor
… 256 colors") because no graphics devices exist yet.
