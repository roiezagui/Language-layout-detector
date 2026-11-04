# Layout Guard

A tiny Windows tray app that notices when you type a word in the wrong keyboard
layout (e.g. `akuo` while the Hebrew layout was expected) and, when the word ends
with Space / Enter / Tab, replaces it with the correct word and switches the layout.

- **One file.** A single native `LayoutGuard.exe` (~25 KB), no Python/.NET runtime, no installer.
- **Local only.** Keystrokes are kept in RAM for the current word, never written to disk, never sent anywhere.
- **Light.** One keyboard hook, one mouse hook, a tray icon, and a word-list lookup per word typed.

## Build

Visual Studio Build Tools (MSVC) or MinGW-w64, from a shell in this folder:

```bat
build.bat
```

Or directly:

```bat
gcc -O2 -s -static -mwindows -municode LayoutGuard.c -o LayoutGuard.exe -luser32 -lshell32 -lole32
```

## Languages

Layout Guard uses the **Windows spell checker** for every installed keyboard layout, so there is
nothing to set up for most people. A layout is used when Windows has a spell checker for its language.

To check that a language has one: Settings → Time & language → Language & region → (language) →
Language options → make sure the **Spell check** feature is installed. If a layout is not
used, check that its language is installed.

**Optional override:** a word list at `dict\<lang>.txt` next to `LayoutGuard.exe` (UTF-8, one
lowercase word per line, e.g. `dict\he.txt`) replaces the Windows spell checker for that language.

## Run

- Double-click `LayoutGuard.exe`. A tray icon appears; the tooltip shows the state.
- Left-click the icon to pause/resume. Right-click for **Enabled** / **Exit**.
- To start it at sign-in, put a shortcut to the exe in `shell:startup`
  (Win+R → `shell:startup`).

## How the decision is made

1. The app keeps the keys of the current word (up to 48), together with the layout each was typed in.
2. On Space, Enter or Tab it converts the keys under the active layout. If that text is spelled
   correctly in that layout's language, nothing happens.
3. Otherwise it converts the same keys under every other installed layout. If one of them is a
   correctly spelled word, the app sends Backspace × N, types the word correctly
   (Unicode input, so it does not depend on the layout), re-sends the Space/Enter/Tab,
   and asks the focused window to switch to that layout.
4. Words that are not spelled correctly in any of the layouts are left alone. This is deliberate: names, slang and typos
   are never "corrected".

The word buffer is reset on: focus change, mouse click, arrow/Home/End/Delete/Esc keys,
Ctrl/Alt/Win shortcuts, and words longer than 48 keys.

## Limitations

- **Spell-checker based**: a correctly spelled word in one language is also accepted there, and
  names or slang that the spell checker doesn't know are left alone.
- **Latency:** each word is checked with the Windows spell checker inside the keyboard hook. Windows
  may drop a hook that stalls, so very slow spell checks could disable it.
- **Layout switch** uses `WM_INPUTLANGCHANGEREQUEST`. Most apps follow it; some ignore it, in which
  case the text is still corrected but you may need to switch layouts yourself.
- **Dead keys** (accents typed as two keys) and IME input (Japanese, Chinese) are not corrected:
  the buffer is cleared, so they are left alone.
- **Caret position**: the app assumes text is typed at the end of the current word. Mouse clicks and
  navigation keys reset its memory, so it does not edit text it cannot see.
- **Password fields** are not detected. The hook sees all keystrokes in the session, but it only
  retains the current word and only rewrites it when a correction is actually triggered.
- Verified so far: compiles cleanly for Windows x64 with MinGW-w64 (`-Wall -Wextra`). It has **not yet been run
  on a Windows machine**, so the hook behaviour, layout switching and spell-checker decisions still need a manual test.

## Privacy

Nothing is logged, persisted or transmitted. The word buffer exists only in memory and is cleared
at every word boundary and focus change. Verify this yourself: the source is one file,
[`LayoutGuard.c`](LayoutGuard.c), and it has no network or file-write code paths. It only reads the optional `dict\*.txt` files.
