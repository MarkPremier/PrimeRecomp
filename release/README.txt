PRIME TARGET - native Windows port
==================================

Prime Target (WizardWorks/MacSoft, 1996) was a Macintosh-only first-person
game on Bungie's Marathon 2 engine. This is a static recompilation of the
original PowerPC program: the game's own code, translated to run natively on
Windows. No emulator, no Mac ROM.

You need your own copy of the game.


GETTING STARTED
---------------

1. Unzip primetarget.exe and SDL2.dll into a folder of their own. The folder
   needs about 400 MB free.

2. Put your Prime Target CD image in the same folder, then run
   primetarget.exe. (You can also drag the image onto primetarget.exe, or
   just run it and pick the image when asked.)

   Best: a BIN/CUE image of the whole CD. The game's music is CD audio, so
   only a BIN/CUE has it. A data-only image (.iso, .toast, .img) also works,
   but the game will be silent apart from sound effects.

3. The first run copies the game into "game" and "cdaudio" folders beside
   the program (a few seconds). After that the CD image isn't needed.

The port is built from version 1.2 of the game, the version on the retail CD.
Other versions (such as the 1.1 update or the demo) are refused.

Settings and saved games stay in the program's folder ("user" and "game").


CONTROLS
--------

The game starts with Marathon's "Standard" keypad layout (the CD's Read Me
mentions "PC Doom" keys, but the program ships set to Standard):

    Keypad 8 / 5        forward / backward
    Keypad 4 / 6        turn left / right
    X / C               slide left / right
    A / Z               jump / crouch
    Control             fire
    Space               action (doors, switches, buttons)
    Tab                 overhead map (+ and - zoom it)
    Shift               run
    Left Alt            sidestep (hold while turning)
    Right Alt           look (hold while moving)
    Keypad - / +        look up / down; Enter looks ahead
    Keypad 7 9 1 3      look up-left, up-right, down-left, down-right
    S / D               glance left / right
    0 - 9               choose weapon
    Return              use the current inventory item
    \                   show inventory items; [ and ] select previous / next
    F1 - F12            shortcuts to the preferences

Preferences > Controls > Configure Keyboard changes any of them (its Layout
menu has the alternatives), and Preferences > Controls turns on mouse control.

Mac keys: Left Alt is the Command key; Right Alt or the Windows key is
Option. Alt+Enter toggles full screen.

Full screen: Preferences > Graphics > Window Size > Full Screen is the original
view without the control panel at the bottom; the window then fills your
monitor too. To fill the monitor with the control panel showing, press
Alt+Enter (or set fullscreen = 1 in primetarget.ini, below).

While the game window is active the mouse pointer stays inside it, as it did
on the Mac's full screen. Alt+Tab to get the pointer back for other programs.

Mouse look: Preferences > Controls > Mouse and Keyboard, "Mouse controls view".
The original game turned the mouse into key presses (full-speed turning or
none) and its up/down look flipped back and forth; this port drives the
engine's smooth Marathon 2 mouse look instead. Pushing the mouse forward looks
up.

Frame rate: the game world runs 30 times a second, as on the Mac. The port
draws extra frames in between (up to your display's refresh rate) with the
camera blended between updates, so turning and moving look smooth; monsters
and other objects still move 30 times a second. Preferences > Graphics >
Smooth Motion switches between this and the original 30 frames per second.


OPTIONS
-------

The first run writes primetarget.ini beside the program. Remove the ; in front
of a line to change that option:

    mouse_speed = 1.5       faster mouse look (0.5 slower; default 1)
    mouse_invert = 1        pushing the mouse forward looks down
    mouse_lock_y = 1        the mouse only turns left and right; look up and
                            down with the keys
    original_mouse = 1      the original game's key-press mouse
    interpolate = 0         the original 30 frames per second (also the
                            Smooth Motion checkbox in Preferences > Graphics)
    max_fps = 60            cap the frame rate (default: the display's)
    vsync = 0               don't wait for the display's refresh (may tear)
    scale = 3               a bigger window (640x480 times this; default 2)
    mouse_grab = 0          let the pointer leave the window
    fullscreen = 1          start filling the monitor

Tip: Preferences > Graphics > Number of Colors > Thousands draws the game in
16-bit colour instead of 256 colours.


FONTS
-----

The classic Mac fonts belong to Apple and are not included. The program draws
text with similar Windows fonts instead. If you have a Mac OS 7.x disk image
from SheepShaver or Basilisk II (a .hfv or .dsk file), put it beside
primetarget.exe and the genuine Mac fonts are used.


KNOWN GAPS
----------

- Network play is unavailable.
- "Hardware Acceleration" stays greyed out, as on most Macs of the time (it
  needed one specific video chip and drew the same picture as Thousands of
  colours).
- Movies use the 256-colour palette by nearest colour, without dithering.


PROBLEMS
--------

If the program reports an error it shows a message box. When reporting a bug,
say what you were doing and include the message.

Uses SDL2 (zlib licence).
