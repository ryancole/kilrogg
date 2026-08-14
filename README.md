# kilrogg

Low-latency one-way Windows desktop streaming over LAN. No audio, no input —
just pixels, as fast as possible.

## Pipeline

```
sender:    Desktop Duplication ──► [mailbox] ──► dirty rects + LZ4 ──► TCP (TCP_NODELAY)
receiver:  TCP ──► LZ4 decode ──► [mailbox] ──► D3D11 flip-model present (vsync off)
```

Each side is two threads joined by a single-slot, latest-wins **mailbox**
([mailbox.h](src/common/mailbox.h)). If the consumer falls behind, the newer
frame replaces the pending one — but its dirty rects are merged in first, so a
dropped frame never loses screen updates (the frame always carries the full
desktop image; only the "what changed" list needs to survive).

- **Capture**: DXGI Desktop Duplication of the primary output, with dirty and
  move rects. `AcquireNextFrame` timing out on a static screen is normal and
  simply produces no traffic.
- **Wire**: per frame, only the dirty rects are sent, each LZ4-compressed.
  A full-frame rect doubles as the keyframe on (re)connect. See
  [protocol.h](src/common/protocol.h).
- **Present**: persistent GPU texture patched per-rect, drawn through a
  flip-model swapchain with sync interval 0 and `DXGI_PRESENT_ALLOW_TEARING`,
  so presentation never blocks on vblank.

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
the next connection. `kilrogg-send --dummy` streams a synthetic bouncing
square instead of the desktop — useful for testing the pipeline without
capture, including over loopback.

## Current limitations / roadmap

- Primary monitor only; no monitor selection.
- No mouse cursor overlay (Desktop Duplication delivers the cursor separately;
  pointer-only updates are currently skipped).
- Display resolution changes mid-stream exit the sender rather than
  renegotiating.
- Sender copies the full desktop to CPU each frame; copying only dirty regions
  (`CopySubresourceRegion` per rect) would cut GPU→CPU bandwidth a lot.
- Unencrypted, unauthenticated TCP — LAN/trusted networks only.
