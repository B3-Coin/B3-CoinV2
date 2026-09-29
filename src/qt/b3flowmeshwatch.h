// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_QT_B3FLOWMESHWATCH_H
#define BITCOIN_QT_B3FLOWMESHWATCH_H

#include <cstdint>
#include <string_view>

namespace B3FlowMeshWatch {

/** Live-update scheduling for the trading panel's selected market.
 *
 * Pure local UI policy with an injected millisecond clock: no widgets, RPC,
 * wallet or evidence. It decides only when the panel may send one bounded
 * live-update read (getflowmeshmarketdata watch_ms) and whether the ordinary
 * 500 ms polling loop still runs. Every read it allows is the same verified
 * market read; trading gates never depend on this policy. */
enum class Mode {
    Off,     //!< Not watching: no wallet, hidden, or not yet enabled.
    Probing, //!< Trying a watch; ordinary polling continues meanwhile.
    Live,    //!< Watches arrive: the 500 ms market poll is skipped.
    Polling, //!< The endpoint offers none, or its watch slots are full.
    Backoff, //!< A watch failed; retry after 1, 2, 4 ... 30 s.
};

//! What one finished watch read means for the next one.
enum class Outcome {
    Changed,     //!< The endpoint waited or reported a change, and the head moved.
    Quiet,       //!< The endpoint waited (timeout) or reported a change of the same head.
    NotWaited,   //!< Answered without waiting (not eligible, lane busy, superseded, ...).
    Busy,        //!< The endpoint's watch slots are full.
    Unsupported, //!< The watched endpoint offers no watch (or withdrew it).
    Failed,      //!< The read failed (transport, verification or RPC error).
};

//! One finished read's classification from its watch object.
Outcome Classify(bool rpc_error, std::string_view status, bool supported, bool waited,
                 bool head_moved, uint32_t endpoint_live_ms);

struct Policy {
    static constexpr int64_t WATCH_MS{1500};          //!< Requested watch_ms.
    static constexpr int64_t TICK_MS{500};            //!< The ordinary poll interval.
    static constexpr int64_t WATCHDOG_MS{15000};      //!< Silence before the watchdog trips.
    static constexpr int64_t BACKOFF_FIRST_MS{1000}, BACKOFF_MAX_MS{30000};
    static constexpr int64_t BUSY_RETRY_MS{10000}, UNSUPPORTED_RETRY_MS{60000};
    static constexpr int64_t NOT_WAITED_RETRY_MS{10000};
    static constexpr unsigned NOT_WAITED_LIMIT{3};    //!< Consecutive, then Polling.
    static constexpr int64_t AUX_REFRESH_MS{5000};    //!< Catalog/status/effects/receipt cadence while Live.
    //! Hard floor on the read rate, whatever the outcome: a burst of two,
    //! then one more every 500 ms (at most two watch reads per second).
    static constexpr int ARM_BURST{2};
    static constexpr int64_t ARM_REFILL_MS{500};

    Mode mode{Mode::Off};
    //! The last finished read's outcome (for display only).
    Outcome last{Outcome::Quiet};
    bool outstanding{false};
    unsigned failures{0}, not_waited{0};
    int tokens{ARM_BURST};
    int64_t armed_at{0}, last_ok_at{-1}, retry_at{0}, refill_at{0};

    //! `advertised`: the local engine, or an endpoint advertising live updates.
    void Enable(int64_t now, bool advertised);
    void Disable();
    bool MayArm(int64_t now);
    void Armed(int64_t now);
    //! `jitter` in [-0.2, 0.2] spreads backoff retries.
    void Finished(int64_t now, Outcome outcome, double jitter);
    //! A result the panel discarded (scope changed): no outcome is recorded.
    void Dropped() { outstanding = false; }
    //! The watchdog tripped: ordinary polling resumes at once, and watching
    //! again waits for the outstanding read and a backoff.
    void Watchdog(int64_t now);
    //! Polling, Probing, Backoff and Off keep today's 500 ms market poll.
    bool PollEveryTick() const { return mode != Mode::Live; }
    //! A watch outstanding, or Live without a waited reply, for 15 s.
    bool WatchdogTripped(int64_t now) const;
    //! Backoff after `failures` consecutive failures, before jitter.
    int64_t BackoffMs() const;
    //! Milliseconds until a retry is allowed (0 when allowed now).
    int64_t RetryInMs(int64_t now) const { return retry_at > now ? retry_at - now : 0; }

private:
    void Refill(int64_t now);
};

} // namespace B3FlowMeshWatch

#endif // BITCOIN_QT_B3FLOWMESHWATCH_H
