# RCRness

## What's new in this fork

- **Widescreen co-op fixes.** Fixed whole fighters flickering on the opening street and moving back and forth as the camera scrolled. Fighters stay visible as they cross the original viewport edge.
- **Desktop pause menu.** Press **Esc** to pause local play, even inside the NES inventory screen. Use the keyboard or either controller to resume, change options, or quit. **Esc → Save states** lists the 12 local slots shared with the hotkeys. Select a slot to save or load. **Shift+F1–F12** saves and **F1–F12** loads. **Start** still opens the original menu for inventory, stats, and passwords.
- **Widescreen wallets.** Each active fighter’s cash appears beside their name and stamina bar. Narrow Fit windows show it beneath the name.
- **Display options.** Open **Esc → Options** to compare widescreen settings, the CRT filter, and sprite-flicker reduction on the frozen scene.
- **Per-player controls.** Open **Esc → Controls** to see or change either player's keyboard and controller bindings. You can swap conflicting inputs and restore one player's defaults.
- **Persistent bodies.** Enable **Esc → Options → Persistent bodies** to leave defeated enemies on the street. Coins still drop and can be collected. The game remembers the last 48 bodies across streets and includes them in save states. This option defaults to Off and works with blood effects on or off.
- **Chainsaw.** Find it on the opening street in local play. Stand over it and press Punch to pick it up. The fighter carries it at torso height. Press Punch to swing it forward or Kick to throw. With **Blood effects** enabled, each chainsaw hit sprays 640 droplets. Enable **Persistent bodies** too, then face a fallen enemy in the same lane and swing to cut the body apart. To spawn one at Alex’s feet, launch `./build-public/RCRness "$rom" --spawn-chainsaw`. Trace builds also accept the live debug command `{"id":1,"cmd":"rcr_spawn_chainsaw"}`.
- **Optional blood effects.** Turn on **Esc → Options → Blood effects** for pixel spray when a fighter takes damage. Weapon hits throw a larger, wider spray. Landed blood accumulates permanently on each street, including across room changes, save-state loads, new games, and relaunches. Turning the effect off hides the stains. The game keeps them in `rcr-blood-v1.bin` next to `config.ini`. In local play, a lethal weapon hit on an enemy can knock their head loose with a much larger spray. The head bounces onto the street. Each street can hold eight extra loose weapons or heads alongside the three NES weapon slots. Walk up to a loose item to bring it within pickup range. Stand over it and press Punch to pick it up, then Punch to swing or Kick to throw. The setting defaults to Off.
- **Character customization.** In **Esc → Characters**, choose hair and eyes independently for Alex and Ryan from the players, street fighters, bosses, townspeople, and shopkeepers. Both fighters also have separate shirt, pants, and hair colors. Turn on **Oni mode** to dress both players as Konoko from the game Oni by Bungie software.
- **City map.** Open **Esc → Map** for a connected atlas of all 35 areas. Each label sits beneath a scaled panorama of the entire playfield. A gold outline marks your location, blue frames mark malls, and a blinking red diamond shows the next required boss. The **Next** number identifies the destination even when it is outside the zoomed view. Use the arrows or D-pad to explore, **Tab** or controller **X** to zoom, and **Enter** or controller confirm to return to your area.

## About RCRness

Public, ROM-free NESRecomp project for a user-owned **River City Ransom (USA)**
ROM. This repository intentionally contains no ROM, generated game code,
playable executable, save data, captures, Ghidra project, or other copyrighted
game data.

Because NESRecomp is a static recompiler, a playable `RCRness.exe` contains
code generated from the user's ROM. Public releases therefore distribute the
clean source/build kit only. Each user generates and builds the playable
executable locally from a legally obtained ROM.

Expected ROM identity:

- iNES mapper 4 (MMC3), 8 x 16 KiB PRG, 16 x 8 KiB CHR, no trainer
- SHA-256: `B95F8DEF04C0012604B7CC68CF4ED8F49B2DFE4BEDA6182051DE4B2423BEA7B1`
- Header-excluded CRC32: `E9C387EC`

## Experimental enhanced renderer and adaptive widescreen

### v0.1.7 scenery and sprite fixes

Sixteen more gameplay areas now show their original two-page scenery in
widescreen. Original movement and collision boundaries are preserved;
single-screen scenes and shops retain their native composition. Carried
enemies use the correct graphics, and coins or other omitted sprites no
longer remain as stale images. The v0.1.6 shop-freeze fix is included.

Download the updated setup and choose **Rebuild** once. **Play Previous Build**
keeps your old executable and will not apply these fixes.

The adaptive widescreen mod is **experimental**. Full game progression, all
shop and boss prerequisites, physical two-controller play, and netplay have
not been fully validated. Use `--widescreen off` for the native view.

