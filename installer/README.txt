MKX Access 0.9.24 - screen reader support for Mortal Kombat X
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
- In practice, each move's frame data after its name: "Slap, front
  punch, High, start-up 11, active 3, recovery 13, plus 2 on block,
  plus 24 on hit, cancel 20, damage 2 percent, block damage 0.4
  percent". Values the game marks N/A are left out.
- Changing page says the page first: "SPECIAL MOVES, Tele-Kick, ...".
- Fatalities you haven't unlocked yet say "hidden" instead of their input.

Frame data in practice
After each of your attacks that hits or is blocked, in practice mode only:
- Detailed: start-up and advantage, for example "start-up 11, plus 24
  on hit" or "start-up 11, plus 2 on block, block damage 0.4 percent".
  With the hit and combo readout off it also says the hit count and
  damage. A string counts as one: the first hit's start-up and the
  advantage after the last hit.
- Plus and minus only: just the advantage, "plus 24 on hit".
- A knockdown says so before the advantage: "knockdown, plus 58 on
  hit". After a knockdown the number depends on how fast the dummy
  gets up; set the dummy's Getup Mode to its fastest to compare with
  the move list.
- Active frames, recovery and cancel can't be measured from a hit;
  the move list has them.
- Choose the style in the accessibility menu (Frame data in practice:
  detailed, plus and minus only, off). Ctrl+Shift+F10 turns it off and
  back on.

Damage and combos
- In practice mode, each single hit with its level and damage:
  "mid, 7 percent". It is read just under a second after it lands,
  once it's clear the hit isn't the start of a combo.
- In any fight, combos once they end, with the total damage:
  "3 hit combo, 8 percent". In practice mode your own combos also list
  each hit's level: "3 hit combo, 8 percent, high, mid, mid".
  Combos that pause mid-string (launchers, specials) are read once,
  when the game shows the combo as finished.
- Throws are read once, with their full damage: "throw, 12 percent".
- Hits that are part of a combo are not read one by one, so the
  readout keeps up. Your own button presses are not read either.
- F10 turns the damage numbers off and on. With them off you still
  hear hit levels and combo counts: "mid", "5 hit combo, overhead,
  mid, mid, mid, mid", "throw".
- Ctrl+F10 turns the hit and combo readout (levels and hit counts) off
  and on. With it off and damage numbers on, you hear just the damage:
  "7 percent".

Fight sounds
Short sounds tell you what is happening in a fight. You and your
opponent use the same sounds; the side they come from tells you who.
- Health: a sound when a fighter drops to 75, 50 and 25 percent.
- X-ray: a sound as soon as a fighter's super meter is full and the
  X-ray can be used.
- Health and X-ray sounds come from the side of that fighter's health
  bar: player 1 on the left, player 2 on the right.
- Walls: a steady hum while a fighter is close to a wall, from that
  wall's side. It gets louder the closer they get and is loudest in
  the corner. It stops when a round ends and in the pause and results
  menus.
- The mod works out on its own which fighter is you, at the start of
  every fight.
- Every sound is a .wav file in the "MKXAccess Sounds" folder in the
  game folder, and you can replace any of them with your own. See
  "About these sounds.txt" in that folder. Install never replaces a
  sound you have customized.

Accessibility menu
- Open it with F3, or hold Back and press Y on the controller. While it
  is open, the game gets no key presses or button presses.
- Auto read has three choices: off, everywhere, or everywhere except
  during fights. "Except during fights" keeps fights quiet (no
  "KOUNTER", "FIRST HIT" and similar) but still reads the pause menu,
  results and everything outside fights. Damage, combo and frame data
  readouts have their own settings and are not affected.
- Up and down choose, left and right change, B or Escape closes (or F3,
  or Back and Y again).
- In order: Auto read, Hit and combo readout, Combo damage numbers,
  Frame data in practice, Menu sounds, Overall sound volume, Your health sounds, Opponent
  health sounds, Your X-ray ready sound, Opponent X-ray ready sound,
  Wall hum, Close menu.
- On a sound, left and right set its volume in steps of 10 percent and
  play it at the new volume. 0 turns it off. A or Enter just plays it.
- Overall sound volume works on top of each sound's own volume. It
  only changes the mod's sounds, not the game or NVDA.
- Your choices are saved in MKXAccess.ini in the game folder and kept
  for next time.

Online
- Matchmaking status as it changes: "LOOKING FOR OPPONENT",
  "WAITING FOR OPPONENT", "MATCH FOUND".
- Your opponent's name, ping, and the accept and decline buttons.

Rooms
- The room list reads each room with its player count:
  "Black Dragon, 2 players".
- Inside a room, each player is read in full: "ZuRRK, level 1,
  playing Versus, 4 wins 12 losses, 61 percent win odds, ping 85
  milliseconds". Players who are free read as "looking for any match".
- If a player's ping or record updates while you're on them, the new
  value is read with its label: "ping 85 milliseconds".
- Player options (A on a player) and the chat box (X) are read.

