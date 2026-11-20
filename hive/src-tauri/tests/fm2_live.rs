//! The FlowMesh V2 client against a real regtest cluster of `fm2d` seats.
//!
//!   HIVE_FM2_CLUSTER=<dir> cargo test --test fm2_live -- --ignored --nocapture
//!
//! `<dir>` holds the cluster's `genesis.json` and `pins.json`. The test makes
//! a new account, writes its id to `<dir>/hive_account.txt` and waits until
//! the faucet funded it with rUSD (`fm2-devchain ... deposit <id> rUSD
//! 1000000`). The market B3/rUSD must hold a resting sell of at least two
//! lots from another account.

use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

use b3_hive::fm2::bytes::id_hex;
use b3_hive::fm2::client::{Channel, Progress, Session, StreamUpdate};
use b3_hive::fm2::crypto::schnorr_verify;
use b3_hive::fm2::http::{request, SeatEndpoint};
use b3_hive::fm2::reply::StreamBody;
use b3_hive::fm2::types::{Payload, SIDE_BUY, TIF_IOC, TIF_POST_ONLY};
use b3_hive::fm2::Error;

fn cluster() -> PathBuf {
    PathBuf::from(
        std::env::var_os("HIVE_FM2_CLUSTER").expect("HIVE_FM2_CLUSTER names the cluster folder"),
    )
}

fn open(dir: &std::path::Path) -> Session {
    let genesis = std::fs::read_to_string(cluster().join("genesis.json")).unwrap();
    let pins = std::fs::read_to_string(cluster().join("pins.json")).unwrap();
    Session::open(&genesis, &pins, dir).unwrap()
}

/// Polls every seat until the action has a certified end.
fn end_of(session: &Session, label: &str) -> Progress {
    let deadline = Instant::now() + Duration::from_secs(60);
    loop {
        let progress = session.settle(label).unwrap();
        match progress {
            Progress::Waiting { .. } if Instant::now() < deadline => {
                std::thread::sleep(Duration::from_millis(300))
            }
            other => return other,
        }
    }
}

/// The account that sold in the fills of an outcome: the other account
/// whose available balance rose.
fn seller_of(session: &Session, label: &str) -> [u8; 32] {
    let Progress::Included { outcome, .. } = session.settle(label).unwrap() else {
        panic!("not included")
    };
    outcome
        .deltas
        .iter()
        .find(|delta| delta.available_delta > 0 && delta.account != outcome.account)
        .unwrap()
        .account
}

