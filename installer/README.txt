MKX Access 0.4.3 - screen reader support for Mortal Kombat X
=============================================================

MKX Access makes Mortal Kombat X (Steam, PC) speak through the NVDA
screen reader. It reads the game's own text as you move through menus,
so you hear the same words a sighted player sees.

This is a test version. To install it, see INSTALL.txt in this folder,
or just run Install.


WHAT IT READS

Menus
- Every menu item as you move to it, followed by its description.
  For example: "TRAINING, Master the ways of Mortal Kombat."
- The menu title when you enter a submenu.

Settings and options
- Each setting with its current value, name first:
  "BLOCK MODE: OFF", "MUSIC, 46".
- New values right away as you change them. Holding the arrow on a
  volume slider reads "45, 44, 43" as it moves.
- This works in the Options menu, AI Options, Practice Options and
  similar screens.

Character, variation and stage select
- The fighter under your cursor, the variation name with its
  description, and the stage name.

Pause menu and move list
- Each move's name, its input in words, and its attack type.
  Button codes are turned into words, for example:
  "Spear, back forward front punch" or "Low Poke, down front punch, Low".
- Move list section tabs, such as "SPECIAL MOVES".

Practice mode
- The hit level of your attacks as they land: HIGH, MID, LOW,
  OVERHEAD. Your own button presses are not read, so it stays quiet
  while you practise.

Online
- Matchmaking status as it changes: "LOOKING FOR OPPONENT",
  "WAITING FOR OPPONENT", "MATCH FOUND".
- Your opponent's name, ping, and the accept and decline buttons.

Messages and popups
- Pop-up questions and their choices, for example:
  "Are you sure you want to quit the game?, YES, A, NO, B".
- Loading and status messages such as "Updating Game Data...".

What it leaves out, to keep things quiet
- Timers, scores and other numbers that change constantly.
- The WB Play news panel and the online news ticker.
- The practice mode input history.


HOTKEYS (only while the game window is in front)
  F5  repeat the last thing spoken
  F6  read everything on the current screen. Use this if a screen
      seems silent.
  F7  turn auto-reading of new text on or off. Menus still read
      when this is off.
  F8  mute or unmute the mod


KNOWN LIMITS
- Inviting a friend to a private match happens in Steam's overlay,
  which the mod can't read. Instead, your friend can open Steam's
  friends list and choose Join game on your name, or you can choose
  Invite to game on theirs.
- Fights themselves are not narrated beyond practice mode hit levels.
  The game's own sounds still tell you most of what is happening.
- Steam version only. On a different version, the mod says "could not
  hook this game version" and stays out of the way.
- Best used offline. The mod only reads text and doesn't change
  gameplay, but keep it out of ranked online matches to be safe.


REPORTING PROBLEMS
After playing, the game's Binaries\Retail folder has a file called
MKXAccess.log. Send it with a short note about where things went
wrong, for example "Krypt was silent". Each launch overwrites the log,
so send it before playing again. You can also report problems at
https://github.com/ItsTBlaze/MKX-accessibility/issues


CREDITS
Made by ItsTBlaze. NVDA's controller library is from NV Access,
licensed under the LGPL 2.1 (see LICENSE-NVDA.txt).
Project page: https://github.com/ItsTBlaze/MKX-accessibility
