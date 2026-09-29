// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshwatch.h>

#include <algorithm>
#include <cmath>

namespace B3FlowMeshWatch {

Outcome Classify(const bool rpc_error, const std::string_view status, const bool supported, const bool waited,
                 const bool head_moved, const uint32_t endpoint_live_ms)
{
    if (rpc_error) return Outcome::Failed;
    if (status == "ticks_catchup") return Outcome::Refreshed;
    if (status == "unsupported") return Outcome::Unsupported;
    if (status == "busy") return Outcome::Busy;
    // Live mode is keyed on the endpoint the watch was for: one that
    // advertises nothing is polling territory, not a transient refusal.
    if (!supported && status == "not_eligible" && endpoint_live_ms == 0) return Outcome::Unsupported;
    if (supported && waited) return head_moved ? Outcome::Changed : Outcome::Quiet;
    return Outcome::NotWaited;
}

void Policy::Enable(const int64_t now, const bool advertised)
{
    mode = advertised ? Mode::Probing : Mode::Polling;
    failures = 0;
    not_waited = 0;
    retry_at = advertised ? now : now + UNSUPPORTED_RETRY_MS;
    last_ok_at = -1;
    tokens = ARM_BURST;
    refill_at = now;
}

void Policy::Disable()
{
    mode = Mode::Off;
    failures = 0;
    not_waited = 0;
    last_ok_at = -1;
}

void Policy::Refill(const int64_t now)
{
    if (tokens >= ARM_BURST) {
        refill_at = now;
        return;
    }
    const int64_t earned{(now - refill_at) / ARM_REFILL_MS};
    if (earned <= 0) return;
    tokens = static_cast<int>(std::min<int64_t>(ARM_BURST, tokens + earned));
    refill_at = tokens >= ARM_BURST ? now : refill_at + earned * ARM_REFILL_MS;
}

bool Policy::MayArm(const int64_t now)
{
    if (mode == Mode::Off || outstanding || now < retry_at) return false;
    // A retry that is due probes again; ordinary polling continues meanwhile.
    if (mode == Mode::Polling || mode == Mode::Backoff) mode = Mode::Probing;
    Refill(now);
    return tokens > 0;
}

void Policy::Armed(const int64_t now)
{
    Refill(now);
    if (tokens > 0) --tokens;
    outstanding = true;
    armed_at = now;
}

void Policy::Finished(const int64_t now, const Outcome outcome, const double jitter)
{
    outstanding = false;
    if (mode == Mode::Off) return;
    last = outcome;
    switch (outcome) {
    case Outcome::Changed:
    case Outcome::Quiet:
        mode = Mode::Live;
        failures = 0;
        not_waited = 0;
        last_ok_at = now;
        retry_at = now;
        return;
    case Outcome::NotWaited:
        // Answered at once: never again at once, and a few in a row mean
        // this endpoint is not watching now.
        if (++not_waited >= NOT_WAITED_LIMIT) {
            mode = Mode::Polling;
            not_waited = 0;
            retry_at = now + NOT_WAITED_RETRY_MS;
        } else {
            retry_at = now + TICK_MS;
        }
        return;
    case Outcome::Busy:
        mode = Mode::Polling;
        not_waited = 0;
        retry_at = now + BUSY_RETRY_MS;
        return;
    case Outcome::Unsupported:
        mode = Mode::Polling;
        not_waited = 0;
        retry_at = now + UNSUPPORTED_RETRY_MS;
        return;
    case Outcome::Refreshed:
        // The mode, strikes and watchdog are unchanged; the next watch at
        // this head is sent (still at most two reads per second).
        retry_at = now;
        return;
    case Outcome::Failed: {
        failures = std::min(failures + 1, 16U);
        not_waited = 0;
        mode = Mode::Backoff;
        const double spread{1.0 + std::clamp(jitter, -0.2, 0.2)};
        retry_at = now + static_cast<int64_t>(std::llround(static_cast<double>(BackoffMs()) * spread));
        return;
    }
    }
}

void Policy::Watchdog(const int64_t now)
{
    if (mode == Mode::Off || mode == Mode::Backoff) return;
    failures = std::min(failures + 1, 16U);
    mode = Mode::Backoff;
    retry_at = std::max(retry_at, now + BackoffMs());
    last_ok_at = -1;
}

bool Policy::WatchdogTripped(const int64_t now) const
{
    return (outstanding && now - armed_at >= WATCHDOG_MS) ||
           (mode == Mode::Live && last_ok_at >= 0 && now - last_ok_at >= WATCHDOG_MS);
}

int64_t Policy::BackoffMs() const
{
    if (failures == 0) return 0;
    if (failures > 5) return BACKOFF_MAX_MS;
    return std::min(BACKOFF_MAX_MS, BACKOFF_FIRST_MS << (failures - 1));
}

} // namespace B3FlowMeshWatch
