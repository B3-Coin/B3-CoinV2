//! Value-moving actions wait here as drafts until the approval window decides.
//!
//! A draft holds the funded, unsigned transaction the core built (its coins
//! reserved in the wallet) and the core's own decoding of it. It is signed
//! only on approval, so a locked wallet can ask for its passphrase in the
//! approval window. Taking a draft removes it, so one approval can submit it
//! once and only once; page code never sees or supplies the transaction.

use std::collections::BTreeMap;

use serde::Serialize;

/// One output of the drafted transaction, as decoded by the core.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct OutputLine {
    pub address: String,
    /// B3 as an exact decimal string (9 places), never a float.
    pub amount: String,
    pub change: bool,
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

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct DraftView {
    pub id: String,
    pub summary: SendSummary,
    /// The wallet is encrypted and locked: approving needs its passphrase.
    pub locked: bool,
}

/// One coin the draft spends.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
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
}

#[derive(Debug, Clone)]
pub struct Draft {
    pub id: String,
    pub summary: SendSummary,
    pub funded: Funded,
}

#[derive(Debug, Default)]
pub struct DraftStore {
    // Ordered by id; ids carry a sequence prefix so the oldest comes first.
    drafts: BTreeMap<String, Draft>,
    next: u64,
}

impl DraftStore {
    pub fn insert(
        &mut self,
        summary: SendSummary,
        funded: Funded,
    ) -> Result<String, getrandom::Error> {
        let mut nonce = [0u8; 8];
        getrandom::fill(&mut nonce)?;
        let nonce: String = nonce.iter().map(|b| format!("{b:02x}")).collect();
        let id = format!("{:08}-{nonce}", self.next);
        self.next += 1;
        self.drafts.insert(
            id.clone(),
            Draft {
                id: id.clone(),
                summary,
                funded,
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
            }],
            fee: "0.000010000".into(),
            inputs: 1,
        }
    }

    fn funded(psbt: &str) -> Funded {
        Funded {
            psbt: psbt.into(),
            inputs: vec![],
        }
    }

    #[test]
    fn a_draft_can_be_taken_once() {
        let mut store = DraftStore::default();
        let id = store.insert(summary("t1"), funded("beef")).unwrap();
        assert_eq!(store.oldest().unwrap().id, id);
        assert_eq!(store.get(&id).unwrap().funded.psbt, "beef");
        assert_eq!(store.take(&id).unwrap().funded.psbt, "beef");
        assert!(store.take(&id).is_none());
        assert!(store.oldest().is_none());
    }

    #[test]
    fn oldest_first_and_unguessable_ids() {
        let mut store = DraftStore::default();
        let first = store.insert(summary("t1"), funded("01")).unwrap();
        let second = store.insert(summary("t2"), funded("02")).unwrap();
        assert_ne!(first, second);
        assert_eq!(store.oldest().unwrap().summary.wallet, "t1");
        assert!(store.take("00000000-0000000000000000").is_none());
        assert_eq!(store.take_all().len(), 2);
        assert!(store.oldest().is_none());
    }
}
