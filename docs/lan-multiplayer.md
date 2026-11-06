# LAN multiplayer

WiiCompiled can be built with LAN multiplayer: the **Nintendo WFC** menu then plays directly with
other players on the local network, without any server. This is a port of the
[rmc-local-net BrainSlug module](https://github.com/Luporion/MKW-LAN-Dolphin-Fork/tree/v0.9-Luporion.2)
("MKW LAN multiplayer" by Chadderz and MrBean35000vr, v0.9-Luporion.2 fork by Luporion).

Only the **PAL** release (`RMCP01`) is supported so far, and only the base game (not Retro Rewind).

## Building

```bash
Launcher/local-build.sh --lan --output-dir out/lan
```

`--lan` passes `projects/mkwii/lan_patches.txt` to every translator step
(`--guest-patches`). Building without `--lan` produces the unmodified game again. Both share the
same translation cache directory, so switching between them retranslates.

The player-name tool (see [Player names](#player-names-miis)) is a separate .NET 8 program. Build
it once for each platform; the output is one self-contained file that needs no installed .NET:

```bash
dotnet publish Launcher/LanPlayerSetup -c Release -r linux-x64 -o out/lan-player/linux-x64
dotnet publish Launcher/LanPlayerSetup -c Release -r win-x64   -o out/lan-player/win-x64
```

Put `LanPlayerSetup` (`LanPlayerSetup.exe` on Windows) next to the game executable.

## Playing

1. Start the game on every PC. Each player needs a license (save file).
2. Open **Nintendo WFC**. It is relabelled **LAN Multiplayer**, and only the friend section is
   available.
3. The friend roster fills in automatically with every console on the network. One player creates
   a room; the others join it from the roster, like a friend room.

Several instances can run on the same PC. Each one needs its own user directory (save files and
NAND), for example:

```bash
for n in 1 2; do
  mkdir -p ~/mkw-lan/$n/MarioKartWiicompiledLAN
  cp ~/.local/share/MarioKartWiicompiledLAN/Config.toml ~/mkw-lan/$n/MarioKartWiicompiledLAN/   # needs paths.dvd_root
  ./LanPlayerSetup --no-gui --user-dir ~/mkw-lan/$n/MarioKartWiicompiledLAN --name "Racer$n"
  XDG_DATA_HOME=~/mkw-lan/$n ./WiiCompiled &
done
```

### Player names (Miis)

Other players see the name of your license's Mii. A fresh install has no Mii database, so the
game only offers guest Miis, and it always shows a guest Mii as "Player".

Start **`LanPlayerSetup`** before the game. Enter a name (default: `Player`), and optionally pick
the face (male/female) and the favorite color. Then click **Save and play**. The tool keeps one
Mii in the game's Mii database (`NAND/shared2/menu/FaceLib/RFL_DB.dat`) and leaves any other Miis
alone. If you copied a database from Dolphin or a Wii, a backup is kept as `RFL_DB.dat.bak`.

The first time, select the new Mii for your license in the game. Either use **Change Mii** in the
license's settings, or create a new license with it. After that, the license keeps pointing at
that Mii, because renaming never changes its Mii ID. Changing the name in the tool is then enough.

It finds the game's user directory the same way the game does: a portable root, otherwise
`%LOCALAPPDATA%\MarioKartWiicompiledLAN` or `~/.local/share/MarioKartWiicompiledLAN`, with
`[paths] nand_root` respected. It also has a command line mode, which `LanPlayerSetup --help`
describes, for example:

```bash
LanPlayerSetup --no-gui --name Andi --color 5      # set the name without a window
LanPlayerSetup --no-gui --list                     # list the Miis in the database
```

### Network requirements

- Discovery uses UDP broadcasts to ports **27900-27929**. Each instance binds the first free port
  in that range, so up to 30 instances can find each other.
- A broadcast goes to 255.255.255.255 and also to the broadcast address of every network adapter
  that is up. Windows sends a 255.255.255.255 broadcast out of only one adapter, which can be a
  VPN, Hyper-V/WSL or VirtualBox adapter instead of the LAN.
- A host also learns a player from their join request, so a console whose broadcasts don't
  arrive can still join. If the host can't answer a console at all, it treats that as a lost
  packet and keeps the room open; before, DWC closed the room for everyone.
- Race traffic uses a UDP port picked by the game. If that port is taken, a free port is used
  instead.
- Allow WiiCompiled through the firewall for UDP on the LAN, for example
  `firewall-cmd --add-port=27900-27929/udp` plus the game ports. The simplest option is to trust
  the LAN zone.
- The wire format is unchanged from the module, so players on Dolphin (or a Wii) running the
  same v0.9-Luporion.2 module can join too. Mixing with the original fixed-port v0.9 module is
  not guaranteed to work.

### Finish times on a slow PC

The race clock counts game frames. A Wii never drops one, but a PC that can't keep up runs the
game slower, so its clock falls behind real time and its player posts too short a time. Online
results are ordered by the reported times, so that player could win a race they finished last.
In a LAN race the port therefore adds the time this PC fell behind the wall clock since the
countdown started to the local player's finish time. At full speed nothing changes. Each finish
is logged, for example:

```
[lan] finish: 6254 game frames, 6266 wall-clock frames, 12 behind; time 1:40.349 -> 1:40.549
```

A PC that is often more than a few frames behind is too slow for its settings: lower the
resolution multiplier, or check the power plan. Offline races, time trials included, keep the
game's own time; the line is still logged there.

### Known limitations

The game's idle disconnect is turned off: the patch set includes Bully's
"No Idle Disconnect" code, which the module's README recommends. Without it, the game drops a
player who gives no input for about 30 seconds. That always happens when two instances run on
one PC, because only the focused window gets input.

These limitations come from the original module:

- The usual online limits still apply: 12 players per race and 30 seconds for selections.
- After a disconnect, leave the LAN menu on both sides and enter it again.
- Rankings, ghost uploads and the Mario Kart Channel are disabled.

## Release packages (Docker)

`Launcher/release/` builds portable, ready-to-play packages in Docker. Use a separate clean
checkout with `Assets/` (the extracted disc) and `Launcher/artifacts/portable-tools`
(`Launcher/prepare-portable-tools.sh --arch x86_64`) in it. The native build caches belong to
the workspace.

```bash
Launcher/release/build-linux.sh   --output-dir ../out-linux     # Ubuntu 22.04, glibc 2.35
Launcher/release/build-windows.sh --output-dir ../out-windows   # llvm-mingw cross build
Launcher/release/package.sh --game-data Assets/DATA \
    --linux-build ../out-linux --linux-player out/lan-player/linux-x64/LanPlayerSetup \
    [--windows-build ../out-windows --windows-player out/lan-player/win-x64/LanPlayerSetup.exe]
```

Both build scripts default to x86-64-v3 (AVX2). Pass `--x86-cpu-profile v2` for older CPUs
(SSE4.2, from about 2008); a v2 build gets its own build directory. Tell `package.sh` with
`--linux-cpu v2` / `--windows-cpu v2`, so the README states the right requirement.

- **Linux** (`Dockerfile.linux`): the repository's portable clang on Ubuntu 22.04. libstdc++ comes
  from GCC 13 (the prebuilt Dawn needs it) and is linked statically. `MKW_GLIBC_C23_COMPAT`
  supplies the four glibc 2.38 `__isoc23_strto*` functions Dawn references. The binary needs only
  glibc 2.35, zlib and the usual desktop libraries SDL loads at run time.
- **Windows** (`Dockerfile.windows`): the Linux-hosted build of the same llvm-mingw release the
  Windows build uses. Wine runs Microsoft's `cppwinrt.exe` once, at image build time, to generate
  the C++/WinRT headers. `MKW_ASM_TARGET=windows` makes the translator emit COFF assembly, and
  `mingw-case-headers.sh` aliases mixed-case Windows header names. Wine can also smoke-test the
  result (see the script comments).
- **Package** (`package.sh`): `game/DATA`, `linux/` and/or `windows/`, a `README.txt` built from
  `README-package.txt`, and `portable.txt` plus `UserData/Config.toml` with
  `dvd_root = "../game/DATA"`. The game and LanPlayerSetup then keep everything in the package
  folder. The package contains the game's data, so it is only for people who own the game.

## How it works

Translated code cannot be patched at run time, so the module is split in two:

- **`projects/mkwii/lan_patches.txt`** holds the module's instruction patches (37 words in DWC
  and the menu code) plus the 5-word idle-disconnect fix. The translator verifies each original
  word and applies the patch to the loaded image before translation
  (`Translator.Core/Loading/GuestPatchSet.cs`), so they are compiled in as if the game had shipped with them. The `bl` patches point at four trampoline
  addresses in unused RAM (`0x819F0000`).
- **`runtime/src/hle/net/lan_mode.cpp`** holds the module's function replacements
  (`DWC_LoginAsync`, `dwc_send_status_record`, `BMG_Print`, ...) and its discovery thread. They are
  native wrappers registered at the original addresses, and natives at the trampoline addresses.
  They call the game's own SO/DWC/OS functions, just like the module does on a Wii, and they only
  activate when the translation carries the `lan` patch set (`RuntimeConfig::GUEST_PATCH_SET`).
  Otherwise they call straight through to the original game code.
