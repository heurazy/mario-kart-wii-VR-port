#pragma once

#include <chrono>

namespace mkw::vr::detail {

// Loading a course or compiling shaders can exceed a display frame by seconds.
// Both compositor modes use the same grace period; shutdown still polls at 50 ms.
inline constexpr auto kSubmissionGracePeriod = std::chrono::seconds(10);

template <class Status> struct SubmissionWaitResult {
    Status status = Status::Timeout;
    bool canceled = false;
    bool stalled = false;
};

// Polling for shutdown is not a producer deadline. Keep the published packet
// available between polls so a slow guest can still take ownership of it.
// Cancellation must remain serialized against the bridge's GPU ownership.
template <class Status, class Stop, class Wait, class Expired, class Withdraw, class Cancel>
SubmissionWaitResult<Status> WaitForPublishedSubmission(
    Stop stop, Wait wait, Expired expired, Withdraw withdraw, Cancel cancel) {
    SubmissionWaitResult<Status> result;
    while (!stop() && result.status == Status::Timeout) {
        result.status = wait(50);
        if (result.status != Status::Timeout || !expired()) {
            continue;
        }
        withdraw();
        result.canceled = cancel();
        if (!result.canceled) {
            // Completion can race cancellation. Recheck the callback before
            // treating a bridge-owned image as stalled; never release it here.
            result.status = wait(0);
            result.stalled = result.status == Status::Timeout;
        }
        break;
    }
    return result;
}

} // namespace mkw::vr::detail
