Mario Kart Wii LAN (PC) - @VERSION@
https://github.com/andygruber/MarioKartWiicompiledLAN

Mario Kart Wii running natively on the PC (WiiCompiled by patchzyy), with LAN multiplayer built
in (a port of the "MKW LAN multiplayer" mod by Chadderz and MrBean35000vr, v0.9-Luporion.2 fork
by Luporion). Everybody on the same network can race together, with no server and no
Nintendo WFC.

This package already contains the game files (the PAL release, RMCP01), so it is ready to play.
They are Nintendo's: only pass this package on to people who own Mario Kart Wii themselves.


STARTING THE GAME

@IF_LINUX@
  Linux:    open the "linux" folder and start LanPlayerSetup
            (or start WiiCompiled directly).
@END@
@IF_WINDOWS@
  Windows:  open the "windows" folder and start LanPlayerSetup.exe
            (or start WiiCompiled.exe directly). If Windows SmartScreen warns about an unknown
            app, choose "More info", then "Run anyway".
@END@

LanPlayerSetup sets your player name: enter a name (default "Player"), pick a face and a
favorite color, then click "Save and play".

Keep the folders together as they are: the game reads its files from "game", and keeps your
settings, saves and logs in "UserData".


THE FIRST TIME

1. Start the game and create a license (save file). When it asks for a Mii, choose the one
   LanPlayerSetup made (it has your name). If you made the license with a guest Mii, open the
   license's settings and use "Change Mii".
   Guest Miis always show up as "Player" for everyone else.
2. After that, changing the name in LanPlayerSetup is enough; the license keeps that Mii.


PLAYING ON THE LAN

1. Start the game on every PC (all PCs need this same version).
2. Choose "Nintendo WFC". It is now "LAN Multiplayer"; only the friends section is available.
3. The friend list fills itself with every PC on the network that runs the game. One player
   creates a room; the others join it from the friend list.

Things to know:
- Up to 12 players. Character, kart and track selection has the usual 30 seconds.
- Idling no longer disconnects you.
- After a disconnect, leave the LAN menu on every PC and enter it again.
- Rankings, ghost uploads and the Mario Kart Channel are turned off.

Several copies can even run on one PC for testing (each one needs its own copy of this folder).


FIREWALL

The PCs find each other with UDP broadcasts on ports 27900-27929; races use further UDP ports
picked by the game.
@IF_WINDOWS@
- Windows: when the firewall asks the first time, allow WiiCompiled on private networks. Set
  your network to "Private" in the Windows network settings.
@END@
@IF_LINUX@
- Linux: allow the game's UDP traffic on your home network, e.g. with firewalld
  "sudo firewall-cmd --zone=home --change-interface=<your network device>" or by trusting the LAN
  zone; with ufw "sudo ufw allow from 192.168.0.0/16".
@END@


CONTROLS AND SETTINGS

A game controller works out of the box; the keyboard works too. Press F10 in the game for the
settings bar: controller and keyboard mapping, resolution, FPS counter, volume and more.
Changes are saved right away.


FILES

  game/        the game files (Mario Kart Wii PAL)
@IF_LINUX@
  linux/       the Linux game and LanPlayerSetup
@END@
@IF_WINDOWS@
  windows/     the Windows game and LanPlayerSetup.exe
@END@
  UserData/    your settings (Config.toml), saves (NAND) and logs. To move to a new version,
               copy this folder into the new package.
  portable.txt keeps everything inside this folder; don't delete it.


REQUIREMENTS

A 64-bit PC with a processor from 2013 or newer (AVX2: Intel Haswell, AMD Excavator / Ryzen).
@IF_WINDOWS@
- Windows 10 or 11, 64-bit, with a DirectX 12 or Vulkan capable graphics card.
@END@
@IF_LINUX@
- Linux with glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+, Fedora 36+, Arch, SteamOS, ...)
  and a Vulkan driver. If the game finds none, install it:
    Fedora          sudo dnf install mesa-vulkan-drivers vulkan-loader
    Ubuntu, Debian  sudo apt install mesa-vulkan-drivers libvulkan1
    NVIDIA          the NVIDIA driver includes it
@END@


LICENSE

The program is free software under the GNU General Public License v3 (LICENSE.txt);
THIRD-PARTY-NOTICES.md lists the libraries it uses and their licenses. Mario Kart Wii and
Nintendo's code and assets belong to Nintendo. Not affiliated with or endorsed by Nintendo.