The local enhanced build defaults to **Fit to window**. Scrolling areas show
real terrain and complete actors beyond the original screen, and the viewport
follows live window resizing up to 32:9. Hitboxes, actor activation and authored
spawn selection account for the wider view. Authored single-screen rooms and
shops retain their native bounds. See [the widescreen guide](docs/WIDESCREEN.md)
for controls, reproducible builds, object handling and validation limits.

Existing Visual Studio 2022 installations can build with:

```powershell
.\build.ps1 -Rom '.\roms\River City Ransom (USA).nes' -UseVisualStudio
```

Both build paths apply the tracked renderer source patch to the pinned engine
automatically. Generated game code and playable builds remain private.

## Linux and macOS setup releases

Version 0.1.5 also includes native **Linux x86-64**, **macOS Intel x86-64**
and **macOS Apple Silicon arm64** setup applications. These are ROM-free setup
hosts: each player selects their own supported ROM and builds the playable game
privately. First build requires an internet connection. Later launches offer
**Play Previous Build** and **Rebuild**.

On Linux, extract `RCRness-Setup-Linux-x86_64.tar.gz` and run `RCRness-Setup`.
Ubuntu/Debian setup can request system authorization through PolicyKit to
install Git, a C compiler, CMake, Ninja and SDL2 development files. On other
distributions, install those dependencies using the system package manager.
The binary targets glibc 2.35 or newer (Ubuntu 22.04 or newer).

