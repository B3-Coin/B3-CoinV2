//! FlowMesh V2 tagged hashes (src/flowmesh2/core/hashes.h): BIP340 tagged
//! hashes, SHA256(SHA256(t) || SHA256(t) || m), whose tag text is "B3/FM2/"
//! followed by a registry name. Only the tags a client needs are listed.

use sha2::{Digest, Sha256};

use super::bytes::H256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tag {
    Domain,
    Account,
    Action,
    Market,
    Slot,
    Commit,
    Cert,
    Outcome,
    SeatSet,
    Profile,
    State,
    StateAccounts,
    MerkleLeaf,
    MerkleNode,
    MerkleEmpty,
    MerkleRoot,
}

impl Tag {
    pub fn text(self) -> &'static str {
        match self {
            Tag::Domain => "B3/FM2/DOMAIN",
            Tag::Account => "B3/FM2/ACCOUNT",
            Tag::Action => "B3/FM2/ACTION",
            Tag::Market => "B3/FM2/MARKET",
            Tag::Slot => "B3/FM2/SLOT",
            Tag::Commit => "B3/FM2/COMMIT",
            Tag::Cert => "B3/FM2/CERT",
            Tag::Outcome => "B3/FM2/OUTCOME",
            Tag::SeatSet => "B3/FM2/SEATSET",
            Tag::Profile => "B3/FM2/PROFILE",
            Tag::State => "B3/FM2/STATE",
            Tag::StateAccounts => "B3/FM2/STATE_ACCOUNTS",
            Tag::MerkleLeaf => "B3/FM2/MERKLE_LEAF",
            Tag::MerkleNode => "B3/FM2/MERKLE_NODE",
            Tag::MerkleEmpty => "B3/FM2/MERKLE_EMPTY",
            Tag::MerkleRoot => "B3/FM2/MERKLE_ROOT",
        }
    }
}

/// The tagged hash of the concatenation of `parts`.
pub fn tagged(tag: Tag, parts: &[&[u8]]) -> H256 {
    let tag_hash = Sha256::digest(tag.text().as_bytes());
    let mut hasher = Sha256::new();
    hasher.update(tag_hash);
    hasher.update(tag_hash);
    for part in parts {
        hasher.update(part);
    }
    hasher.finalize().into()
}

/// SHA-256 of `bytes`.
pub fn sha256(bytes: &[u8]) -> H256 {
    Sha256::digest(bytes).into()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fm2::bytes::hex;

    #[test]
    fn a_tagged_hash_is_the_bip340_construction() {
        // BIP340's own tag, checked against the specification's formula.
        let tag = Sha256::digest(b"B3/FM2/ACCOUNT");
        let mut expected = Sha256::new();
        expected.update(tag);
        expected.update(tag);
        expected.update(b"abc");
        let expected: H256 = expected.finalize().into();
        assert_eq!(tagged(Tag::Account, &[b"a", b"bc"]), expected);
        assert_ne!(
            tagged(Tag::Account, &[b"abc"]),
            tagged(Tag::Action, &[b"abc"])
        );
    }

    #[test]
    fn sha256_matches_the_standard_vector() {
        assert_eq!(
            hex(&sha256(b"abc")),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }
}
