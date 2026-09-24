# kilrogg

Low-latency one-way Windows screen streaming over LAN, built to hold up even
when the whole screen changes every frame. No audio, no input — just pixels, as
fast as possible.

## Pipeline

```
sender:    Desktop Duplication ──► [mailbox] ──► hardware H.264/HEVC (NVENC etc.) ──► [send queue] ──► TCP (TCP_NODELAY)
receiver:  TCP ──► hardware decode ──► [mailbox] ──► D3D11 flip-model present (vsync off)
                └──────────────── back-channel: keyframe requests, clock probes ───────────────►
```

Each side is two threads joined by a single-slot, latest-wins **mailbox**
([mailbox.h](src/common/mailbox.h)): if a consumer falls behind, stale frames
are dropped *before* encoding, never after.

- **Capture**: DXGI Desktop Duplication of one display — the primary by
  default, `--display N|name` for any other, `--list-displays` to see what this
  machine has. That choice is not only about which screen: duplication works
  only between an output and the adapter that owns it, so the display picked is
  also the GPU that capture, the BGRA→NV12 conversion and the encode all run
  on. Left to itself `D3D11CreateDevice` takes whichever adapter Windows lists
  first, which on a hybrid machine is as likely to be the iGPU as the card driving
  the display, and nothing said which one it had been; the startup line now
  names the monitor and the GPU together. The desktop is asked for BGRA8
  explicitly rather than given whatever it happens to be in. With HDR
  switched on the desktop composites as scRGB half-float, which nothing
  downstream can use: the copy into the capture pool is a format mismatch,
  which D3D11 answers by doing nothing rather than by failing, so the stream
  becomes a texture nobody ever wrote to and no error says so. `DuplicateOutput1`
  takes a list of formats the caller accepts and has DXGI convert, which is
  Windows' own HDR-to-SDR mapping rather than one guessed at here — and it is
  the only route available, since the GPU's video processor will not take a
  float surface as *input* to convert either (measured on an RTX 4090:
  unsupported outright). The format frames actually arrive in is read back from
  the texture and treated as part of the display mode, so a machine where the
  conversion does not happen says so at startup instead of streaming black.
  Frames stay on the GPU from capture through encode — no CPU copies on the
  sender. `AcquireNextFrame` timing out on a static screen is normal and simply
  produces no traffic. Between clients the capture thread is parked and the
  duplication released: acquiring and copying a full desktop image costs the
  same whether or not anyone is watching, and a sender left listening on an
  idle machine should cost nothing at all (measured: zero CPU time over eight
  seconds with no client attached).
- **Encode**: the GPU vendor's encoder via Media Foundation
  ([mf_encoder.cpp](src/sender/mf_encoder.cpp)) — CBR, low-latency mode,
  ~40 Mbit/s default. H.264 High profile where available (its 8×8 transform is
  worth a lot on text) falling back to Main, or HEVC with `--codec hevc`.
  BGRA→NV12 conversion runs on the GPU video processor.
- **Frame rate**: a CBR encoder divides its bitrate by the frame rate it was
  configured with to get a budget per frame, so that number has to be the one
  it is actually fed. Capture polls with a zero timeout and has no rate of its
  own — a 175 Hz desktop produces frames at 175 Hz — so the sender reads the
  display's refresh rate, tells the encoder that, and paces its own submissions
  to match: a frame that arrives early is held rather than sent, and newer
  frames replace it while it waits, so what goes in at each slot is the latest
  one. `--fps N` overrides the reading.
