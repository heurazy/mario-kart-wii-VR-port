#pragma once
#include <cstdint>

namespace mkw::vr::detail {

// GPU image production and compositor ticks have separate lifetimes. Repeated
// display never consumes the cached image or schedules extra simulation steps.
class FrameDelivery {
    struct Image { uint64_t token = 0, tag = 0, session = 0; } pending_, cached_;
public:
    uint64_t PendingToken() const { return pending_.token; }
    bool Start(uint64_t token, uint64_t tag, uint64_t session) {
        if (!token || pending_.token) return false;
        pending_ = {token, tag, session};
        return true;
    }
    bool Complete(uint64_t token) {
        if (!token || token != pending_.token) return false;
        cached_ = pending_;
        pending_ = {};
        return true;
    }
    void Cancel() { pending_ = {}; cached_ = {}; }
    void InvalidateCache() { cached_ = {}; }
    bool CanDisplay(uint64_t tag, uint64_t session) const {
        return cached_.token && cached_.tag == tag && cached_.session == session;
    }
    // Keep the previous, already completed image visible only while a new UI
    // image is being produced in this same OpenXR session. The completed UI
    // image replaces it as soon as Complete() runs.
    bool CanDisplayWhileUiPending(uint64_t ui_tag, uint64_t session) const {
        return cached_.token && cached_.session == session &&
               pending_.token && pending_.tag == ui_tag && pending_.session == session;
    }
};
} // namespace mkw::vr::detail
