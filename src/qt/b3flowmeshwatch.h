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
    Probing, //!< Trying a watch; ordinary polling continues between probes.
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
    //! An ordinary read the client took once per head instead of an eligible
    //! watch (ticks_catchup): fresh data but no evidence about the watch, so
    //! neither a live reply nor a strike; watch again at once.
    Refreshed,
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
    //! Longest an outstanding probe holds the market poll: its wait plus a
    //! slow link's round trips (a new TLS connection and a snapshot).
    static constexpr int64_t PROBE_HOLD_MS{WATCH_MS + 3000};
    //! Hard floor on the read rate, whatever the outcome: a burst of two,
    //! then one more every 500 ms (at most two watch reads per second to a
    //! remote endpoint; the local engine refills faster, see below).
    static constexpr int ARM_BURST{2};
    static constexpr int64_t ARM_REFILL_MS{500};
    //! The local engine only: one more every 100 ms (at most ten reads a
    //! second). Provisional owner default of 2026-09-30, amending the watch
    //! critique's two-per-second floor for the local engine, which has no
    //! network, endpoint or Admit budget; a remote endpoint keeps 500 ms.
    static constexpr int64_t LOCAL_ARM_REFILL_MS{100};

    Mode mode{Mode::Off};
    //! The last finished read's outcome (for display only).
    Outcome last{Outcome::Quiet};
    bool outstanding{false};
    unsigned failures{0}, not_waited{0};
    int tokens{ARM_BURST};
    int64_t armed_at{0}, last_ok_at{-1}, retry_at{0}, refill_at{0};
    //! Reading the local engine: no network, endpoint or Admit budget. Set
    //! from the client info when enabled; any reply that names an endpoint
    //! clears it (Remote).
    bool local{false};
    //! LOCAL_ARM_REFILL_MS for the local engine, else ARM_REFILL_MS.
    int64_t RefillMs() const { return local ? LOCAL_ARM_REFILL_MS : ARM_REFILL_MS; }

    //! `advertised`: the local engine, or an endpoint advertising live
    //! updates. `local`: the local engine (LOCAL_ARM_REFILL_MS).
    void Enable(int64_t now, bool advertised, bool local = false);
    //! A reply named an endpoint: whatever the client info said, this is
    //! not the local engine, so the remote pacing applies at once.
    void Remote() { local = false; }
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
    //! Polling, Probing, Backoff and Off keep today's 500 ms market poll
    //! (Probing: except while ProbeHoldsPoll).
    bool PollEveryTick() const { return mode != Mode::Live; }
    //! A probe is outstanding, for at most PROBE_HOLD_MS: its market read is
    //! left to it, as when Live. Any ordinary read of the market moves the
    //! client's event cursor, which supersedes the outstanding watch, so a
    //! poll beside it would keep a busy market's probe from ever waiting.
    bool ProbeHoldsPoll(int64_t now) const
    {
        return mode == Mode::Probing && outstanding && now - armed_at < PROBE_HOLD_MS;
    }
    //! The watch supplies the market read: Live, or a probe holding the poll.
    bool SuppliesMarketRead(int64_t now) const { return mode == Mode::Live || ProbeHoldsPoll(now); }
    //! A watch outstanding, or Live without a waited reply, for 15 s.
    bool WatchdogTripped(int64_t now) const;
    //! Backoff after `failures` consecutive failures, before jitter.
    int64_t BackoffMs() const;
    //! Milliseconds until a retry is allowed (0 when allowed now).
    int64_t RetryInMs(int64_t now) const { return retry_at > now ? retry_at - now : 0; }
    //! Milliseconds until MayArm would first return true (0: now), from the
    //! retry time and the token refill; -1 while Off or a read is
    //! outstanding. The panel re-arms then instead of on its next tick; the
    //! pacing rules themselves are unchanged.
    int64_t NextArmIn(int64_t now) const;

private:
    void Refill(int64_t now);
};

} // namespace B3FlowMeshWatch

#endif // BITCOIN_QT_B3FLOWMESHWATCH_H
