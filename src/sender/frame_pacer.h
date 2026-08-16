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
// the MFT indefinitely. Re-submitting it on a quiet tick pushes it out. What
// makes that cheap rather than expensive is *when* the encoder is asked: only
// once it has been sitting on a frame for longer than encoding one takes, and
// only while it still has it. An encoder that honours low-latency mode never
// pays for this at all, and a static screen stops producing traffic.
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

    // Ceiling on the quiet-tick re-submits between one arriving frame and the
    // next. pipeline_depth() is submitted-minus-emitted, and a ProcessOutput
    // that fails leaves emitted permanently behind — so the reading can be not
    // just wrong but wrong forever, and this is what stops that becoming a
    // standing cost: one burst of motion, this many duplicates, then quiet.
    static constexpr int kMaxFlushResubmits = 8;

    // How long the encoder is given to produce a frame before still having it
    // is read as *holding* it rather than working on it. That distinction is
    // the whole of the re-submit decision, and it used to be missing: the depth
    // was sampled when a new capture arrived, which is precisely the moment the
    // previous frame is most likely to still be inside the encoder for the
    // ordinary reason. Encode latency measured 4-8 ms typical and 16 ms at
    // worst on an RTX 4090 at 3440x1440, against a 5.7 ms slot at 175 Hz, so
    // the previous frame usually was still in flight — and 11-14% of everything
    // submitted came out a duplicate, each costing a full frame's bit budget
    // rather than a cheap one, because NVENC under CBR pads to the budget
    // instead of emitting an all-skip frame.
    //
    // Measured, this matters far less than reading the depth here at all: with
    // the depth checked live, a grace of 6 ms already leaves duplicates at 0.6%
    // of submissions against the 11-14% that sampling at arrival produced, and
    // 20 ms and up leave none at all. So this is not the load-bearing part —
    // it is insurance for hardware where the depth *is* still nonzero at the
    // tick, which is exactly where mistaking one for the other would be
    // expensive. Set at twice the worst encode observed (16 ms), and under the
    // 33 ms between frames of 30 fps content, so it can still fire before the
    // next real frame arrives to flush the last one out on its own.
    static constexpr Clock::duration kFlushGrace = std::chrono::milliseconds(30);

    // `fps` is what the encoder settled on, which is not always what the
    // display runs at — H.264's levels run out somewhere above 129 fps at
    // 2560x1600, and pacing to the display's rate there would overspend the
    // link by the ratio between the two.
    FramePacer(uint32_t fps, Clock::time_point now)
        : interval_(std::chrono::microseconds(1'000'000 / std::max(1u, fps))),
          next_submit_(now),
          last_submit_(now) {}

    // How long the caller may block waiting for the next frame. With a frame in
    // hand that is however long is left of its slot; with nothing in hand there
    // is no slot to wait for, only the poll — which is also what keeps a static
    // screen from spinning once the re-submit budget is spent.
    Clock::duration wait_for(Clock::time_point now) const {
        if (held_ && next_submit_ > now) return std::min(kQuietPoll, next_submit_ - now);
        return kQuietPoll;
    }

    // A frame arrived and is now the one in hand.
    //
    // Called only by a caller that is actually holding the frame it announces:
    // the next slot is granted on the strength of this, without consulting
    // take_slot's `have_frame` at all. Fresh content also restores the flush
    // budget, which is per quiet stretch rather than per session.
    void on_frame() {
        resubmits_left_ = kMaxFlushResubmits;
        held_ = true;
    }

    // Whether the frame in hand should go to the encoder now. `have_frame` is
    // false before the first one has ever arrived, when there is nothing to
    // re-submit even if the budget would allow it. `pipeline_depth` is how many
    // frames the encoder has not given back *at this moment* — read here rather
    // than remembered from when the frame arrived, because that is the whole
    // difference between an encoder holding a frame and one encoding it.
    //
    // Consumes the slot, so a true answer must be acted on.
    bool take_slot(Clock::time_point now, bool have_frame, int pipeline_depth) {
        if (now < next_submit_) return false; // this frame's slot has not come round
        if (!held_) {
            // Nothing new since the last submission, so this would be a
            // duplicate sent only to flush the encoder's pipeline.
            if (resubmits_left_ <= 0 || !have_frame) return false;
            // The encoder has given back everything it was handed, so there is
            // nothing in there to shake loose.
            if (pipeline_depth <= 0) return false;
            // It does still have something — but not yet for long enough to
            // say it is holding rather than working. See kFlushGrace.
            if (now - last_submit_ < kFlushGrace) return false;
            --resubmits_left_;
        }
        held_ = false;
        last_submit_ = now;
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
    // When the encoder was last handed anything, which is what kFlushGrace is
    // measured from — a re-submit is itself a submission, so a second one has
    // to wait out the grace again rather than following on the next slot.
    Clock::time_point last_submit_;
    // A frame has arrived that has not been handed over yet. False means the
    // only thing left to send is a duplicate.
    bool held_ = false;
    int resubmits_left_ = 0;
};

} // namespace krg
