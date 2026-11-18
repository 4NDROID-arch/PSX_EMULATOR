
# VIZO PSX

A lightweight PlayStation 1 emulator for Android, powered by PCSX-ReARMed.

## Features

- Play PS1 games on Android.
- ARMv7 (32-bit) support.
- Landscape fullscreen gameplay.
- On-screen virtual controller.
- Multitouch controls.
- Support for CHD and CUE/BIN game formats.
- Save and load game states.
- Independent save states for each game.
- Customizable HUD controls.
- FPS counter.
- Optimized for low-end Android devices.

## Screenshots



## Requirements

- Android 5.0 or later.
- ARMv7 (armeabi-v7a).
- At least 2 GB RAM recommended.

## Installation

1. Download and install the APK.
2. Create the following folders:

   `/sdcard/PSX/games/`

   `/sdcard/PSX/bios/`

   `/sdcard/PSX/saves/`

3. Add your legally obtained PS1 game files to the games folder.
4. Launch VIZO PSX and select a game.

## Supported Formats

- `.chd`
- `.cue`
- `.bin` (used with its matching CUE file)

Make sure CUE files reference the correct BIN filenames.

## Save States

Each game has its own save-state file.

Example:

`save1_state_Crash_Bandicoot.vst`

`save1_state_Crash_Team_Racing.vst`

## Native Libraries

The project uses:

- PCSX-ReARMed Libretro Core
- Custom JNI Bridge
- Android NDK

ABI:

`armeabi-v7a`

## Project Structure

```text
jni/
├── psxbridge.c
├── libretro.h
├── Android.mk
└── Application.mk

app/src/main/jniLibs/armeabi-v7a/
├── libpcsx_rearmed_libretro_android.so
└── libpsxbridge.so
```

## Disclaimer

VIZO PSX is an independent project and is not affiliated with Sony Interactive Entertainment.

PlayStation is a trademark of Sony Interactive Entertainment.

This project does not include copyrighted games or BIOS files. Users must provide their own legally obtained files.

## Developer

**VIZO XITS**
