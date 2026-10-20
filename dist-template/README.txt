HARVEST MOON: BACK TO NATURE - NATIVE PC PORT
=============================================

This is a native PC (Windows / Linux) build of Harvest Moon: Back to Nature (USA, SLUS-01115),
made from your own copy of the game. The PlayStation game code was statically recompiled into
native x86-64 code - there is no emulator and no PlayStation BIOS involved. The PlayStation
hardware the game talks to (graphics chip, sound chip, CD drive, controller port, memory card,
geometry coprocessor, timers) and the BIOS services it uses are reimplemented natively so the
game behaves exactly like the original.

RUNNING
-------
Run HarvestMoonBTN.exe (Windows) or ./HarvestMoonBTN (Linux).

Linux: requires SDL2 (normally already installed; otherwise e.g. "sudo apt install
libsdl2-2.0-0", "sudo dnf install SDL2" or "sudo pacman -S sdl2"). Make the file executable
if needed: chmod +x HarvestMoonBTN

{{GAME_DATA}}

CONTROLS
--------
                 Keyboard                 Xbox controller
  D-pad          Arrow keys               D-pad / left stick
  Cross (X)      X / Space / K            A
  Circle (O)     Z / L                    B
  Square         S / J                    X
  Triangle       A / I                    Y
  L1 / R1        Q / W                    LB / RB
  L2 / R2        1 / 2                    LT / RT
  Start          Enter                    Start (Menu)
  Select         Right Shift / Backspace  Back (View)

  F11 or Alt+Enter   toggle fullscreen         Esc   leave fullscreen
  F12                screenshot (screenshots folder)
  Tab (hold)         fast-forward (can be disabled in hmbtn.ini)

All keys and buttons can be remapped in hmbtn.ini. Controllers can be plugged in at any time.

SAVES
-----
Saving works exactly as on the PlayStation: go to bed in your house and pick one of the three
diaries (Continue on the title screen loads them). The memory card is stored in
saves\card1.mcd - a standard 128 KB raw memory card image, compatible with DuckStation, ePSXe,
PCSX and others, so you can copy an existing card here to continue an old game, or take your
card to an emulator. If the game folder is not writable, the card is kept in
%APPDATA%\HarvestMoonBTN (Windows) or ~/.local/share/HarvestMoonBTN (Linux).

NOTES
-----
* Video runs at the original NTSC rate (59.29 fps) with the original 320x240 picture, scaled up.
* A log file (HarvestMoonBTN.log) is written next to the executable - useful if something
  goes wrong.
* This folder is for your personal use only - do not share it. The executable contains the
  game's program code translated from your copy, which, like the game's data, remains the
  property of its copyright holders (Victor Interactive Software / Marvelous, Natsume).
  SDL2 is used under the zlib license (LICENSE-SDL2.txt).
