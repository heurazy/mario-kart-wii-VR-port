#include "vr/submission_wait.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

enum class Status { Timeout, Success, Failed };
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Bridge {
    int now = 0;
    int completion = 67;
    int withdrawals = 0;
    int cancellations = 0;
    bool published = true;
    bool owned = false;
    bool shutdown = false;
    bool raceAtCancel = false;
    bool failure = false;
    int stopAt = 20000;

    auto run() {
        return mkw::vr::detail::WaitForPublishedSubmission<Status>(
            [&] { return shutdown; },
            [&](unsigned ms) {
                const int end = now + static_cast<int>(ms);
                if ((published || owned) && completion <= end) {
                    now = std::max(now, completion);
                    owned = true;
                    return failure ? Status::Failed : Status::Success;
                }
                now = end;
                if (now >= stopAt) shutdown = true;
                return Status::Timeout;
            },
            [&] { return std::chrono::milliseconds(now) >= mkw::vr::detail::kSubmissionGracePeriod; },
            [&] { ++withdrawals; published = false; },
            [&] {
                ++cancellations;
                if (raceAtCancel) { owned = true; completion = now; }
                return !owned;
            });
    }
};

int main() {
    try {
        // 15 FPS (67 ms) and even a 200 ms guest frame must survive the
        // 50 ms shutdown polls. Previously the first poll removed the packet.
        for (int frameMs : {17, 34, 50, 51, 67, 100, 200, 250, 300, 2000, 9000}) {
            Bridge bridge;
            bridge.completion = frameMs;
            const auto result = bridge.run();
            require(result.status == Status::Success, "slow producer lost its published packet");
            require(bridge.withdrawals == 0 && bridge.cancellations == 0, "successful frame was canceled");
        }
        Bridge paused;
        paused.completion = 15000;
        const auto timeout = paused.run();
        require(timeout.canceled && !timeout.stalled && paused.now == 10000, "idle frame deadline broken");
        require(paused.withdrawals == 1 && paused.cancellations == 1, "cancellation must happen once");

        Bridge owned;
        owned.completion = 15000;
        owned.owned = true;
        const auto stall = owned.run();
        require(stall.stalled && !stall.canceled, "GPU-owned frame must not be released as canceled");

        Bridge raced;
        raced.completion = 15000;
        raced.raceAtCancel = true;
        const auto race = raced.run();
        require(race.status == Status::Success && !race.stalled && !race.canceled, "completion race lost");

        Bridge stopping;
        stopping.completion = 10000;
        stopping.stopAt = 50;
        stopping.run();
        require(stopping.now == 50 && stopping.cancellations == 0, "shutdown did not exit promptly");

        Bridge failed;
        failed.failure = true;
        const auto failure = failed.run();
        require(failure.status == Status::Failed && failed.cancellations == 0, "GPU failure was hidden");
        std::cout << "Submission timing regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
