# The on-screen keyboard

`shell.qml` is the keyboard: a Quickshell layer window (namespace
`omarchy-mobile-keyboard`, overlay layer, no keyboard focus) in the theme's
colours, which change with the theme. `vkbd.c` (`omarchy-mobile-vkbd`) sends
its keys through `zwp_virtual_keyboard_v1` with an evdev/pc105/us keymap and
decodes swipes. It replaces wvkbd, which `keyboard.sh` still runs when the
keyboard is not installed or `~/.local/state/omarchy-mobile/keyboard-wvkbd`
exists.

## Layout

Gboard's, in ASCII:
- **Letters:** a long press types the hint in the key's corner: the digits
  on the top row, `@ # $ _ & - + ( )` and `* " ' : ; ! ?` below. Shift once
  for a capital, a long press on Shift for caps lock. After `. ! ?` and a
  space, or Enter, the next letter is a capital.
- **`,` and `.`:** a long press opens their alternatives; slide to one.
- **?123:** digits and the common symbols; **=\\<** the rest of ASCII, with
  Esc, Tab, a one-shot Ctrl and the arrows, for the terminal.
- Backspace repeats while held. Characters are typed when the finger
  lifts; each finger is followed on its own, for typing with two thumbs.

## Swipe typing

A finger that leaves a letter by more than half a key, onto another letter,
draws a trail. On release `vkbd` compares the path, resampled to 40 points,
with each candidate word's path through the key centres, among words whose
first and last keys are within a key of the path's ends:

    cost = d^2 / (2 * 0.24^2) + (s^2 + e^2) / (2 * 0.4^2) - ln p(word)

(`d` the mean point distance, `s` and `e` the end distances, all in key
widths). On noisy synthetic swipes over the 50 000 words this put the word
first 84 % of the time and in the top three 96 %. The best word is typed
(with a space before it after another word, or before a letter tapped
right after it), the three best show in the strip above the keys: tap one to
replace it; Backspace removes the whole word.

## Learning

`~/.local/share/omarchy-mobile/keyboard/user.txt` ("word uses") counts the
words the user keeps: a swiped word once the next word, a space or Enter
follows (not one taken back with Backspace or replaced by a suggestion, which
counts instead), and tapped words of two or more letters. A word in the list
gains 1e-4 of probability per use (about the 2000th word's share); a new word
("omarchy") becomes a candidate at its second use, so a one-off typo is not
learned. vkbd saves the list every five words and when it ends.

The word list is FrequencyWords' 50 000 most frequent English words
(OpenSubtitles 2018, CC BY-SA 4.0), fetched by `install.sh` to
`~/.local/share/omarchy-mobile/keyboard/en.txt` and checked by hash. It
splits contractions ("don" and "'t"); `vkbd` types "dont" as "don't" and so
on, with a share of the suffix's count.

`vkbd WORDS --test` reads `keys` and `swipe` commands on stdin without a
display, for tuning.

## Voice typing

The microphone at the strip's right end runs `omarchy-mobile-dictate listen`
(`dictate.py`): speech becomes text on the phone with Moonshine's streaming
English models (MIT), and nothing is recorded or sent. While it listens the
strip shows the phrase being said; each phrase is typed when the speaker
pauses, after a space, and without its capital when it carries on a sentence
(after a word or a comma; "I" keeps it). A second tap stops listening (what
was already said is still typed), as do 8 s with nothing said, 10 s with no
new words, five minutes, or hiding the keyboard. Before anything is installed
the microphone opens Settings > Keyboard, which downloads the engine (a
Python environment of its own, `moonshine-voice` from PyPI) and a model:
Fast (tiny, 44 MB) or Accurate (small, 136 MB).

On the Pixel 7 Pro, 44 s of speech at speaking pace: both models type each
phrase within about a second of the pause; Accurate gets more words right
("a season of light" where Fast heard "the season"). The engine makes its
passes over the phrase so far only when no audio is waiting, so a slow pass
means fewer partial updates rather than falling behind. ONNX Runtime aborts
as it loads while CPUs are offline, which the Pixel does with the screen dark;
`listen` says so instead.

## Shown and hidden

`keyboard.sh show|hide|toggle` writes the verb and the time in milliseconds
to `$XDG_RUNTIME_DIR/omarchy-mobile-keyboard.cmd`, which the window watches
(a file write is faster than an IPC client); a command from before the window
started is not replayed. The window reserves the shell's 24 px navigation
strip below its keys. Hyprland shows it above the lock screen, for passwords.

Android apps: the shell shows the keyboard when an Android text field takes
focus (Android's input method there is `devices/pixel7pro/waydroid/keyboard-bridge`)
and hides it when Android lets go. Over an Android app the keyboard reserves
no space; the bridge takes the same height inside Android instead, so the
app's window keeps its size.

Not yet: non-ASCII text (emoji, accents; the US keymap has none), word
prediction while tapping, a way to remove a learned word, haptics, voice
typing in other languages.
