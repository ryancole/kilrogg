#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>

namespace krg {

// Decides when the run loop hands a frame to the encoder, and how long it may
// block waiting for one in the meantime.
//
// Capture has no rate of its own: it polls with a zero timeout, so a 175 Hz
// desktop produces frames at 175 Hz. The encoder, meanwhile, divides its
// bitrate by the frame rate it was configured with to get a budget per frame,
// and spends that much on every frame it is handed — so it has to be fed at the
// rate it was told about, and the two only agree if something in between keeps
// the time. That is this.
//
// A frame arriving early is held rather than sent. Nothing is lost by holding
// it: newer frames replace it in the mailbox while it waits, so what goes in at
// each slot is the latest one either way.
//
// The second job is the opposite problem. An async encoder can hold a pipeline
// of frames, emitting frame N only once frame N+1 arrives, so sparse content —
// a desktop with occasional changes — would leave the last frame sitting inside
// the MFT indefinitely. Re-submitting it on a quiet tick pushes it out, but
// only as many times as the encoder is actually holding: a static screen has to
// stop producing traffic, and an encoder that honours low-latency mode (depth
// 0) should never pay for this at all.
//
// The pacer holds no frame of its own. Which texture is in hand is the caller's
// business; all of this is about when.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;

    // The longest the run loop will sit waiting for a frame that may never
    // come. It bounds how late the caller's control and stats intervals can
    // run on a static screen; on a moving one the frame pacing below expires
    // long before it does.
    static constexpr Clock::duration kQuietPoll = std::chrono::milliseconds(16);

    // Ceiling on the quiet-tick re-submits. The depth it clamps is a property
    // of the encoder, not the content, so anything this large means the reading
    // is wrong and duplicate frames should stop rather than run. It is what
    // bounds the cost of a depth that has gone wrong permanently: one burst of
    // motion, this many duplicate frames, and then quiet again.
    static constexpr int kMaxFlushResubmits = 8;

    // `fps` is what the encoder settled on, which is not always what the
    // display runs at — H.264's levels run out somewhere above 129 fps at
    // 2560x1600, and pacing to the display's rate there would overspend the
    // link by the ratio between the two.
    FramePacer(uint32_t fps, Clock::time_point now)
        : interval_(std::chrono::microseconds(1'000'000 / std::max(1u, fps))), next_submit_(now) {}

    // How long the caller may block waiting for the next frame. With a frame in
    // hand that is however long is left of its slot; with nothing in hand there
    // is no slot to wait for, only the poll — which is also what keeps a static
    // screen from spinning once the re-submit budget is spent.
    Clock::duration wait_for(Clock::time_point now) const {
        if (held_ && next_submit_ > now) return std::min(kQuietPoll, next_submit_ - now);
        return kQuietPoll;
    }

    // A frame arrived and is now the one in hand. `pipeline_depth` is how many
    // earlier frames the encoder is still sitting on, and therefore how many
    // pushes it would take to get this one back out of it — sampled before the
    // frame goes in, since afterwards it counts this one too.
    //
    // Called only by a caller that is actually holding the frame it announces:
    // the next slot is granted on the strength of this, without consulting
    // take_slot's `have_frame` at all.
    void on_frame(int pipeline_depth) {
        resubmits_left_ = std::clamp(pipeline_depth, 0, kMaxFlushResubmits);
        held_ = true;
    }

    // Whether the frame in hand should go to the encoder now. `have_frame` is
    // false before the first one has ever arrived, when there is nothing to
    // re-submit even if the budget would allow it.
    //
    // Consumes the slot, so a true answer must be acted on.
    bool take_slot(Clock::time_point now, bool have_frame) {
        if (now < next_submit_) return false; // this frame's slot has not come round
        if (!held_) {
            // Nothing new since the last submission, so this would be a
            // duplicate sent only to flush the encoder's pipeline.
            if (resubmits_left_ <= 0 || !have_frame) return false;
            --resubmits_left_;
        }
        held_ = false;
        next_submit_ += interval_;
        // Falling a whole frame behind — a stalled encoder, a capture that went
        // away for a moment — is not a reason to then send a burst catching up
        // on frames nobody will ever see.
        if (next_submit_ < now) next_submit_ = now + interval_;
        return true;
    }

private:
    Clock::duration interval_;
    Clock::time_point next_submit_;
    // A frame has arrived that has not been handed over yet. False means the
    // only thing left to send is a duplicate.
    bool held_ = false;
    int resubmits_left_ = 0;
};

} // namespace krg
