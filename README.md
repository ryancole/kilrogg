# kilrogg

Low-latency one-way Windows screen streaming over LAN, built for game
streaming (the whole screen changing every frame). No audio, no input — just
pixels, as fast as possible.

## Pipeline

```
sender:    Desktop Duplication ──► [mailbox] ──► hardware H.264/HEVC (NVENC etc.) ──► [send queue] ──► TCP (TCP_NODELAY)
receiver:  TCP ──► hardware decode ──► [mailbox] ──► D3D11 flip-model present (vsync off)
                └──────────────── back-channel: keyframe requests, clock probes ───────────────►
```

Each side is two threads joined by a single-slot, latest-wins **mailbox**
([mailbox.h](src/common/mailbox.h)): if a consumer falls behind, stale frames
are dropped *before* encoding, never after.

- **Capture**: DXGI Desktop Duplication of the primary output. Frames stay on
  the GPU from capture through encode — no CPU copies on the sender.
  `AcquireNextFrame` timing out on a static screen is normal and simply
  produces no traffic.
- **Encode**: the GPU vendor's encoder via Media Foundation
  ([mf_encoder.cpp](src/sender/mf_encoder.cpp)) — CBR, low-latency mode,
  ~40 Mbit/s default. H.264 High profile where available (its 8×8 transform is
  worth a lot on text) falling back to Main, or HEVC with `--codec hevc`.
  BGRA→NV12 conversion runs on the GPU video processor.
- **Keyframes on demand**: the GOP is effectively infinite. A periodic IDR at
  3440×1440 is a bitrate spike big enough to be felt as a hitch on a
  constrained link, so instead one is emitted only when something asks: a fresh
  connection, a send-queue overflow, or the receiver reporting a decode failure
  over the back-channel. `--gop N` pins a fixed spacing if you want one.
- **Wire**: the receiver opens with a `ClientHello` listing the codecs it can
  decode, the sender replies with a `Hello` naming the one it picked, and the
  stream is then length-prefixed Annex B packets carrying the frame's capture
  timestamp. The receiver's back-channel carries keyframe requests and clock
  probes. See [protocol.h](src/common/protocol.h).
- **Send queue**: packets go out on their own thread
  ([packet_sender.cpp](src/sender/packet_sender.cpp)) so a slow link never
  blocks the encoder. The queue holds a quarter second of video at the
  configured bitrate; when it overflows, the *whole* pending video backlog is
  dropped and a fresh IDR is forced, since every queued P-frame after a
  dropped one is undecodable anyway. Cursor packets are never dropped. The
  5-second stats line reports the wire rate and the queue high-water mark, so
  a link that cannot keep up says so explicitly.
- **Decode/Present**: hardware decode (DXVA) on the presenter's own D3D11
  device ([mf_decoder.cpp](src/receiver/mf_decoder.cpp)); NV12 is converted to
  RGB in the pixel shader and drawn through a flip-model swapchain with sync
  interval 0 and `DXGI_PRESENT_ALLOW_TEARING`, so presentation never blocks
  on vblank. `--smooth` swaps that for a frame-latency waitable object at one
  frame of latency and sync interval 1: a refresh or so slower, but free of
  the judder you get when the capture rate and the local refresh rate disagree.

`--codec lz4` selects the original lossless dirty-rect + LZ4 path (pixel-
perfect, near-zero traffic on a static desktop, but tops out around 10 fps
when the whole screen changes). Kept as a debug reference; the receiver
auto-detects the mode from the handshake.

## Measuring latency

`kilrogg-recv --stats` draws an overlay breaking the glass-to-glass path into
the stages that can each be blamed separately:

```
encode    capture to the encoder's output, measured on the sender
network   the sender's clock to the receiver's, measured across machines
decode    packet in to decoded texture out
queue     decoded frame waiting for the present loop
scanout   the Present call to the vblank that displayed it (GetFrameStatistics)
```

The two machines share no clock, so the receiver runs an NTP-style ping/pong
over the back-channel and keeps the offset from the shortest round trip seen —
the sample least distorted by queueing. Until a probe lands, the cross-machine
figures read `--` rather than a fabricated number. The sender's own 5-second
log line reports encode latency and the encoder's *pipeline depth*: frames
handed to the MFT that have not come back out. Zero means low-latency mode is
genuinely in force and output is 1:1 with input.

## Build

Requires Visual Studio 2022+ and CMake 3.24+. LZ4 is fetched automatically
(pinned to v1.10.0), no vcpkg needed.

```
cmake -B build
cmake --build build --config Release
```

## Run

On the machine being shared:

```
build\Release\kilrogg-send.exe            # listens on port 47800
```

On the viewing machine:

```
build\Release\kilrogg-recv.exe <host-ip>
```

Esc or closing the window quits the receiver; the sender keeps listening for
the next connection.

Sender flags: `--bitrate N` (Mbit/s, default 40), `--codec h264|hevc|lz4`,
`--gop N` (frames between keyframes; the default is "only when asked"),
`--port N`, and `--dummy`, which streams a synthetic bouncing square instead of
the desktop — useful for testing the pipeline without capture, including over
loopback.

Receiver flags: `--stats` for the latency overlay and `--smooth` for the
waitable-swapchain present mode.

`--codec hevc` is a request, not a demand: the receiver advertises what it can
decode and the sender falls back to H.264 if either end lacks HEVC. It is worth
asking for — on the same content HEVC used a third of H.264's bitrate here.

## Current limitations / roadmap

- Primary monitor only; no monitor selection.
- Display resolution changes mid-stream exit the sender rather than
  renegotiating.
- No adaptive bitrate: the sender encodes at `--bitrate` no matter what the
  link can carry. Overflow is handled (video is dropped and an IDR forced,
  and the stats line says so) but not avoided — lowering `--bitrate` is the
  fix.
- Requires a hardware H.264 encoder (any non-ancient GPU) and decoder.
- The two clocks are synchronised only well enough to attribute latency; the
  `network` and `capture to present` figures inherit the ping/pong estimate's
  error, which on a busy link is a millisecond or two.
- Unencrypted, unauthenticated TCP — LAN/trusted networks only.
