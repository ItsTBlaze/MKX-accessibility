# MKX Access

Screen reader support for **Mortal Kombat X** (Steam, PC). MKX Access reads the
game's menus aloud through [NVDA](https://www.nvaccess.org/): menu items and their
descriptions, option names with their values, character and variation select,
move lists, and popup messages.

Status: early testing (version 0.4.4). Feedback is very welcome; see
[Reporting problems](#reporting-problems).

## Install

1. Download **[MKXAccess.zip](https://github.com/ItsTBlaze/MKX-accessibility/raw/main/download/MKXAccess.zip)**.
   This link always gets the newest version (currently 0.4.4).
2. Unzip it, and make sure Mortal Kombat X is closed.
3. Open the unzipped MKXAccess folder and run **Install**. It finds the game on its
   own and tells you when it's done. Windows may ask whether to run a file from the
   internet: choose **Run**, or **More info** then **Run anyway**.
4. Start NVDA, then start the game. About 2 seconds after the game window appears
   you will hear "MKX Access loaded".

To update, download the new zip and run its Install the same way. To remove the
mod, run **Uninstall** from the same folder.

If Install can't find the game, copy the files by hand: in Steam, right-click
Mortal Kombat X, choose **Manage**, then **Browse local files**, open **Binaries**,
then **Retail**, and copy `dinput8.dll`, `MKXAccess.asi`,
`nvdaControllerClient64.dll` and `LICENSE-NVDA.txt` there.

## Hotkeys

These only work while the game window is in front.

| Key | Action |
| --- | --- |
| F5 | Repeat the last thing spoken |
| F6 | Read everything on the current screen |
| F7 | Turn auto-reading of new text on or off |
| F8 | Mute or unmute the mod |

## Reporting problems

Open an [issue](../../issues) and say where in the game it happened, for example
"options menu was silent". Please attach `MKXAccess.log` from the Retail folder.
The log is overwritten every time the game starts, so grab it before playing again.

## Notes

- Steam version only. On a different build the mod says "could not hook this game
  version" and stays out of the way instead of crashing.
- Best used offline. The mod only reads text and doesn't change gameplay, but keep
  mods out of ranked online matches.
- Compatible with [MKXHook](https://github.com/ermaccer/MKXHook): either project's
  `dinput8.dll` loads both.

## How it works

The game ships with its debug symbols (`MK10.pdb`), which name its internal
functions. MKX Access hooks the functions that put text on screen
(`UITextItem::setText`, `Scaleform::GFx::Value::SetText` and friends) and the ones
that move menu focus (`UIFocusElementWidget::GainFocus`, the grid cursor, and the
"GainFocus" animation every menu plays). Each text label has a path such as
`_root.mainMenuShift.mainMenu.menuLines.lineItem2.menuItem.centerJust.text`. When
focus moves, the mod finds the labels belonging to the focused item, adds any text
that changed with it (descriptions, frame data), turns button codes like
`[Down][Away][FK]` into words, and sends the result to NVDA.

`src/dinput8.c` is a small loader that forwards DirectInput to Windows and loads
every `.asi` file next to it.

## Building

Needs clang and lld (LLVM 18 works). No Windows SDK is required:

```
./build.sh
```

`test/harness.c` replays an `MKXAccess.log` through the speech logic on
Linux/macOS, so changes can be checked without running the game:

```
clang -fshort-wchar -w -O1 -o test/harness test/harness.c
./test/harness MKXAccess.log
```

`tools/repack_msf.py` rewrites `MK10.pdb` with a 4 KB block size so
`llvm-pdbutil` can read it.

## Credits

- [MKXHook](https://github.com/ermaccer/MKXHook) by ermaccer, whose work confirmed
  the game's addresses.
- NVDA controller client by NV Access (LGPL 2.1), via
  [Tolk](https://github.com/dkager/tolk).