#[test]
#[ignore = "needs a running fm2d cluster"]
fn a_hive_account_trades_on_a_real_cluster() {
    let dir = std::env::temp_dir().join(format!("b3hive-fm2-live-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    let session = open(&dir);

    // Every seat answers its health over the pinned connection.
    let health = session.health();
    assert_eq!(health.len(), session.seats.len());
    for (seat, answer) in &health {
        let answer = answer
            .as_ref()
            .unwrap_or_else(|e| panic!("seat {seat}: {e}"));
        assert!(!answer.fenced && answer.published_height.is_some());
    }

    // A seat presenting another key than the pinned one gets no request.
    let wrong = SeatEndpoint {
        pin: [7; 32],
        ..session.seats[0].clone()
    };
    let refused = request(&wrong, "GET", "/v2/health", None, Duration::from_secs(5));
    assert_eq!(
        refused.err(),
        Some(Error::Verify(
            "the seat's certificate does not carry the pinned key"
        ))
    );

    // A new account: unknown to every seat (a claim, with verified heads).
    let account = session.create_key().unwrap();
    for state in session.state(&account) {
        assert!(state.error.is_none(), "{:?}", state.error);
        assert!(state.height.is_some() && state.leaf.is_none());
    }
    std::fs::write(cluster().join("hive_account.txt"), id_hex(&account)).unwrap();
    println!("waiting for the faucet to fund {}", id_hex(&account));
    let quote = session
        .genesis
        .assets
        .iter()
        .find(|a| a.name == "rUSD")
        .unwrap()
        .id;
    let deadline = Instant::now() + Duration::from_secs(120);
    let funded = loop {
        let states = session.state(&account);
        if let Some(leaf) = states.iter().find_map(|state| state.leaf.clone()) {
            break leaf;
        }
        assert!(
            Instant::now() < deadline,
            "the faucet never funded the account"
        );
        std::thread::sleep(Duration::from_millis(500));
    };
    let rusd = funded
        .balances
        .iter()
        .find(|b| b.asset == quote)
        .unwrap()
        .available;
    assert!(rusd >= 1_000, "funded with {rusd} atoms of rUSD");

    // The slot stream carries envelopes that verify, at rising heights.
    let stop = AtomicBool::new(false);
    let mut heights = Vec::new();
    session.follow(Channel::Slots, &stop, |update| {
        if let StreamUpdate::Event {
            verified, event, ..
        } = update
        {
            assert!(verified);
            let StreamBody::Slot { envelope, .. } = &event.body else {
                panic!("a slot event")
            };
            heights.push(envelope.header.height);
            if heights.len() == 3 {
                stop.store(true, Ordering::Relaxed);
            }
        }
    });
    assert!(
        heights.windows(2).all(|pair| pair[0] < pair[1]),
        "{heights:?}"
    );

    // The book shows the other account's resting sell.
    let market = session
        .genesis
        .markets
        .iter()
        .find(|m| m.name() == "B3/rUSD")
        .unwrap()
        .descriptor
        .clone();
    let stop = AtomicBool::new(false);
    let mut ladder = Vec::new();
    session.follow(Channel::Book(market.id()), &stop, |update| {
        if let StreamUpdate::Event {
            verified, event, ..
        } = update
        {
            assert!(!verified, "a book event carries no proof");
            let StreamBody::Book { asks, .. } = &event.body else {
                panic!("a book event")
            };
            ladder = asks.clone();
            stop.store(true, Ordering::Relaxed);
        }
    });
    // The lowest price at which two lots are offered.
    let mut offered = 0;
    let limit = ladder
        .iter()
        .find(|level| {
            offered += level.lots;
            offered >= 2
        })
        .expect("resting sells of two lots in B3/rUSD")
        .price_ticks;

    // Buy two lots up to that price: signed here, admitted and included there.
    let buy = Payload::Place {
        market: market.id(),
        side: SIDE_BUY,
        tif: TIF_IOC,
        price_ticks: limit,
        lots: 2,
    };
    let submitted = session.submit(&buy, 64).unwrap();
    println!("{} admissions: {:?}", submitted.label, submitted.admissions);
    assert_eq!(submitted.nonce, 1);
    assert!(
        submitted.admissions.iter().all(|a| a.accepted()),
        "{:?}",
        submitted.admissions
    );
    let entry = session.entry(&submitted.label).unwrap();
    assert!(schnorr_verify(
        &entry.action.signer,
        &entry.action.id(),
        &entry.action.sig
    ));
    let Progress::Included {
        outcome, height, ..
    } = end_of(&session, &submitted.label)
    else {
        panic!("the buy was not included")
    };
    assert_eq!(outcome.code, 3, "FILLED: the whole order matched");
    assert_eq!(outcome.remaining_lots, 0);
    assert_eq!(outcome.fills.iter().map(|fill| fill.lots).sum::<u64>(), 2);
    assert_eq!(outcome.fills[0].taker, submitted.action_id);
    println!("included in slot {height}: {} fill(s)", outcome.fills.len());

    // The verified leaf shows the B3 bought and the rUSD paid.
    let paid: i64 = outcome.fills.iter().map(|fill| fill.quote_atoms).sum();
    assert!(paid <= market.quote_atoms(2, limit).unwrap());
    let leaf = session
        .state(&account)
        .into_iter()
        .find_map(|state| state.leaf)
        .unwrap();
    let balance = |asset| {
        leaf.balances
            .iter()
            .find(|b| b.asset == asset)
            .map_or(0, |b| b.available)
    };
    assert_eq!(balance(market.base), market.base_atoms(2).unwrap());
    assert_eq!(balance(quote), rusd - paid);
    assert_eq!(leaf.used_nonces, [1]);

    // A post-only buy far below the market rests; cancelling it is certified.
    let rest = Payload::Place {
        market: market.id(),
        side: SIDE_BUY,
        tif: TIF_POST_ONLY,
        price_ticks: 1,
        lots: 3,
    };
    let resting = session.submit(&rest, 64).unwrap();
    assert_eq!(resting.nonce, 2);
    let Progress::Included { outcome, .. } = end_of(&session, &resting.label) else {
        panic!("not included")
    };
    assert_eq!((outcome.code, outcome.remaining_lots), (2, 3), "RESTING");
    let cancel = session
        .submit(
            &Payload::Cancel {
                order: resting.action_id,
            },
            64,
        )
        .unwrap();
    let Progress::Included { outcome, .. } = end_of(&session, &cancel.label) else {
        panic!("not included")
    };
    assert_eq!(outcome.code, 6, "CANCELLED");

    // A transfer to the seller's account, and its certified outcome.
    let seller = seller_of(&session, &submitted.label);
    let transfer = session
        .submit(
            &Payload::Transfer {
                to: seller,
                asset: quote,
                amount: 5,
            },
            64,
        )
        .unwrap();
    let Progress::Included { outcome, .. } = end_of(&session, &transfer.label) else {
        panic!("not included")
    };
    assert_eq!(outcome.code, 7, "TRANSFERRED");

    // An action that was signed but never sent, with a window that is over:
    // the seats prove from the account's leaf that no slot can include it.
    let unsent = session.sign_and_store(&Payload::CancelAll, 1).unwrap();
    assert!(unsent.admissions.is_empty());
    let Progress::Expired { height, .. } = end_of(&session, &unsent.label) else {
        panic!("not expired")
    };
    assert!(height > unsent.ref_slot + 1);

    // Posting stored bytes again changes nothing: every seat answers DUPLICATE.
    let again = session.resend(&submitted.label).unwrap();
    assert!(again.iter().all(|a| a.code == "DUPLICATE"), "{again:?}");

    // A reopened folder holds the same key, the outbox and its certified ends.
    drop(session);
    let session = open(&dir);
    assert_eq!(session.account(), Some(account));
    let entries = session.entries();
    assert_eq!(entries.len(), 5);
    assert!(entries.iter().all(|entry| entry.result.is_some()));
    // The next nonce continues after everything this outbox signed.
    let last = session.submit(&Payload::CancelAll, 64).unwrap();
    assert_eq!(last.nonce, 6);
    assert!(matches!(
        end_of(&session, &last.label),
        Progress::Included { .. }
    ));

    drop(session);
    std::fs::remove_dir_all(&dir).unwrap();
}
