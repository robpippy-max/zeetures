[![Watch the Zeetures trailer on YouTube](https://img.youtube.com/vi/C6BTk-ySg68/maxresdefault.jpg)](https://youtu.be/C6BTk-ySg68)

**[▶ Watch the trailer on YouTube](https://youtu.be/C6BTk-ySg68)** (36 seconds, full HD). It shows the game running from a player's own copy of Creatures 2, whose pictures and sounds belong to Gameware Europe. Music: Trepak (Tchaikovsky), synthesized performance by Quinn Mason, IMSLP, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

<h1><img src="art/logo/zeetures.png" alt="Zeetures: a labor of love by DotairZee" width="720"></h1>

> **Zeetures: a fan fork of openc2e, focused on Creatures 2** (forked from openc2e's commit [dfcd46f2](https://github.com/openc2e/openc2e/commit/dfcd46f2), 7 October 2026). It adds an original creature mind (the "homeostat" mind), a C2-style interface with the kits, world saving, scents and many fixes; see the `creature-mind` branch. Nightly builds are under [Releases](../../releases/tag/nightly) (they contain no game data: you need your own copy of Creatures 2). A noncommercial labor of love by an old fan, developed alongside Claude (Anthropic's AI), not affiliated with Gameware Europe or the openc2e developers. Every change is in the [changelog](CHANGELOG.md). See [Built on openc2e](#built-on-openc2e) below.

## What you need

* **Your own copy of Creatures 2.** Zeetures contains no part of the game: no sprites, sounds, genomes, scripts or worlds. It reads them from your installed copy. Creatures 2 comes with *Creatures: The Albian Years* on Steam, and is also sold on GOG. Creatures, Norns, Grendels, Ettins and Albia belong to Gameware Europe.
* An internet connection the first time, for two breed packs (below), and `curl`, which Windows 10 and later, macOS and most Linux systems already have.
* A Linux, Windows or macOS computer. There are 64-bit builds for all three, and 32-bit (x86) builds for Linux and Windows.

## Download and play

Download the file for your system from the [nightly release](../../releases/tag/nightly) (the newest build), then follow its one-time steps. No command line is needed.

| System | Download |
| --- | --- |
| Windows 10 or 11 | `Zeetures-Setup.exe` |
| Mac (Apple Silicon or Intel, macOS 10.15 or later) | `Zeetures-macOS.dmg` |
| Linux (64-bit) | `Zeetures-x86_64.AppImage` |
| Linux (32-bit) | `Zeetures-i686.AppImage` |

* **Windows:** run `Zeetures-Setup.exe`. Windows may say "Windows protected your PC", because the installer is not signed: click **More info**, then **Run anyway**. It installs for you alone (no administrator password), adds Zeetures to the Start menu and the desktop, and can start it at the end. The 64-bit and 32-bit programs are both inside; it picks the right one.
* **Mac:** open `Zeetures-macOS.dmg` and drag Zeetures into Applications. The first time, open it from Applications: macOS says it can't check it, because it is not signed by Apple. Click **Done**, open **System Settings > Privacy & Security**, click **Open Anyway** beside "Zeetures was blocked", and confirm. (On macOS 14 and earlier, right-click Zeetures and choose **Open** instead.) From then on it opens normally. To get the game's files on a Mac, see [On a Mac](#on-a-mac).
* **Linux:** make the file executable once (in most file managers: right-click it, **Properties**, then tick **Allow executing file as program** or **Executable as Program**; or `chmod +x Zeetures-x86_64.AppImage`), then double-click it. It runs on Ubuntu 22.04, Debian 12, Zorin 17, Mint 21, Fedora 36 and later, and needs nothing else installed.

**Finding your Creatures 2.** The first time Zeetures starts, it looks for Creatures 2 in the usual places: every Steam library (including Steam's Flatpak and Snap versions on Linux), GOG's folders, and on a Mac `~/Games/Creatures The Albian Years`. If it finds none, a window asks you to choose the folder: the `Creatures 2` folder that holds `Creatures2.exe`, `Eden.sfc` and the `Images` folder, or the `Creatures The Albian Years` folder that holds it. It remembers the folder. To use a different one later, choose **File > Creatures 2 Folder...** (or edit `game-folder.txt`; see below).

The `.zip` and `.tar.gz` files in the release are the same programs unpacked, for anyone who prefers them: unpack and start `Zeetures.exe` (Windows) or `openc2e` (Linux). Started from a terminal, they still take `-d "<Creatures 2 folder>"` (see [Command-line options](#command-line-options)).

The creatures use Zeetures's own mind (`--mind original` gives them openc2e's unfinished copy of the original brain, which does very little). Zeetures's world additions (map fixes, script fixes and new objects) are built into the program and applied to every Creatures 2 world as it loads. Their pictures that Zeetures draws itself (walkways, lifts, bushes, the memorial stone and more) come with the download, in its `zeetures-art` folder: keep it beside the program. The pictures that are cut from the game's own (the waterwheel stream, the west sea airlock's rock, the volcano passage and trunk, the treehouse lift, the waterfall path, a pillar's foot) are made by Zeetures on your machine, from your own copy of the game, when it starts.

Your worlds, saves and settings are kept in the user folder, never in the game's folder:

* **Linux:** `~/.openc2e/Creatures 2`
* **Windows:** `Documents\My Games\Creatures 2`
* **macOS:** `~/Documents/openc2e Data/Creatures 2`

One level up from those (for example `~/.openc2e`) is `game-folder.txt`, the Creatures 2 folder Zeetures remembered, and on Windows `zeetures-log.txt`, the program's messages from its last run, useful with a bug report.

### On a Mac

Zeetures runs natively on macOS (Metal graphics, CoreAudio sound). You do **not** need Wine, CrossOver or Steam Play: only the game's *data* comes from Steam. Checked on an M4 MacBook Pro with macOS 15 Sequoia.

**Getting the Creatures 2 files.** Steam for Mac lists *Creatures: The Albian Years* as Windows-only and won't install it, but an account that owns it can download its files through Steam's console:

1. With Steam running, open its console: in Terminal, run `open "steam://open/console"` (or open that link in a browser). Steam shows a **Console** tab. If it doesn't appear, run the command again.
2. Type `download_depot 1818340 1818341` and press Return. (1818340 is the game; 1818341 its one depot, the Windows files, with both Creatures 1 and Creatures 2: about 84 MB, 307 MB unpacked.) Steam answers `Depot download complete`.
3. The files are in `~/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_1818340/depot_1818341/`. Copy them somewhere permanent, since Steam may clear that folder later:

   ```bash
   mkdir -p ~/Games
   ditto ~/Library/Application\ Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_1818340/depot_1818341 ~/Games/"Creatures The Albian Years"
   ```

   The folder to give Zeetures is then `~/Games/Creatures The Albian Years/Creatures 2`. (The same console command works on Linux and Windows, to get the files without installing the game. With the GOG release, use the `Creatures 2` folder from a Windows install of it.)

**Running it.** Open `Zeetures-macOS.dmg`, drag Zeetures into Applications, and allow it once in System Settings > Privacy & Security (see [Download and play](#download-and-play)). If the game's files are in `~/Games/Creatures The Albian Years`, or still where Steam downloaded them, Zeetures finds them by itself; otherwise it asks for the folder. From Terminal: `/Applications/Zeetures.app/Contents/MacOS/Zeetures -d "$HOME/Games/Creatures The Albian Years/Creatures 2"`.

**Two breed packs, fetched on your machine.** The first time a Creatures 2 world runs, Zeetures downloads two community breed packs from the [Eem Foo archive](https://eem.foo/archive), which already hosts them, and imports them: [Boney Grendels](https://eem.foo/archive/downloads/1fad-boney-grendels) (Creature Labs' own Creatures 2 release) and [Frog Norns V3](https://eem.foo/archive/downloads/2538-frog-norns-v3) (by Alex of Norns of the Round Table, from Nornenmeister's V2), which takes the place of the game's own frog norn. Zeetures itself does not carry or host them. The files are kept in your user folder's `Downloads`; to skip this, put the line `breed-packs off` in your user folder's `settings.txt`.

Linux is where Zeetures is played and tested every day; the Windows and macOS builds are new and less tried. The Windows installer has been installed, played and uninstalled in 64-bit and 32-bit Wine (a new world, and a world of a hundred creatures in the 32-bit program); the 32-bit builds have not yet been tried on a real 32-bit machine.

## Status

Zeetures is at version 0.33.0 (shown in Help > About), after about 180 hours of work (since 7 November 2026). It is made by DotairZee alongside Claude, Anthropic's AI. It is a work in progress, but Creatures 2 is playable from start to finish: creatures hatch, eat, learn, talk, breed, grow old and die, and worlds are saved. The published research behind the creatures' mind and behaviors is listed in [CITATIONS.md](CITATIONS.md) (also in Help > About).

### What works

* **A new creature mind.** In stock openc2e, C2 creatures hatch but do almost nothing, because the original brain was never finished. Zeetures adds a small "creature mind" interface to the engine and an entirely new mind behind it (the "homeostat" mind): an original design, not a recreation of the C2 brain. Creatures learn what each action does to their needs, remember where food is, follow smells, learn a map of the world (lifts, boats, teleporters), hold their breath underwater, avoid dangerous drops and show their thoughts in picture bubbles. They learn by watching one another (family most, and the young most of all), go back to places that eased their pain, learn which fruit is a medicine, and bring things to the sick and the young. They are curious about what they are still learning, eat ahead of a long time without food, and find food less pleasant the fuller they are.
* **Personality and language.** Seven inborn temperament traits and eight mind genes, inherited with mutation, so minds evolve over the generations; temperament also comes partly from a creature's own Creatures 2 genes, and is shifted by what happens to a creature while it is young, and an inherited seasonal rhythm (how strongly a creature fattens in autumn and saves itself in winter); relationships with friends, enemies and mates; creatures born knowing no words, who learn them from the learning machines, the hand, each other and the Stone of Knowledge, and babble in "Nornish" until they do. Speak to them and they look at you and try your words; say the name of a thing they know, and they look at it; verbs they know ("come", "push toy") are suggestions they weigh by age, temper, needs and how you have praised them. Teach them names of your own ("mummy" for your hand, anything for a thing you point at): they use them, and teach one another.
* **The life cycle.** Courtship, genetic crossover with mutation, pregnancy, eggs and hatching; aging, deaths with real causes, and grown creatures who tend babies, children and the senile. Breed import from zips, COBs, old installers and .exp files.
* **Old custom genomes.** Genome packs made for the original game (genomes, eggs, exported creatures) hatch and work: their chemistry and instincts run as written, including those tied to the original brain's cells, and lobes they add to the brain (Nova Subterra's, Canny Norns', and others read from their wiring) get matching roles in the new mind, listed in `brain-profiles.txt`.
* **Made for your old add-ons.** Zeetures takes Creatures 2 breeds, genomes and COBs, and has been tested on dozens of packages: of the objects in one large archive of old COBs, 115 of 116 ran cleanly, each tried in the game; breed packs (Midsummer Norns by Martina Hilsenbeck (Nimue) and Amanora, genome by Katira; Goat Norns by Angela Allan; Kai Norns by Wafuru, genome by Lis Morris; Scorpio Norns by Wafuru, genome by Mannkind; Boney Grendels by Creature Labs, updated by Lis Morris; Frog Norns V3 by Alex (Norns of the Round Table), from Nornenmeister's V2) and custom genomes (Nova Subterra by Lummox JR, Canny Norns by Lis Morris and Chris Double) import and live.
* **Illness and medicine.** Germs live in dark caves and rotting food, spread from room to room and creature to creature by sneezes and coughs, slowly make new strains, and are fought by antibodies; the ill are tended like the young. Fruit left lying rots, feeding the soil and the germs; eating it hurts, and may make a creature ill. Heavy metals from radiation hurt and wear the body, the volcano's fruit clears them, and radiation lowers fertility and adds mutations before birth.
* **Smooth movement.** Creatures move smoothly from pose to pose instead of jumping five times a second: the pictures between are made as the game runs, by turning your own copy's sprites at the joints. Every breed has it, add-ons too, and Camera > Smooth Movement turns it off. Sleepers show the game's own Z's.
* **Saving.** Multiple worlds with a chooser (or one world opened at every launch) and autosave. Creatures, their minds, eggs, family records and every object in the world are saved. Worlds saved by earlier versions load in later ones, and a save that cannot be read is kept aside, never overwritten.
* **The interface.** Every C2 kit rebuilt inside the game window (Health, Science, Breeder's, Owner's, Observation, Mind, Graveyard, Chronicles, Ecology, the Agent Injector and the Hatchery), status-bar notices, a Creature's View seen through the creature's own eye (its pupil, lids and coloring, centered on what it is looking at), a speed control, zoom from 50% to 300%, pinch to zoom, scroll bars and full screen. An in-depth Help (F1) with a search: 44 topics, one for every kit. It updates itself (Help > Check for Updates), and the checks and automatic installing can be turned off.
* **The world.** Dozens of map fixes, new routes and objects (seasonal fruit bushes, airlocks, walkways, a lab lift), ways out of the seas for swimmers (the submarine picks them up, the west sea's airlock draws in everyone near it, the sea jump pads throw them onto the shore), scents that drift between rooms, a nutrient cycle in the soil (the dead feed it, and it seeps downhill), air pressure that spreads between rooms and makes the wind, radiation that spreads from its sources, outdoor light that follows day and night, and seasons that change creatures' appetite and metabolism, breeding, bonds, rest and play, shaped by their life stage.
* **Stability.** Fixes for an audio deadlock, several crashes and hangs, a game tick about seven times faster in a crowded world, about half the CPU use of 0.8.0 and about a third less again since 0.27.0: a world of a hundred creatures takes about a quarter of one core of a recent laptop.

### Not done yet

* Hunger in very crowded worlds (about 150 creatures in a world built for 16).
* Windows and macOS are built as often as Linux but played far less.
* Creatures 2 is the focus. Creatures 1, Creatures 3 and Docking Station run as they do in upstream openc2e, and have not been tested in Zeetures.

### What's coming

* **A Breed Builder Kit**, inside the game: make new breeds.
* **A COB Builder Kit**, inside the game: make new objects.
* **Full emotion sprites for the Creatures 1 breeds**, several of which have only a neutral face in Creatures 2.

Every change, in technical detail, is in the [changelog](CHANGELOG.md).

## Building it yourself

You need CMake (3.16 or newer), Python 3 (3.9 is enough), Git and a C++20 compiler. SDL3, SDL3_mixer, zlib (on Windows), fmt, imgui and the rest come with the source, in `externals`.

```bash
git clone https://github.com/DotairZee/zeetures.git
cd zeetures
```

### Linux (Debian, Ubuntu and relatives)

```bash
sudo apt-get install build-essential cmake python3 git zlib1g-dev libpng-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxfixes-dev \
  libwayland-dev libxkbcommon-dev libegl1-mesa-dev libgl1-mesa-dev libdecor-0-dev wayland-protocols \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libdbus-1-dev libudev-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENC2E_ASAN=OFF -DOPENC2E_BUNDLED_SDL=ON
cmake --build build -j"$(nproc)"
./build/openc2e -d "/path/to/Creatures 2"
```

`OPENC2E_BUNDLED_SDL=ON` builds the SDL3 in `externals` (the packages above are its headers). If your system has SDL3 development packages (`libsdl3-dev`), you can leave it off. `OPENC2E_ASAN=OFF` turns off AddressSanitizer, which is on by default and makes the game several times slower.

### Windows

Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with the "Desktop development with C++" workload (it includes CMake), and [Python 3](https://www.python.org/). In a Developer PowerShell or Command Prompt:

```bat
cmake -S . -B build -DOPENC2E_ASAN=OFF
cmake --build build --config RelWithDebInfo
build\RelWithDebInfo\openc2e.exe -d "C:\path\to\Creatures 2"
```

### macOS

1. Install Apple's command line tools (clang, git and Python 3):

   ```bash
   xcode-select --install
   ```

   A window asks you to click **Install**, then **Agree** (about 1 GB). `xcode-select -p` prints `/Library/Developer/CommandLineTools` once they are in. On a Mac without them, `git`, `clang` and `python3` are only stand-ins that open the same installer. If that window gets closed or hidden, the installer keeps running unseen and `xcode-select --install` then does nothing: bring it back with `open -a "/System/Library/CoreServices/Install Command Line Developer Tools.app"`, or quit it with `osascript -e 'quit app "Install Command Line Developer Tools"'` and run `xcode-select --install` again.

2. Get CMake. Homebrew works (`brew install cmake`), but isn't needed: the Python 3 that comes with the command line tools (3.9) is enough for the build, and CMake installs from PyPI into a folder of its own (here beside the clone):

   ```bash
   python3 -m venv .tools
   .tools/bin/pip install --upgrade pip cmake
   ```

3. Clone and build (with Homebrew's CMake, just `cmake`):

   ```bash
   git clone https://github.com/DotairZee/zeetures.git
   cd zeetures
   ../.tools/bin/cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENC2E_ASAN=OFF
   ../.tools/bin/cmake --build build -j"$(sysctl -n hw.ncpu)" --target openc2e
   ./build/openc2e -d "$HOME/Games/Creatures The Albian Years/Creatures 2"
   ```

   `--target openc2e` builds just the game; leave it off to build the tools and tests too. Clang prints some `-Winconsistent-missing-override` warnings; they are harmless. For an Intel Mac build on an Apple Silicon Mac, add `-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15` to the first `cmake` line.

### 32-bit builds

On Windows, add `-A Win32` to the first `cmake` line to build for 32-bit x86. On Debian or Ubuntu, a 32-bit build on a 64-bit system needs the multilib compilers and the `:i386` development packages:

```bash
sudo dpkg --add-architecture i386 && sudo apt-get update
sudo apt-get install gcc-multilib g++-multilib zlib1g-dev:i386 libpng-dev:i386 libx11-dev:i386 libxext-dev:i386 \
  libxcursor-dev:i386 libxrandr-dev:i386 libxi-dev:i386 libxss-dev:i386 libxtst-dev:i386 libxfixes-dev:i386 \
  libxkbcommon-dev:i386 libgl1-mesa-dev:i386 libegl1-mesa-dev:i386 libasound2-dev:i386 libpulse-dev:i386
PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig cmake -S . -B build32 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu \
  -DOPENC2E_BUNDLED_SDL=ON -DOPENC2E_ASAN=OFF -DSDL_WAYLAND=OFF
cmake --build build32 -j"$(nproc)"
```

(On a 32-bit system itself, the plain Linux steps above work as they are.)

## Command-line options

`openc2e --help` lists them all. The ones Zeetures adds or uses most:

* `-d PATH`: the Creatures 2 folder. Without it, Zeetures uses the folder it remembered, looks in the usual Steam and GOG places, or asks.
* `--mind original`: openc2e's unfinished copy of the original brain instead of Zeetures's mind.
* `--world NAME`: open that world from the Worlds folder, skipping the world chooser.
* `--no-autosave`: don't save the world on quit or when switching worlds.
* `--no-world-chooser`: load the game's own starting world, as the original game does.
* `-s`: no sound.

# Built on openc2e

![openc2e logo](https://raw.githubusercontent.com/ligfx/openc2e/master/Openc2e-logo2008.png)

Zeetures is a fork of [openc2e](https://github.com/openc2e/openc2e), the free and open-source engine for the [_Creatures_](https://creatures.wiki) artificial life games (Creatures, Creatures 2, Creatures 3, Docking Station and more), by Alyssa Milburn, Michael Maltese, Bryan Donlan, Jeremy Apthorp, Eric Goodwin and others. It was forked from openc2e's commit [dfcd46f2](https://github.com/openc2e/openc2e/commit/dfcd46f2) of 7 October 2026. For openc2e itself, its other games and its own status, see [its repository](https://github.com/openc2e/openc2e); its developers and players gather in the [Caos Coding Cave Discord](https://discord.gg/rWFC3b3). Zeetures is not affiliated with the openc2e developers.

openc2e is free software under the GNU Lesser General Public License, version 2.1 (see `COPYING`), and Zeetures's changes are under the same license.