King of the Hill
- Moving between the players in the theater reads each one:
  "Your Sins, 2 wins, 1 loss, streak 2, ping 99 milliseconds".
  An empty place reads "empty slot".
- The first time you land on a player, the theater's buttons are read
  too, each with its action: "back, FULLSCREEN. A, KOMBAT HISTORY.
  right trigger, PROFILE. start, ENTER AFK. left trigger, INVITE.
  left bumper, MUTE. B, EXIT". They are read again when they change,
  for example after going AFK.
- Pings that update every second are not read on their own.
- Paying respects reads each choice as you move: "PAY YOUR RESPECTS,
  NONE", "5", "6".

Button bars
- Rows of buttons are read with a pause between them, button first:
  "A, SELECT. Y, REFRESH. B, EXIT". The Back button (which the game
  calls Select) is read as "back".

Typing a room name or a chat message
- X on the room list (CREATE ROOM) opens a text box. Its title and
  keys are read, then: "Type with the keyboard. Enter to accept,
  Escape to cancel, F6 reads what you typed."
- Type the name on your keyboard. Each key you type is read back
  ("cap B", "l", "a", "space"), and Backspace reads the letter it
  removed. When the box is empty again you hear "blank".
- F6 while the box is open reads the title and everything typed so
  far: "CREATE ROOM, typed: Blaze room".
- The same works for room chat messages.
- If Steam would show its own on-screen keyboard (Big Picture mode),
  the mod has the game use its own readable text box instead.

Fight challenges
- When someone challenges you, it's read in one sentence:
  "sixpaths099 challenged you to Versus. Level 73, 341 wins 179
  losses, 3 percent win odds. 30 seconds to answer. A accept,
  Y decline, B minimize, X ignore player."
- A second challenge while you're answering the first:
  "PandaPlant also challenged you, 2 pending challenges".
- The 30 second countdown is only read at 10 and 5 seconds left:
  "10 seconds left to answer sixpaths099".

Inviting friends (private match, private King of the Hill)
- Steam's invite window can't be read, so the mod replaces it. When
  the game would open it, you hear "Invite a friend" instead.
- D-pad down or F11 goes to the next online Steam friend, D-pad up
  or Shift+F11 to the previous one. Friends already playing Mortal
  Kombat X come first: "TheOneLad, playing Mortal Kombat X, 1 of 3",
  "Hankiro, online, 2 of 3".
- A or Ctrl+F11 sends the invite: "Invite sent to Hankiro". Your
  friend gets a normal Steam game invite.
- While the invite screen is up, D-pad up, D-pad down and A only
  control the friend list, so they can't press anything in the game.
  B still works as normal to cancel. Once you leave the invite screen
  the controller works normally again.
- If NVDA isn't running, Steam's own window opens as usual.

Accepting invites
- When a friend invites you to a match while you're in the game, you
  hear "pdexinthelab invited you to a match. Press Back on the
  controller, or Control F11, to accept."
- Press Back (View on newer controllers) or Ctrl+F11 to accept. You
  hear "Joining pdexinthelab's match" and the game joins, just as if
  you had accepted in Steam.
- While an invite is waiting, the Back button only accepts it. An
  invite you don't answer is dropped after 2 minutes.

The Krypt
- Tips and messages, such as "Welcome to The Krypt!".
- Button prompts as they appear: "A, Open Tomb", and prices:
  "1,280, A, Purchase".
- Your location as you walk: "Krypt Gateway (0, 1)".
- Rewards, such as "YOU HAVE BEEN AWARDED, 4,000 KOINS".
- The pause screen's tab and its button options:
  "JOURNAL, X, return to Krypt entrance, Y, quit to main menu, B, back".

Messages and popups
- Pop-up questions and their choices, for example:
  "Are you sure you want to quit the game?, YES, A, NO, B".
- Button prompts that screens show, for example "B, back".
- Loading and status messages such as "Updating Game Data...".

What it leaves out, to keep things quiet
- Timers, scores and other numbers that change constantly, such as
  the Krypt clock.
- The WB Play news panel and the online news ticker.
- The practice mode input history.


HOTKEYS (only while the game window is in front)
  F3  open or close the accessibility menu (controller: hold Back,
      press Y)
  F5  repeat the last thing spoken
  F6  read everything on the current screen. Use this if a screen
      seems silent. While typing a name or message, F6 reads
      what you have typed so far.
  F7  auto read of new text: each press goes to the next choice,
      off, everywhere, or everywhere except during fights. Menus
      still read when it is off.
  F8  mute or unmute the mod
  F9  say which version of the mod is running
  F10 turn damage numbers on or off (hit levels and combos stay)
  Ctrl+F10 turn the hit and combo readout on or off
  Ctrl+Shift+F10 turn frame data in practice on or off
  F11 next friend to invite, Shift+F11 previous, Ctrl+F11 send invite
      (Ctrl+F11 accepts when a friend has invited you)


KNOWN LIMITS
- Only online friends are listed, since offline friends can't join.
- Fights are not narrated beyond damage, combos and the fight sounds.
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
