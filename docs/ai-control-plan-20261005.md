# An assistant that runs the phone

Goal: an assistant that can do anything the user can do on the phone:
change a setting, start an app, set an alarm, find a file, write a text. It
should be a small model on the phone first, with a larger cloud model
available when wanted. Later, a model fine-tuned on how this phone works.

## The rule for every feature from now on

Every setting and action the UI offers must also be reachable without the
UI, through a tool that takes arguments and answers in JSON. The UI is then
one client and the assistant another. Most of the phone already works this
way: the shell and Settings call helpers, and the shell has an IPC
interface.

**Helpers:** each is `omarchy-mobile-NAME`, mostly with a JSON answer.

| Area | Helper | Reads | Changes |
| --- | --- | --- | --- |
| Wi-Fi | `wifi` | networks, connection | connect, forget, radio |
| Bluetooth | `bluetooth` | devices, power | power, pair, connect |
| Sound | `volume` | volume groups | set, mute |
| Brightness, torch, haptics | `controls` | state | set |
| Preferences | `prefs` | all | set |
| Theme | `theme` | themes, backgrounds | set, background |
| Sleep | `sleep` | policy | — |
| Alarms, timers | `clock` | list | add, remove, snooze |
| Calendar | `calendar` | events | add, edit, remove |
| Contacts | `contacts` | search, card | add, edit |
| Notes | `notes` | list, search, read | write, delete |
| Files | `files` | list | move, delete |
| Calls | `phone` | calls | dial, answer, hang up |
| Texts | `messages` | threads, thread | send |
| Screen | `screen` | recording | shot, record |
| NFC | `nfc` | state | on, off |
| Rotation | `rotation` | state | lock |
| Battery | `battery` | level, health | — |
| Apps | `app` | list, grants | launch, grant |

**Shell IPC:** `quickshell ipc … call mobile FUNCTION` (apps, launchApp,
settings, lock, controls, screenshot, record, volume, workspace, windows,
focus, inputState, …), plus the camera's (`call camera …`). The browser
opens a URL by starting it.

**Gaps to close as each area is touched:**
- Brightness and display modes have no single writable helper.
- Settings pages that only exist in QML (Security, Apps) need helper
  commands.
- No helper describes itself for an assistant: its commands, their
  arguments, what they change.

## The action registry

Each helper describes its commands in a small JSON file installed beside
it: `~/.local/share/omarchy-mobile/actions/NAME.json`. For each command:
- **name** (`wifi.connect`) and a one-line **description**;
- **arguments** as a JSON schema;
- the **command** to run;
- a **risk** level:
  - `read`: only reads;
  - `change`: changes a setting;
  - `sensitive`: costs money, reaches people or deletes something (a call,
    a text, a payment, deleting files);
  - `forbidden` for the assistant: the PIN, fingerprints, grants.

An action broker (a small daemon) is the assistant's only way in:
- It reads the registry and offers the actions as tools to whichever model
  runs, in the function-calling format that model takes.
- It runs `read` and `change` actions directly. Every change is logged and
  can be undone where the helper can undo it.
- For each `sensitive` action it asks first, with a sheet from the shell
  that shows exactly what will happen ("Text Mom: 'running late'"). It
  never dials or texts a number the user has not approved.
- It never runs `forbidden` actions.
- It can be turned off in one tap, and is off while the phone is locked
  unless the user allows it.

The same registry feeds the computer link, so a desktop assistant can ask
the phone to do things through the same broker.

## Models

- **On the phone:** a 1-3B instruction model with function calling, served
  by llama.cpp (OpenAI-style API on a local socket), on the CPU first.
  Speed and power are measured in `devices/pixel7pro/docs/llm-20261005.md`.
  It handles quick commands offline ("turn on the torch", "alarm for 7").
- **In the cloud (optional):** the same tools offered to a large model (a
  Claude model through the API, or through the user's own server) for longer tasks. The
  broker sends it only what a task needs. Each request says which model
  answered.
- **Routing:** the small model answers or hands off ("this needs the
  cloud model"), or the user picks.

## Fine-tuning for this phone

Training data, generated from the registry rather than written by hand:
1. requests to tool calls, from each action's description and argument
   schema (several phrasings each, with argument values);
2. multi-step tasks (find a contact, then text them; check the battery,
   then dim the screen);
3. refusals and confirmations for sensitive actions;
4. the docs and settings descriptions, for questions about the phone.

LoRA on a 1-3B base, run on the laptop, evaluated on held-out requests per
action, re-run when the registry changes.

## Seeing the screen (later)

For "what's on my screen" and acting inside apps:
- **Screenshots:** `omarchy-mobile-screen shot` for a vision model.
- **An accessibility tree:** Quickshell apps can publish their controls
  (name, role, state) over IPC.
- **Web apps:** through the browser's page scripts.

## Order

1. Measure small models on the phone (llama.cpp): speed and power.
2. The registry format, and descriptions for the helpers above.
3. The broker, with confirmations in the shell.
4. A voice front end (whisper.cpp in, piper out) and a text box in the
   shade.
5. Cloud model routing.
6. Fine-tuning data from the registry.
