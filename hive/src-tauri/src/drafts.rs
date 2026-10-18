//! Value-moving actions wait here as drafts until the approval window decides.
//!
//! A payment draft holds the funded, unsigned transaction the core built (its
//! coins reserved in the wallet) and the core's own decoding of it. Staking
//! drafts hold what the node is to be asked for. Nothing is signed or sent
//! before approval, so a locked wallet can ask for its passphrase in the
//! approval window. Taking a draft removes it, so one approval can submit it
//! once and only once; page code never sees or supplies the transaction.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

/// One output of the drafted transaction, as decoded by the core.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct OutputLine {
    pub address: String,
    /// B3 as an exact decimal string (9 places), never a float.
    pub amount: String,
    pub change: bool,
    /// The address is this wallet's own, so the amount stays in it.
    pub mine: bool,
}

/// What the approval window shows. The transaction id is known only once
/// it is signed, so the main window gets it with the result.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SendSummary {
    pub chain: String,
    pub wallet: String,
    pub outputs: Vec<OutputLine>,
    pub fee: String,
    /// How many of the wallet's coins it spends.
    pub inputs: usize,
}

/// What a draft asks for. The approval window sees all of it except a
/// payment's funded transaction.
#[derive(Debug, Clone, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum Request {
    /// A payment: the core's decoding and the unsigned transaction.
    Pay {
        summary: SendSummary,
        #[serde(skip)]
        funded: Funded,
    },
    /// Lock `amount` as a stake for this wallet's validator (createstake).
    Stake {
        chain: String,
        wallet: String,
        amount: String,
        /// Blocks after it confirms until it counts.
        activation_depth: i64,
    },
    /// Release stakes back into ordinary B3: exactly the chosen STAKE
    /// outputs, to one new address of this wallet.
    Release {
        summary: SendSummary,
        #[serde(skip)]
        funded: Funded,
    },
    /// Sign a PSBT made elsewhere with this wallet's keys; the result goes
    /// back to the page, nothing is broadcast.
    SignPsbt {
        summary: crate::psbt::Summary,
        #[serde(skip)]
        loaded: crate::psbt::Loaded,
    },
    /// Broadcast a complete PSBT.
    SendPsbt {
        summary: crate::psbt::Summary,
        #[serde(skip)]
        loaded: crate::psbt::Loaded,
    },
    /// Bind, or rotate, this wallet's BLS finality key (bindfinalitykey).
    BindFinality {
        chain: String,
        wallet: String,
        rotate: bool,
    },
}

impl Request {
    pub fn wallet(&self) -> &str {
        match self {
            Request::Pay { summary, .. } | Request::Release { summary, .. } => &summary.wallet,
            Request::SignPsbt { summary, .. } | Request::SendPsbt { summary, .. } => {
                &summary.wallet
            }
            Request::Stake { wallet, .. } | Request::BindFinality { wallet, .. } => wallet,
        }
    }

    /// Approving needs the wallet's keys, so a locked wallet's passphrase.
    /// Broadcasting a PSBT signed elsewhere does not.
    pub fn needs_keys(&self) -> bool {
        !matches!(self, Request::SendPsbt { .. })
    }

