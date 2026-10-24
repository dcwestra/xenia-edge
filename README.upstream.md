<p align="center">
    <a href="https://github.com/xenia-canary/xenia-canary/tree/canary_experimental/assets/icon">
        <img height="256px" src="https://raw.githubusercontent.com/xenia-canary/xenia/master/assets/icon/256.png" />
    </a>
</p>

<h1 align="center">Xenia Edge - Xbox 360 Emulator</h1>

Xenia Edge is yet another experimental fork of the Xenia emulator, originally based on [Xenia Canary](https://github.com/xenia-canary/xenia-canary). The focus is
on faster iteration, higher default game compatibility, usability and platform support.

## Status

Build (Windows / Linux / macOS): [![CI](https://github.com/has207/xenia-edge/actions/workflows/CI.yml/badge.svg?branch=edge)](https://github.com/has207/xenia-edge/actions/workflows/CI.yml) [![Codacy Badge](https://app.codacy.com/project/badge/Grade/cd506034fd8148309a45034925648499)](https://app.codacy.com/gh/has207/xenia-edge/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

Releases
--------
[Latest](https://github.com/has207/xenia-edge/releases/latest) ◦ [All](https://github.com/has207/xenia-edge/releases)

FAQ
---

- Q: How to use systemlink?<br>
  A: Systemlink is enabled by default and will work without further configuration with other Edge instances on the same network. To connect to actual xbox 360 consoles (stock is fine) you need an instance of [Xlink Kai](https://www.teamxlink.co.uk/) running somewhere on your network, most likely the same PC you're running Xenia but it's not necessary. In that case you will need to set ```systemlink_transport = "kai"``` in the Network settings. In order to connect with remote users over Kai you will need to add them as a friend in the Kai Engine interface once you started it, and they must do the same. No other configuration should be needed. It's possible to use Kai locally with multiple Edge instances but it requires setting "Allow remote DDS connections" in the Kai settings, but again you don't need Kai for local Xenia-only connections so use ```systemlink_transport = "tunnel"```, which is the default mode.

- Q: How to run original Xbox games?<br>
  A: OG Xbox games are supported in the same manner they are on a real console, more precisely an RGH/JTAG console, through ```xefu```. You will want to search for how to run them there, the same information applies here. Keep in mind xefu is not a single executable but a "family" where ```xbox.xex``` is the console's dispatcher that selects the appropriately whitelisted ```xefuN.xex```. These have all been community patched to run other games, so you'll want to figure out how it's done on RGH/JTAG setups and mostly follow those instructions. Do not expect compatibility/performance to be quite as good as a real console quite yet, it is still a work in progress.

- Q: How to run Xbox Live Indie (XBLIG) games?<br>
  A: XBLIG titles require an XNA runtime and launcher. The launcher is available in the system update you can download from MS when choosing the USB Update option [direct link](https://www.xbox.com/system-update-usb). Unzip and pull out ```Xna_TitleLauncher.xex``` and place it in its own directory (this step is important). Then add it to the library by running it with Edge, it will complain about a missing update but you want to do this so that it's added to the game list. As for the update, you will need to get it the same place you find XBLIG titles themselves, it contains the actual XNA runtime that's needed to launch them. The titles are not proper game packages like everything else, and instead install as DLC content for the launcher, so they won't show up as individual games in the game list (for now). When you start the launcher any XBLIG titles you have installed will be shown in a prompt for you to select which one to start. They are also listed in the Content tab on the launcher's info pane. This experience will probably improve in the future so no need to tell me how awkward it is. I know.
  
- Q: How to tell what options a particular game might need?<br>
  A: Many games that are not running well by default require one or two simple config changes that make them (near) perfect. To find out, check the Compatibility section in game details pane. If there is an existing compatibility page there will be a link to Master, Canary, or both. Prefer information on the Canary page, you will often find the answer there.

- Q: Why are translations into language X so bad?<br>
  A: They're AI-generated, if you speak the language and want to help edit the relevant .po file in the assets directory and submit it. If you don't know how to use git then just open a bug and attach the fixed .po file. Best way to edit those is with a program called Poedit. Or just open a bug and tell me what translation is bad and how to improve it.

- Q: Should I use the Windows build with wine/proton on Linux?<br>
  A: The native build for Linux is quite competitive with Windows now and is recommended over wine/proton.

- Q: Why is macOS build not as capable as Windows / Linux?<br>
  A: macOS is the newest port, with a completely different CPU and GPU backends that have not had nearly the amount of testing of the other platforms. Many games will have issues on macOS that run and render fine on Windows and Linux. This is working as intended for now. No need to tell me about issues you find with the macOS port. I know.

- Q: Can you add ARM64 Linux or Windows builds?<br>
  A: Yes, and I choose not to, there is no need to send me PRs or requests for these builds. They will be made available when I'm ready to support them, in the meantime you can build from source if you really want to run them.
  
- Q: Will you accept donations of money or games/hardware for testing?<br>
  A: No, this project is strictly a hobby and I would like to keep it that way. I pay for all games/hardware I test out of my own pocket and do not want to receive any compensation for my work on Xenia.

