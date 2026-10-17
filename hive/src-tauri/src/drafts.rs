//! Value-moving actions wait here as drafts until the approval window decides.
//!
//! A draft holds the exact signed transaction the core built and the core's own
//! decoding of it. Taking a draft removes it, so one approval can submit it
//! once and only once; page code never sees or supplies the bytes.

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

/// What the approval window shows.
#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct SendSummary {
    pub chain: String,
    pub wallet: String,
    pub txid: String,
    pub outputs: Vec<OutputLine>,
    pub fee: String,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct DraftView {
    pub id: String,
    pub summary: SendSummary,
}

#[derive(Debug)]
pub struct Draft {
    pub id: String,
    pub summary: SendSummary,
    /// The fully signed transaction, submitted byte for byte on approval.
    pub hex: String,
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
        hex: String,
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
                hex,
            },
        );
        Ok(id)
    }

    /// The oldest draft waiting for a decision.
    pub fn oldest(&self) -> Option<DraftView> {
        self.drafts.values().next().map(|d| DraftView {
            id: d.id.clone(),
            summary: d.summary.clone(),
        })
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

    fn summary(txid: &str) -> SendSummary {
        SendSummary {
            chain: "regtest".into(),
            wallet: "w".into(),
            txid: txid.into(),
            outputs: vec![OutputLine {
                address: "a".into(),
                amount: "1.000000000".into(),
                change: false,
            }],
            fee: "0.000010000".into(),
        }
    }

    #[test]
    fn a_draft_can_be_taken_once() {
        let mut store = DraftStore::default();
        let id = store.insert(summary("t1"), "beef".into()).unwrap();
        assert_eq!(store.oldest().unwrap().id, id);
        assert_eq!(store.take(&id).unwrap().hex, "beef");
        assert!(store.take(&id).is_none());
        assert!(store.oldest().is_none());
    }

    #[test]
    fn oldest_first_and_unguessable_ids() {
        let mut store = DraftStore::default();
        let first = store.insert(summary("t1"), "01".into()).unwrap();
        let second = store.insert(summary("t2"), "02".into()).unwrap();
        assert_ne!(first, second);
        assert_eq!(store.oldest().unwrap().summary.txid, "t1");
        assert!(store.take("00000000-0000000000000000").is_none());
        assert_eq!(store.take_all().len(), 2);
        assert!(store.oldest().is_none());
    }
}