    /// The coins a payment holds back; the node picks a stake's coins only
    /// when it is approved.
    pub fn reserved(&self) -> &[Outpoint] {
        match self {
            Request::Pay { funded, .. } | Request::Release { funded, .. } => &funded.inputs,
            _ => &[],
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct DraftView {
    pub id: String,
    pub request: Request,
    /// The wallet is encrypted and locked: approving needs its passphrase.
    pub locked: bool,
}

/// One coin the draft spends.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Outpoint {
    pub txid: String,
    pub vout: u64,
}

/// The funded transaction: signing must not change what it spends.
#[derive(Debug, Clone, PartialEq)]
pub struct Funded {
    /// Base64 PSBT, unsigned.
    pub psbt: String,
    /// Reserved with lockUnspents until the draft is decided.
    pub inputs: Vec<Outpoint>,
    /// Address-book labels (address, label) saved once it is sent. They are
    /// never shown in the approval window, which shows only what the core
    /// decoded.
    pub labels: Vec<(String, String)>,
}

#[derive(Debug, Clone)]
pub struct Draft {
    pub id: String,
    pub request: Request,
}

#[derive(Debug, Default)]
pub struct DraftStore {
    // Ordered by id; ids carry a sequence prefix so the oldest comes first.
    drafts: BTreeMap<String, Draft>,
    next: u64,
}

impl DraftStore {
    pub fn insert(&mut self, request: Request) -> Result<String, getrandom::Error> {
        let mut nonce = [0u8; 8];
        getrandom::fill(&mut nonce)?;
        let nonce: String = nonce.iter().map(|b| format!("{b:02x}")).collect();
        let id = format!("{:08}-{nonce}", self.next);
        self.next += 1;
        self.drafts.insert(
            id.clone(),
            Draft {
                id: id.clone(),
                request,
            },
        );
        Ok(id)
    }

    /// The oldest draft waiting for a decision.
    pub fn oldest(&self) -> Option<Draft> {
        self.drafts.values().next().cloned()
    }

    /// A copy of the draft, which stays waiting.
    pub fn get(&self, id: &str) -> Option<Draft> {
        self.drafts.get(id).cloned()
    }

    /// Removes and returns the draft; a second take of the same id gets None.
    pub fn take(&mut self, id: &str) -> Option<Draft> {
        self.drafts.remove(id)
    }

    /// Removes every waiting draft (the approval window was closed).
    pub fn take_all(&mut self) -> Vec<Draft> {
        std::mem::take(&mut self.drafts).into_values().collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn summary(wallet: &str) -> SendSummary {
        SendSummary {
            chain: "regtest".into(),
            wallet: wallet.into(),
            outputs: vec![OutputLine {
                address: "a".into(),
                amount: "1.000000000".into(),
                change: false,
                mine: false,
            }],
            fee: "0.000010000".into(),
            inputs: 1,
        }
    }

    fn pay(wallet: &str, psbt: &str) -> Request {
        Request::Pay {
            summary: summary(wallet),
            funded: Funded {
                psbt: psbt.into(),
                inputs: vec![Outpoint {
                    txid: "aa".into(),
                    vout: 0,
                }],
                labels: vec![],
            },
        }
    }

    fn psbt(draft: Draft) -> String {
        match draft.request {
            Request::Pay { funded, .. } => funded.psbt,
            other => panic!("not a payment: {other:?}"),
        }
    }

    #[test]
    fn the_approval_window_never_sees_the_transaction() {
        let view = serde_json::to_value(pay("w", "secret-psbt")).unwrap();
        assert_eq!(view["kind"], "pay");
        assert_eq!(view["summary"]["wallet"], "w");
        assert!(!view.to_string().contains("secret-psbt"));
        let stake = Request::Stake {
            chain: "main".into(),
            wallet: "w".into(),
            amount: "10".into(),
            activation_depth: 20,
        };
        assert_eq!(serde_json::to_value(&stake).unwrap()["kind"], "stake");
        assert_eq!(stake.wallet(), "w");
        assert!(stake.reserved().is_empty());
        assert_eq!(pay("w", "p").reserved().len(), 1);
    }

    #[test]
    fn a_draft_can_be_taken_once() {
        let mut store = DraftStore::default();
        let id = store.insert(pay("t1", "beef")).unwrap();
        assert_eq!(store.oldest().unwrap().id, id);
        assert_eq!(psbt(store.get(&id).unwrap()), "beef");
        assert_eq!(psbt(store.take(&id).unwrap()), "beef");
        assert!(store.take(&id).is_none());
        assert!(store.oldest().is_none());
    }

    #[test]
    fn oldest_first_and_unguessable_ids() {
        let mut store = DraftStore::default();
        let first = store.insert(pay("t1", "01")).unwrap();
        let second = store.insert(pay("t2", "02")).unwrap();
        assert_ne!(first, second);
        assert_eq!(store.oldest().unwrap().request.wallet(), "t1");
        assert!(store.take("00000000-0000000000000000").is_none());
        assert_eq!(store.take_all().len(), 2);
        assert!(store.oldest().is_none());
    }
}