On macOS, extract the archive for your CPU and open `RCRness-Setup.app`.
Install [Homebrew](https://brew.sh) first; setup uses it to install CMake,
Ninja and SDL2. Apple Command Line Tools are required; setup opens Apple's
installer when they are missing. Reopen setup after completing installation.
The app is unsigned and not notarized. Use macOS **Open Anyway** for the
download from this repository; there is no requirement to disable Gatekeeper.
Setup is built and tested on macOS 14 (Apple Silicon) and macOS 15 (Intel).
Older macOS releases have not been verified.

Command-line source builds on either platform use:

```sh
bash ./build.sh '/path/to/River City Ransom (USA).nes'
./build-public/RCRness '/path/to/River City Ransom (USA).nes'
```

GitHub Actions compiles the shared host on each OS with original synthetic
test input and tests each packaged setup's embedded source audit. That checks
compiler/linker portability; it is not a commercial-ROM gameplay test. Windows
gameplay is tested privately; Linux/macOS game and physical-controller playtests
remain unverified until tested on those machines.

## Windows setup

### Recommended: setup executable

Download `RCRness-Setup.exe` from the latest GitHub release and run it. The
setup host asks for the user's legally obtained River City Ransom (USA) ROM,
verifies its SHA-256 locally, prepares the clean source kit, installs any
missing build tools with the user's approval, builds the private playable
executable, and launches it. A ROM can also be dragged onto the setup window or
onto `RCRness-Setup.exe`.

The setup executable embeds only this clean source/build kit. It contains no
ROM, generated game code, playable title executable, saves, or captures. The
ROM is never uploaded or copied into the source kit. The setup host and locally
built game are currently unsigned, so Windows SmartScreen may identify them as
unrecognized applications; use only the executable attached to this project's
GitHub release and verify its published SHA-256.

After a successful first build, opening setup again offers **Play Previous
Build** and **Rebuild**. Playing reuses the private executable immediately and
does not rerun the recompiler or compiler. Setup remembers only the original
ROM's local path; it never copies or uploads the ROM. If an older build has no
remembered path, the game's normal local ROM picker opens instead.

### Build from the source kit

The easiest path requires only 64-bit Windows 10/11, PowerShell, an internet
connection, and your legally obtained ROM. The setup wrapper downloads pinned,
SHA-256-verified portable copies of LLVM-MinGW, CMake, and Ninja into
`%LOCALAPPDATA%\RCRness\toolchains`, then runs the normal build:

```powershell
.\setup-and-build.ps1 -Rom 'C:\path\to\River City Ransom (USA).nes'
```

The portable downloads total about 231 MiB and are reused by later builds. They
do not install Visual Studio or a Windows SDK and do not require administrator
approval. Git is the only system tool required to fetch NESRecomp; if Git is
missing, the wrapper offers to install Git for Windows through WinGet. Automated
setups can pass `-AcceptToolLicenses` to accept that Git package prompt.

For an existing development machine, the direct build requires Git, CMake
3.20+, Ninja, and an x86-64 LLVM-MinGW compiler on `PATH` (or their paths can be
passed with `-CMakeExecutable`, `-NinjaExecutable`, and `-CCompiler`):

```powershell
git submodule update --init --recursive
.\build.ps1 -Rom 'C:\path\to\River City Ransom (USA).nes'
```

Both paths build the recompiler, privately generate `generated/rcr_*.c`, and
build `build-public/RCRness.exe`. The ROM and generated files stay
ignored and must never be committed or attached to a public release.

Xbox/SDL-compatible controllers are the clean-install P1 and P2 defaults. The
checked-in `keybinds.ini` also keeps movement on WASD and A/B on K/J as a P1
fallback; analog-stick and d-pad movement are both enabled. With two connected
pads, the first controls player 1 and the second controls player 2. P2 can also
use the arrow keys, keypad 1/2 for Punch/Kick, keypad Enter for Start, and
keypad 0 for Select.

Xbox, DualShock 4, DualSense, and recognized Switch-family pads use SDL's
standardized HIDAPI/gamepad mappings. Newer or unmapped devices (including
PowerA Switch 2 models not yet present in the bundled SDL database) remain
available through a generic joystick fallback with PowerA-aware face buttons,
hat/button d-pad support, analog movement, and common +/-/Share/Options mapping.
Hotplug is supported; connect both pads before launching for deterministic P1/P2
assignment.

Press **Escape** to pause local play, including inside the original NES
Start menu. Use the arrow keys and Enter, or either controller's D-pad and
confirm/back buttons. Options previews widescreen and CRT changes on the
frozen picture. Each explicit setting change is saved to `config.ini`.
Resume or press Escape again to continue. The NES Start button still opens
belongings, stats, and passwords. In a netplay match the overlay leaves the game running
and sends neutral local input. Widescreen and sprite-flicker reduction retain
their match values because both affect synchronized state. Confirming Quit
returns to the lobby.

In **Controls**, select a player, move to an action, and use Left/Right to
select its keyboard or controller binding. Confirm and press the new input.
Escape cancels capture. Conflicts offer a swap. The controls screen saves
changes to `keybinds.ini`. Press Punch and Kick together to jump, or double-tap
Left or Right to run.

In **Characters**, use Left/Right to choose Alex or Ryan, then move down and
choose shirt and pants colors, hair color, a hairstyle, or eyes. The standing previews show the active costume. Turning Oni mode off restores each player’s individual choices. Each
choice is saved separately in `config.ini` and applies when loading a save.

Display aspect ratio is selectable without changing the native framebuffer:

- Press Ctrl+F9 in game to toggle native-pixel and 4:3 presentation (persisted
  to `config.ini`).
- Or launch with `--aspect-4x3` / `--pixel-aspect` for a one-run override.
- `Aspect4x3 = 1` under `[Display]` selects 4:3 directly in `config.ini`.

Sprite-flicker reduction is enabled by default. RCR sometimes needs more than
one video frame to finish rebuilding shadow OAM. The option repairs only an
individual DMA entry carrying the writer's verified transient clipped
`Y=$F8` marker, using that entry's preceding coherent value.

Every other sprite entry from the current DMA passes through immediately. This
prevents the repair from holding characters and ground objects one update behind
the scrolling background.

- Press Ctrl+F8 in game to toggle the option (persisted to `config.ini`).
- Or launch with `--reduce-sprite-flicker` / `--authentic-sprites` for a
  one-run override.
- `ReduceSpriteFlicker = 1` under `[Display]` enables it directly.

The option does not modify guest RAM, generated code, or game timing. Disable
it to compare against the original hardware-era partial sprite updates.

## CRT/composite presentation

The CRT/composite filter is enabled by default and runs only on the displayed
image. Its presentation is modeled after MiSTer's official JVC preset: a neutral
staggered 6x6 JVC slot mask spans multiple picture pixels instead of aligning an
RGB triad to every pixel. Mild horizontal composite bleed, warm halation, soft
rounded pixel envelopes, filtered mask scaling, and a light Gaussian bloom keep
the image organic rather than rigidly gridded. A gentle
nonlinear midtone lift keeps the filtered image bright without raising true
black. The image is aspect corrected before the phosphor mask is drawn, keeping
the mask evenly spaced and avoiding wide interference bands in 4:3 mode. Native
screenshots, diagnostics, and game state remain unchanged.

The filter reuses finished blocks from the previous two frames when the source
pixels and their neighbors match, including during horizontal scrolling. Changed
blocks are filtered again with the same beam, slot mask, and bloom.

- Press Ctrl+F7 in game to toggle it (persisted to `config.ini`).
- Or launch with `--crt-filter` / `--no-crt-filter` for a one-run override.
- `CrtFilter = 1` under `[Display]` enables it directly.

An active HD texture pack takes presentation priority over the CRT filter.

The game and its audio automatically pause whenever the main game window loses
focus, then resume when it regains focus. Active netplay sessions remain live so
one unfocused peer cannot stall the other player.

## Distribution policy

Public source archives and releases must pass `package-public.ps1`. The audit
fails if it finds ROMs, generated game code, saves, captures, Ghidra databases,
or playable binaries. See `PUBLIC_DISTRIBUTION.md` for the complete boundary.
