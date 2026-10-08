# Omarchy Link

The phone and a computer as a pair: the phone's texts, notifications and
battery on the computer, ringing the phone from it, and one Omarchy theme on
both. `omarchy-link` (Qt, C++) runs on each end; the computer shows it in the
bar with the Phone Link widget (`plugin/`).

## Protocol

KDE Connect's protocol, version 8 (`src/lan.cpp`, `src/device.cpp`):
- **Discovery:** each device announces its identity by UDP, to the broadcast
  address and each network's own, and to the addresses it knows.
- **Connection:** the device that hears an announcement opens TCP, sends its
  identity in plain text, then both start TLS with roles reversed.
- **Identity:** each sends its identity again inside TLS. A certificate's
  common name must be the device ID; a paired device's certificate must be
  the one saved at pairing.
- **Pairing:** a request carries a timestamp. Both ends show the same eight
  characters (SHA-256 of both public keys and the timestamp, as KDE Connect
  computes them), and the receiving end accepts with a notification button.

**Ports.** KDE Connect uses UDP 1716 and TCP 1716-1764; the link uses the
same scheme from 1816. So it stays apart from a KDE Connect app on the same
computer (Flux, GSConnect): neither sees the other's devices, and they do
not share a socket. `"kdeconnectPorts": true` in
`~/.config/omarchy-link/settings.json` switches to KDE Connect's ports.

**Who connects.** A computer connects to a phone; between two of a kind, the
smaller device ID connects. A computer's firewall then lets the phone's answer
in as a reply. To find the phone by broadcast, though, the computer must
accept UDP 1816 and TCP 1816-1864 from the network:
```
sudo ufw allow 1816/udp && sudo ufw allow 1816:1864/tcp
```
Without that, `omarchy-link connect PHONE_ADDRESS` on the computer finds the
phone directly. A paired device's last address is announced to as well.

**Packets.** The standard packets, plus a few of the link's own:
- **Battery:** `kdeconnect.battery`.
- **Notifications:** `kdeconnect.notification`, and `.request` (all of them,
  or dismiss one) and `.action`.
- **Texts:** `kdeconnect.sms.request_conversations`,
  `.request_conversation`, `.request` (send) and `.messages`. Each message
  carries an `omarchy` object (contact name, state, unread count).
  `omarchy.sms.read` marks a thread read.
- **Calls and ringing:** `kdeconnect.telephony` and
  `kdeconnect.findmyphone.request`.
- **Theme:** `omarchy.theme`, `omarchy.theme.request`, `.files`, `.chunk` and
  `.result` (below).

## The phone (`src/phone.cpp`)

The mobile shell runs the daemon (`overlay/mobile/LinkAdapter.qml`) and is
its adapter:
- **Notifications:** it hands over each notification and its closing. A
  computer's dismissal or button press comes back to it.
- **Ringing:** the alarm tone and a buzz until Stop, for a minute at most.
- **Texts:** they come from `omarchy-mobile-messages`. A phone number's
  conversation ID is its last ten digits; a sender name gets a number from
  its hash, above any ten-digit one.
- **Pushing changes:** a change to the messages database sends the newest
  message of each conversation that changed.

## The computer (`src/desktop.cpp`, `plugin/`)

The daemon runs as the user service `omarchy-link.service`:
- **Notifications:** the phone's notifications become desktop notifications.
  Dismissing one dismisses it on the phone, and its buttons work.
- **Texts:** texts sent while the phone is away wait in `outbox.json` and go
  when it links again.
- **The bar:** the widget dims while the phone is away and takes the accent
  for unread texts or a pairing request.
- **The panel:**
  - pairing;
  - texts: conversations, a thread with a reply box, a new message;
  - the phone's notifications;
  - ringing;
  - the switches.

## Theme

Each end sends its theme when a device links, whenever it changes, and when
`theme.send` is switched on. The message names the theme and background file
and says how long ago it changed, by the sender's own clock; the two clocks
need not agree (the phone's ran 45 s ahead on October 4, which swapped
which choice looked newer). The other end follows when its `theme.follow`
switch is on (the phone's is, the computer's is not) and the choice is newer
than its own.

A theme or background missing on the following end is asked for and sent
over the link as a gzip tar, in 768 KiB chunks checked with SHA-256. No git
or network is needed, and a theme made by hand travels as well as a stock
one. The archive holds:
- `theme/`: the theme folder, without `.git`;
- `backgrounds/`: the user's backgrounds for it, from
  `~/.config/omarchy/backgrounds/NAME`.

Broken links are left out: themes carry links into their author's home
(Purple Haze's `background` files). A theme with only `alacritty.toml` gets
the `colors.toml` that `omarchy-theme-colors-from-alacritty` makes from it,
since the phone's tools need one.

They are installed where Omarchy keeps them. Only plain files and folders
under those two names are unpacked. The theme is then set with each end's
own tool: `omarchy-theme-set` and `omarchy-theme-bg-set` on a computer,
`omarchy-mobile-theme` on the phone. A failure shows on both ends.

## Sleep

The phone's Wi-Fi is off in deep sleep. TCP keepalive (30 s, then 3 × 10 s)
ends the computer's link to a sleeping phone, and the widget shows it as
away. The phone announces itself when its network returns. Notifications
and new texts go out then; texts written on the computer wait until then.
Wake-on-Wi-Fi (deep-sleep plan, stage 6.3) would make this immediate.

## Install and test

- **Computer:** `overlay/link/install.sh` builds the daemon, installs the
  user service and the widget, and enables it on the right of the bar.
- **Phone:** `overlay/mobile/install.sh` builds the daemon; the shell starts
  it.
- **Command line:** `omarchy-link state | watch | pair ID | accept ID | ring ID |
  connect ADDRESS | feature NAME on|off | call JSON`.
- **Test:** `tests/link/test_loopback.py BUILD/omarchy-link` runs two daemons
  (phone and computer) over loopback, each with its own XDG directories and a
  private session bus. It checks pairing and its code, texts, notifications
  and their actions, ringing, a theme sent over the link (with its
  backgrounds) and a later theme pick, and unpairing.

Still to come: clipboard sharing (opt-in), sending files and pictures, the
phone's Settings page for linked computers, and replying to a phone
notification from the computer.
