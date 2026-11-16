VIRTUA NBA - RECOMP for Linux x86_64 and SteamOS  (Made by Pocket_lucho with Claude)
====================================================================================

If you want to support me: https://buymeacoffee.com/pocket_lucho

REQUIREMENTS: nothing to install. Works on SteamOS (Steam Deck, Legion Go S...), Ubuntu,
Fedora, Arch, etc. If the system has no C++ compiler, build_game.sh downloads a portable
one (Zig, ~45 MB) the first time, inside this folder. No sudo needed.
An internet connection is only needed the first time if you do not have g++.

SETUP
1. Copy your virnba.zip romset to the "roms" folder.
2. Copy the NAOMI BIOS naomi.zip to the "data" folder (only needed the first time).
3. Open a terminal in this folder and run ONCE:   ./build_game.sh
   It generates the game code from YOUR ROM and compiles it into libvnba_game.so.
4. To play run:   ./VirtuaNBA.sh
   (if it fails, the reason is shown in a window and saved to vnba.log)

STEAMOS (Steam Deck / Legion Go S)
1. Switch to Desktop Mode (Steam button > Power > Switch to Desktop).
2. Extract the .tar.gz (for example to ~/Games/VirtuaNBA-Recomp) and copy the ROM and BIOS.
3. In the file manager (Dolphin), inside the folder: right click > "Open Terminal Here"
   and type:
       chmod +x build_game.sh VirtuaNBA.sh VirtuaNBA-Recompiled
       ./build_game.sh
4. Test the game with ./VirtuaNBA.sh
5. To play in Game Mode: open Steam on the desktop > "Add a Game" > "Add a Non-Steam Game"
   > Browse > pick VirtuaNBA.sh (set the filter to "All Files"). Go back to Game Mode and it
   will be in your library. The gamepad works directly; the settings menu opens with the
   Tab key or the button mapped in the controls options.

This package contains nothing from the game. libvnba_game.so and data/vnba_boot_*.state are
generated from your files: do not share them. GPL v2 license (based on Flycast).
Source code: https://github.com/PocketLucho/Virtua-NBA---Recompilation
