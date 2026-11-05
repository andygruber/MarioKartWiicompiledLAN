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
- Race traffic uses a UDP port picked by the game. If that port is taken, a free port is used
  instead.
- Allow WiiCompiled through the firewall for UDP on the LAN, for example
  `firewall-cmd --add-port=27900-27929/udp` plus the game ports. The simplest option is to trust
  the LAN zone.
- The wire format is unchanged from the module, so players on Dolphin (or a Wii) running the
  same v0.9-Luporion.2 module can join too. Mixing with the original fixed-port v0.9 module is
  not guaranteed to work.

### Known limitations

The game's idle disconnect is turned off: the patch set includes Bully's
"No Idle Disconnect" code, which the module's README recommends. Without it, the game drops a
player who gives no input for about 30 seconds. That always happens when two instances run on
one PC, because only the focused window gets input.

These limitations come from the original module:

- The usual online limits still apply: 12 players per race and 30 seconds for selections.
- After a disconnect, leave the LAN menu on both sides and enter it again.
- Rankings, ghost uploads and the Mario Kart Channel are disabled.

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
