# Daily driver plan (October 5)

What the Pixel 7 Pro still needs before it can be the only phone. Written
after a survey of the repo, the apps and the phone, at the user's request:
"come up with whatever else you think is needed", with their examples
(attach a photo from the camera, music controls in the shade, replying from
a notification, AI, the apps and features phones normally have).

## Where it stands

Works today: the 120 Hz display and touch, GPU, calls (VoLTE), SMS and
mobile data, Wi-Fi, Bluetooth, audio, all four cameras and 1080p video,
fingerprint unlock, sensors and steps, wired and Qi charging, deep sleep
(about 32 mA idle), alarms that wake the phone, the home screen with
widgets, notifications, the browser and web apps, Android apps in Waydroid,
the laptop link, and themes.

The gaps below are grouped by how much they block daily use.

## 1. Safety and reliability first

These are the ones a phone must not get wrong.

| Gap | Why it matters | Work |
|---|---|---|
| **Emergency calls** | No emergency button on the lock screen, and no special handling of 911/112 (IMS emergency session, no PIN needed, works without a SIM). | Emergency dialer above the lock; route emergency numbers through the modem's emergency path; test with the carrier's test procedure, never a live 911 call without the user. |
| **Location (GPS)** | Maps, navigation, emergency location, weather by place. The BCM4776 answers, but no position fixes yet. | Finish the GNSS bring-up (position session without the Android HAL), then a location service (geoclue backend) with per-app grants. |
| **MMS** | Group texts and pictures arrive as WAP pushes and are dropped. Many people send photos by text. | MMS receive and send over the carrier's MMSC (a second data context), in messages.py and the Messages app. |
| **Bluetooth call audio** | Calls in the car or on earbuds: only the earpiece and speaker work now. | HFP: route the modem's call audio over Bluetooth SCO; car kit call buttons. |
| **Voicemail** | No way to reach it except dialling. | Voicemail entry in Phone (dial the box; visual voicemail later). |
| **Display recovery** | Today a scanout fault (a buffer the display could not read) froze the screen until the display was power-cycled by hand. | The scanout driver resets its pipeline itself after a fault; the shell watchdog notices a stuck frame counter. |
| **Updates and rollback** | A broken update must not strand the phone. | Already planned (`devices/pixel7pro/docs/updates-20261001.md`); keep a known-good boot slot and a root snapshot. |
| **Backups** | Messages, contacts, photos, settings, keys. | Nightly backup to the laptop over the link, or to a server, encrypted. |
| **Storage encryption** | A lost phone exposes everything on the root. | Encrypted home (fscrypt or LUKS) unlocked by the PIN; verified boot later. |

**MMS, done the same evening (e4645d7, 7f02e0a):** pictures received
(the WAP push kept by the modem service, the message fetched from Tello's
server over rmnet0, which works beside Wi-Fi without a second data context)
and sent from Messages (camera or newest photos, shrunk to fit). Still open:
a real MMS in each direction (a send needs a recipient the user approves),
group threads (they need the phone's own number), video.

## 2. The user's examples

### Take a picture and attach it anywhere

**Done for web pages (a6ed552):** the browser's file picker (Take photo,
the newest photos) and the camera's capture mode, opened over the browser
by the shell and returning to it. Still open: xdg-desktop-portal for other
apps, Photos and Files as sources, several files at once.

Example: a self-hosted finance app has a button to take a picture of a
receipt. In a web page that is `<input type="file" accept="image/*"
capture>`; today the browser does not handle file requests at all.

- **A system picker**, the same for every app: a sheet with *Take photo*,
  *Record video*, *Photos* and *Files*.
- **The camera in capture mode:** `omarchy-mobile-camera --capture PATH`
  opens straight to the shutter, shows the shot with *Use* and *Retake*, and
  hands back the file.
- **The browser** answers the page's file request (QtWebEngine's
  `fileDialogRequested`) with the picker, honouring `accept` and `capture`.
- **Other apps** reach the same picker through xdg-desktop-portal
  (FileChooser), so GTK and Qt apps get it too. Android apps keep their own.

### Music in the shade and on the lock screen

**Done (848b303):** the MPRIS media card in the shade and on the lock
screen. Still open: the always-on display, headset and AVRCP buttons, an
MPRIS bridge for the browser and Android apps (neither publishes one yet),
a Music app.

- A media card in the shade from MPRIS (any player: the browser, Waydroid
  through a bridge, a native Music app): artwork, title, a seek bar,
  play/pause and skip.
- The same card on the lock screen and a small one on the always-on
  display.
- Headset and Bluetooth (AVRCP) buttons control it; the volume keys follow
  the playing stream.

### Answering from a notification

**Done (75716ac):** inline reply in banners and the shade, and texts'
Reply and Mark as read. Still open: missed calls' actions, the Android
notification bridge, smart replies.

- **Inline reply:** Quickshell's notification server supports the
  freedesktop `inline-reply` action. The toast and the shade get a reply
  field, and Messages' notifications offer *Reply* and *Mark as read*.
- **Missed calls:** *Call back* and *Text*.
- **Android apps:** a Waydroid bridge that turns Android notifications into
  desktop ones (with their actions and replies) so WhatsApp, Signal and
  others appear in the shade.
