# Wisp

The phone's assistant: chats with a language model that runs on the phone,
typed or spoken, with pictures, and replies read aloud when wanted. The app is
`shell.qml`; everything else is `omarchy-mobile-assistant`
([`assistant.py`](../../assistant.py)), which the OS can use the same way
later.

- **Model**: llama.cpp's `llama-server` on 127.0.0.1:39281 with a key only
  the assistant knows. The first message starts it and it stops after ten idle
  minutes, so it holds no memory while Wisp is not in use. The models are
  Qwen3-VL 2B (the default) and 4B at Q4_K_M, each with its vision projector,
  downloaded from Qwen's own GGUF repositories and checked against SHA-256
  sums (Settings in the app, or `omarchy-mobile-assistant model install
  qwen3-vl-2b`).
- **Voice in**: the keyboard's voice typing (`omarchy-mobile-dictate listen`,
  Moonshine). An answer to a spoken question is read aloud.
- **Voice out**: Kokoro (82M parameters, Apache 2.0) through
  [kokoro-pi](https://pypi.org/project/kokoro-pi/), which runs the vocoder's
  convolutions in an int8 kernel for the CPU's dot-product instructions, in its
  own venv (`omarchy-mobile-assistant voice install`: about 460 MB, built on the
  phone in under three minutes). Settings offers nine of its English voices;
  "Heart" is the default. The app keeps one speaker (`voice serve`) while it is
  open, started as the microphone opens, and speaks each sentence as the reply
  is written, through `pw-play`.

  Tried on the Pixel 7 Pro (October 7-8), seconds of work per second of
  speech on the middle and big cores:

  | Engine | Alone | Beside the model writing |
  |---|---|---|
  | kokoro-pi, int8 | 0.24 | under 1, no gaps |
  | Kokoro in Moonshine's engine | 0.8 | 1.3-1.5, gaps of up to 1.5 s |
  | Kokoro in sherpa-onnx, int8 | 1.3 | — |
  | Kyutai Pocket TTS in sherpa-onnx | 8 | — |
  | ZipVoice (cloning) in Moonshine's engine | 2.3 | — |
  | Piper, `en_US-lessac-medium` (the first voice) | 0.14 | — |

  Beside the model, both share the memory bus: Kokoro in Moonshine's engine
  slowed to 1.3 and the model from 12.5 to 6.4 tokens a second.
- **History**: SQLite with FTS5 search over every message, in
  `~/.local/share/omarchy-mobile/assistant/assistant.db` (mode 600). Pictures
  taken for a chat are kept beside it in `pictures/`.
- **Memory**: "remember that …" keeps a fact; with "Learn from chats" on, the
  model is asked after each reply whether a message about the user (one with
  "I", "my", "we" …) held a lasting fact ("The user …"), and keeps it. Memories go into every chat's
  system text. They are listed, added and removed on the Memory page.

## The engine

`llama-server` is not packaged for the phone. It is a static CPU build of
llama.cpp, put at `~/.local/share/omarchy-mobile/assistant/bin/llama-server`
(or anywhere on `PATH`). The Pixel's is cross-compiled on the laptop against
the phone's sysroot, as in
[`devices/pixel7pro/docs/llm-20261005.md`](../../../../devices/pixel7pro/docs/llm-20261005.md):
building on the phone, at four jobs, browned it out.

```sh
cmake -B build-aarch64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=aarch64.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16 \
    -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF -DGGML_OPENMP=OFF
ninja -C build-aarch64 llama-server
```

## Speed on the Pixel 7 Pro

Measured October 7 with the screen on; generation runs on the two big cores
(CPUs 6-7) and reading on 4-7.

| Model | Writes | Reads | Memory | A picture |
|---|---|---|---|---|
| Qwen3-VL 2B | 12.5 tokens/s | 83 tokens/s | 3.5 GB | about 25 s |
| Qwen3-VL 4B | 5.6 tokens/s | 31 tokens/s | 5.5 GB | — |

With the screen dark, CPUs 4-7 are offline and the 2B writes under one token a
second; even with them held online it writes 2.8, because the kernel lowers the
memory clock (MIF) to 421 MHz. Wisp answers best with the screen on. With it on,
the memory clock stays at the display's 1352 MHz floor: at 2028 MHz
(`pixel_scanout.mif_floor_mhz`) the 2B wrote 14.1 tokens a second instead of
11.1, Kokoro was unchanged. Raising it while Wisp works needs a root-side
request, not built yet.
