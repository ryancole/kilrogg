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
  Requests are honoured at most twice a second — an IDR is the largest packet
  the encoder emits, so answering every overflow with one immediately refills
  the queue that just overflowed. A request arriving inside that window is held
  rather than dropped.
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
- **Adaptive bitrate** ([rate_control.cpp](src/sender/rate_control.cpp)):
  `--bitrate` is a ceiling, not a fixed rate. Every 200 ms the sender looks at
  what the send queue is doing and retunes the encoder — AIMD, cutting hard
  and climbing back slowly, since guessing high costs a backlog flush, which
  costs an IDR, which is the most expensive thing that can go on the wire. It
  backs off on an overflow, or on a queue that stays half full across
  consecutive samples, down to `--min-bitrate` (3 Mbit/s by default); after
  five clean samples in a row it steps back up by a sixteenth of the ceiling.
  A cut aims just under what the wire was measured to carry rather than
  stepping down blindly — while the queue is backed up the send thread never
  idles, so that figure is the link and not the encoder. Depth is read as it
  *stands* rather than as it peaked, and no reading cuts the rate while the
  wire is carrying everything it was asked for: the encoder hands a frame over
  in one burst, and a queue caught mid-drain is not a slow link. On loopback
  that distinction is the difference between holding 40 Mbit/s and sawing away
  at it over megabyte peaks that drain instantly. `--no-adapt` pins the rate
  for A/B comparisons.
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

Sender flags: `--bitrate N` (Mbit/s, default 40 — a ceiling, not a fixed rate),
`--min-bitrate N` (how far the link is allowed to push it down, default 3),
`--no-adapt` to pin the rate at `--bitrate` instead, `--codec h264|hevc|lz4`,
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
- Rate control reacts rather than predicts: it learns a link is too slow by
  filling the queue on it, so the first second or so of a link going bad still
  costs dropped video and a resync. It also relearns from scratch on every
  reconnect, starting each new client at `--bitrate`.
- Rate control assumes the encoder honours a mid-stream
  `AVEncCommonMeanBitRate`. NVENC does (measured: commanded 4 Mbit/s settles at
  ~6 on the wire, commanded 40 settles at ~41, no IDR needed). An encoder that
  accepts the setting and quietly ignores it would leave the rate pinned; a
  readback mismatch is logged, but a vendor MFT that lies about both is not
  something the sender can detect. `--no-adapt` is the fallback.
- Requires a hardware H.264 encoder (any non-ancient GPU) and decoder.
- The two clocks are synchronised only well enough to attribute latency; the
  `network` and `capture to present` figures inherit the ping/pong estimate's
  error, which on a busy link is a millisecond or two.
- Unencrypted, unauthenticated TCP — LAN/trusted networks only.