- **Spending the whole bit budget** ([rate_control.cpp](src/sender/rate_control.cpp)):
  the rate the encoder divides by is the display's, but content only reaches
  that while something redraws the screen every single refresh. A 72 fps game
  on a 175 Hz panel is handed 41% of the frames the encoder budgeted for and
  spends 41% of the bitrate — measured here, 22 Mbit/s of a commanded 40. Those
  bits are not saved, just never spent, and the picture is worse for it. So the
  sender counts the frames it actually submits and scales what it *commands* by
  how far short they fall, leaving the same bits to be spent on fewer, better
  frames. The wire rate is unaffected — that is still whatever rate control
  asked for — so the send queue's budget and the AIMD loop both go on measuring
  the link and not the command. The correction climbs a step at a time and drops
  at once, for the same reason rate control does: a command that turns out too
  high is paid for in a queue overflow, which costs an IDR. It is capped at 3x,
  which covers anything down to a third of the panel's rate; below that `--fps`
  set to what the content really manages remains the exact answer. `--no-adapt`
  turns this off along with the rest of rate control, so an A/B comparison is
  still a comparison of one fixed number.
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
- **Reconnection**: a dropped connection is not the end of a session. The
  receiver reconnects on its own — with backoff, keeping its window, its D3D
  device and the last frame it drew, so a link that blips for a second costs a
  second of frozen picture and a message over it rather than a trip to the
  other machine to restart anything. Only closing the window (or Esc) ends it.
  Before the *first* connection there is a thirty-second deadline instead:
  that early, a failure is more likely a wrong address than a blip, and a
  receiver that retried forever would just sit there looking like it worked.
- **Display mode changes**: dimensions are settled at the handshake and fixed
  for the life of a connection, so a resolution change is handled by ending the
  connection. Capture adopts the new mode, the sender drops the client, and the
  receiver reconnects into a fresh `Hello` carrying the new size — which it
  adopts by recreating its textures, not its window. Alt-tabbing into a game
  that sets its own mode used to take the sender down with it. A refresh rate
  change on its own goes the same way: the textures are still the right size,
  but the encoder is configured for the old rate, and a fraction of a second of
  black costs less than encoding against the wrong bit budget until the client
  leaves.
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
  A client that reconnects picks up where its link left off rather than
  starting at the ceiling again. This matters most in exactly the case that
  produces reconnections: a link bad enough to drop the connection is a link
  that would otherwise be rediscovered the only way rate control can — by
  overflowing the send queue and spending an IDR on the recovery, on every
  reconnect. The memory is per client address and deliberately short: exact for
  a minute, relaxing toward the ceiling after that, forgotten after ten. A rate
  learned on a bad afternoon should not pin the picture down for the evening.
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
over the back-channel and keeps the offset from the shortest round trip seen
*lately* — the sample least distorted by queueing, from a window half a minute
wide. The window is the part that matters over a long session: two machines'
clocks drift apart, which is what the later probes are for, and a best-ever
sample would mean the first lucky probe won and every one after it was thrown
away. Until a probe lands, the cross-machine figures read `--` rather than a
fabricated number. The sender's own 5-second
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

## Tests

```
ctest --test-dir build -C Release
```

The suite covers the parts of the pipeline that are decisions rather than
device calls: the AIMD rate controller and the frame-rate budget that sits on
top of it, the per-client rate memory and its expiry, the clock-offset
estimate, the latest-wins mailbox and the capture gate, cursor shape decoding,
`--display` resolution, the numbers taken off the command line, and the
validation every header off the wire has to pass. None of it needs a GPU, a socket or a screen — which is the point, since
everything else here can only be exercised by pointing it at a real desktop.

doctest is fetched at configure time and pinned, like LZ4;
`-DKRG_BUILD_TESTS=OFF` skips the fetch and builds only the two executables.

## Run

On the machine being shared:

```
build\Release\kilrogg-send.exe            # listens on port 47800
```

On the viewing machine:

```
build\Release\kilrogg-recv.exe <host-ip>
```

Esc or closing the window quits the receiver; nothing else does. If the sender
goes away — restarted, rebooted, or just a link that dropped — the receiver
says so on the window and keeps trying until it comes back. The sender keeps
listening for the next connection.