- **Smart replies** later from the assistant (below).

## 3. Apps a phone is expected to have

Existing: Phone, Messages, Contacts, Calendar, Clock, Calculator, Notes,
Files, Camera, Browser and web apps, Settings, and Tesla (private).

| App | Notes |
|---|---|
| **Photos** | Camera roll and folders, viewer with zoom, share, delete, edit (crop, rotate). Also the picker's photo source. |
| **Music** | Local library, MPRIS, the shade card; streaming through web apps. |
| **Maps** | OSM vector maps offline, search, routing (needs GPS). Google Maps as a web app meanwhile. |
| **Email** | IMAP/JMAP client, or Gmail/Fastmail as a web app with notifications. |
| **Authenticator** | TOTP codes (2FA), stored encrypted, backed up. Needed the day the old phone is gone. |
| **Recorder** | Voice memos, and transcription on the phone. |
| **Video player** | Local files, with hardware decode once the codecs are up. |
| **Scanner** | QR codes from the camera (Wi-Fi joins, links, payments) and documents to PDF. |
| **Wallet** | Boarding passes and tickets (pkpass), loyalty cards. Contactless payment is not possible without Google's certification. |
| **Podcasts, Reminders, Weather app, Compass** | Smaller ones; the weather card exists. |

## 4. Features Android and iOS have

- **Share sheet:** any app sends a link, text or file to any other
  (Messages, Notes, the laptop, AI). *Done (abe10bf): copy, Messages,
  Notes, browser and Downloads targets, from the browser, Camera and Notes;
  the laptop target waits for a `share` command in the link.*
- **Default apps and links:** which app opens `tel:`, `sms:`, `geo:`,
  `mailto:` and file types.
- **Notification control:** per-app on/off, priority and lock-screen
  privacy; Do Not Disturb schedules and exceptions (Do Not Disturb itself
  exists).
- **Hotspot and tethering.** *Hotspot done (dd36530): a virtual ap0 beside
  Wi-Fi, NetworkManager's shared mode, masquerading onto mobile data; in the
  shade and Settings > Network. Not yet tried with a client; USB tethering
  open.*
- **VPN:** WireGuard and Tailscale toggles. This also answers the Omarchy
  Link question from earlier: on the tailnet the phone and laptop sync away
  from home.
- **Wi-Fi calling.**
- **Contact and calendar sync:** Google, CardDAV/CalDAV.
- **Photo backup** to a server or the laptop.
- **Find my phone:** locate, ring, lock and wipe from the laptop or a web
  page.
- **Accessibility:** text size, bold text, colour filters, screen reader
  later.
- **Fast charging:** USB PD at 9 V (planned as stage B).
- **Night light** and display colour modes.
- **Battery saver** and per-app background limits. Web apps already stop
  drawing behind other apps (October 5); Android apps need the same.
- **Hardware video decode and encode:** video playback, the X feed, video
  calls and recording all use the CPU now.
- **Picture-in-picture** for video and calls.

## 5. AI

`docs/ai-control-plan-20261005.md` has the backbone: every action as a
described tool, a broker that asks before anything sensitive, a small model
on the phone (Qwen 1.5B at 13 tokens/s on the big cores) and a cloud model
through the user's own server. On top of that:

- **Hold the power key to ask**, by voice (whisper.cpp in, piper out) or
  text, from anywhere including the lock screen (read-only there unless
  allowed).
- **Dictation in the keyboard**, on the phone, with punctuation. *Done
  (653cb26): Moonshine's streaming models (44 MB or 136 MB), a microphone in
  the keyboard's strip, phrases typed as the speaker pauses; Settings >
  Keyboard downloads and picks the model.*
- **Notification summaries** and **smart replies** in the reply field.
- **"What's on my screen"**: a screenshot to a vision model through that server;
  later an accessibility tree from Quickshell apps.
- **Text from photos (OCR)**: copy a phone number from a sign, a receipt's
  total into a finance app.
- **Search everything**: notes, messages, files, photos (on-device
  embeddings), from the home screen.
- **Calls:** live transcript and a summary afterwards, with consent.
- **Routines:** "when I leave home, turn Wi-Fi off", run by the broker.
- **Fine-tuning** on the phone's own tool registry, as planned.

## 6. Performance follow-ups from today

- Scrolling: the browser now paces Chromium by Qt's frames (patch 0003), Qt
  apps flick at Android speeds, and the thermal limits follow stock's
  (kernel v40 and the skin guard). X still drops frames in fast flings
  because of its content: a 2400×1700 feed video decoded in software.
  Hardware decode (above) is the next real gain there.
- Waydroid apps in the background still run at full speed.

## Order proposed

1. **Safety:** emergency calls, display self-recovery.
2. **The daily loop:** inline reply, the media card, the photo picker with
   camera capture (receipts for a finance app), MMS.
3. **Away from home:** GPS and Maps, Bluetooth call audio, VPN (Tailscale),
   hotspot.
4. **Apps:** Photos, Authenticator, Music, Recorder, Scanner.
5. **Sync and safety nets:** contacts and calendar sync, backups, find my
   phone, encryption.
6. **AI:** registry and broker, then voice, then the rest above.
7. **Power and media:** hardware video codecs, fast charging, Android
   background limits.
