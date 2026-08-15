# kilrogg

Low-latency one-way Windows screen streaming over LAN, built for game
streaming (the whole screen changing every frame). No audio, no input — just
pixels, as fast as possible.

## Pipeline

```
sender:    Desktop Duplication ──► [mailbox] ──► hardware H.264 (NVENC etc.) ──► [send queue] ──► TCP (TCP_NODELAY)
receiver:  TCP ──► hardware H.264 decode ──► [mailbox] ──► D3D11 flip-model present (vsync off)
```

Each side is two threads joined by a single-slot, latest-wins **mailbox**
([mailbox.h](src/common/mailbox.h)): if a consumer falls behind, stale frames
are dropped *before* encoding, never after.

- **Capture**: DXGI Desktop Duplication of the primary output. Frames stay on
  the GPU from capture through encode — no CPU copies on the sender.
  `AcquireNextFrame` timing out on a static screen is normal and simply
  produces no traffic.
- **Encode**: the GPU vendor's H.264 encoder via Media Foundation
  ([mf_encoder.cpp](src/sender/mf_encoder.cpp)) — CBR, low-latency mode,
  ~40 Mbit/s default. BGRA→NV12 conversion runs on the GPU video processor.
- **Wire**: `Hello` then length-prefixed H.264 Annex B packets; an IDR is
  forced on every (re)connect. See [protocol.h](src/common/protocol.h).
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
  on vblank.

`--codec lz4` selects the original lossless dirty-rect + LZ4 path (pixel-
perfect, near-zero traffic on a static desktop, but tops out around 10 fps
when the whole screen changes). Kept as a debug reference; the receiver
auto-detects the mode from the handshake.

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
the next connection. Sender flags: `--bitrate N` (Mbit/s, default 40),
`--codec h264|lz4`, `--port N`, and `--dummy`, which streams a synthetic
bouncing square instead of the desktop — useful for testing the pipeline
without capture, including over loopback.

## Current limitations / roadmap

- Primary monitor only; no monitor selection.
- No mouse cursor overlay (Desktop Duplication delivers the cursor separately;
  pointer-only updates are currently skipped).
- Display resolution changes mid-stream exit the sender rather than
  renegotiating.
- No adaptive bitrate: the sender encodes at `--bitrate` no matter what the
  link can carry. Overflow is handled (video is dropped and an IDR forced,
  and the stats line says so) but not avoided — lowering `--bitrate` is the
  fix.
- Requires a hardware H.264 encoder (any non-ancient GPU) and decoder.
- Unencrypted, unauthenticated TCP — LAN/trusted networks only.