Sender flags: `--bitrate N` (Mbit/s, default 40 — a ceiling, not a fixed rate),
`--min-bitrate N` (how far the link is allowed to push it down, default 3),
`--no-adapt` to pin the rate at `--bitrate` instead, `--codec h264|hevc|lz4`,
`--gop N` (frames between keyframes; the default is "only when asked"),
`--fps N` (default: the display's refresh rate), `--port N`,
`--display N|name` (default: the primary; the name is matched case-
insensitively against any part of `Dell AW3423DW (\\.\DISPLAY1) on NVIDIA
GeForce RTX 4090`), `--list-displays` to print that list and exit, `--list-encoders` to print every
hardware encoder this machine has, the GPU each belongs to and whether it will
actually start right now (the same activation that fails at connection time,
without needing a client — so it can be taken with and without whatever is
suspected of holding the card's encoder sessions), and
`--dummy`, which streams a synthetic bouncing square instead of the desktop —
useful for testing the pipeline without capture, including over loopback.

A `--display` that names no display, or names more than one, stops the sender
rather than falling back to the primary: capturing a screen other than the one
asked for is not an improvement on saying so. The numeric flags stop it for the
same reason: `--bitrate` and `--min-bitrate` take a whole number of megabits
from 1 to 1000, `--port` a port from 1 to 65535, and anything else — a typo, a
negative, a number past what the arithmetic carries — is refused rather than
folded into the nearest legal value. Folding is how `--bitrate 5000` used to
leave the sender announcing one rate and encoding at another.

Receiver flags: `--stats` for the latency overlay and `--smooth` for the
waitable-swapchain present mode. The optional port after the host is checked
the same way as the sender's `--port`, and refused the same way.

`--codec hevc` is a request, not a demand: the receiver advertises what it can
decode and the sender falls back to H.264 if either end lacks HEVC. It is worth
asking for — on the same content HEVC used a third of H.264's bitrate here.

With no `--codec` at all, the sender may reach for HEVC on its own, for frame
rate rather than for bitrate. H.264's levels are a macroblock-per-second budget
and the encoders built into GPUs stop at level 5.2, so a large display at a
high refresh rate can ask for a level nobody offers; the encoder's entire reply
is a refused media type, and the sender answers by walking the frame rate down.
2560×1600 at 240 Hz settles on 129 fps, which is felt as judder on a 240 Hz
panel. HEVC budgets luma samples instead and has room to spare. So when H.264
lands short of the display's rate and the receiver can decode HEVC, the sender
builds the encoder again as HEVC and keeps it if it bought frames.

This reacts to what the encoder settled on rather than predicting it from the
spec, because the spec is not what the hardware does: NVENC took 3440×1440 at
480 fps here, against the 107 level 5.2 nominally allows, so anything computed
up front would switch codecs on machines that never needed it. The second
attempt is therefore paid for only on the connections that were going to judder
anyway, and an explicit `--codec` is left alone — that is an instruction, not a
preference.

## Current limitations / roadmap

- One display at a time, settled at startup: `--display` cannot be changed
  without restarting the sender, and there is no mode that spans several
  monitors into a single stream. A display unplugged mid-session is a lost
  duplication that never comes back, rather than a fall back to another one.
- A display mode change costs a reconnect rather than being absorbed mid-
  stream, which is a fraction of a second of black. One that lands while no
  client is attached is not noticed until capture resumes, so the first client
  after it is dropped and reconnects a frame or two in.
- Content slower than a third of the display's rate still under-spends its
  bitrate: the correction that answers this is capped at 3x, because the cost
  of commanding too high is a queue overflow and an IDR, and the cap bounds the
  one control interval it takes to notice content speeding back up. A 30 fps
  game on a 175 Hz panel is past that. `--fps` set to what the content actually
  manages remains exact where the correction is only approximate.
- An HDR desktop is streamed by having DXGI convert it to SDR at capture, so
  what the receiver shows is Windows' HDR-to-SDR mapping, not the picture as
  the local display renders it. There is no HDR path end to end: the stream is
  8-bit BT.709 throughout.
- Rate control reacts rather than predicts: it learns a link is too slow by
  filling the queue on it, so the first second or so of a *new* link going bad
  still costs dropped video and a resync. What it remembers, it remembers only
  in memory — restarting the sender forgets every client's link.
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
