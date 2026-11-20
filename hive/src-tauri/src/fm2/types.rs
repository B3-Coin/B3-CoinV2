//! The FlowMesh V2 objects a client signs or checks, with their exact
//! encodings (src/flowmesh2/core/*.h). Every decoder is exact and capped:
//! trailing bytes, a count above its cap and a non-canonical value are
//! errors, so the bytes a decoder accepts are the bytes the node hashed.

use super::bytes::{compact_len, put_compact, Reader, H256};
use super::hash::{tagged, Tag};
use super::Error;

/// Largest amount of any asset, in atoms (core/amount.h).
pub const MAX_ATOMS: i64 = 1 << 62;

pub fn in_atom_range(atoms: i64) -> bool {
    (0..=MAX_ATOMS).contains(&atoms)
}

// ------------------------------------------------------------------ profile

/// The engine profile (core/params.h). B3 Hive knows one: the provisional
/// regtest profile the regtest daemon runs, `fm2-dex1-regtest-provisional/1`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Profile {
    pub profile_id: &'static str,
    pub fm_version: u32,
    pub chain: &'static str,
    pub matching: u8,
    pub fee_ppm: u32,
    pub fee_incidence: u8,
    pub seat_share_percent: u32,
    pub max_actions_per_slot: u32,
    pub max_action_payload_bytes: u32,
    pub max_fills_per_order: u32,
    pub max_fills_per_slot: u32,
    pub max_changed_leaves_per_slot: u32,
    pub max_open_orders_per_account: u32,
    pub price_band_enabled: bool,
    pub price_band_percent: u32,
    pub nonce_window: u32,
    pub max_delay_slots: u32,
    pub futures_enabled: bool,
    pub signer_mode: u8,
    pub pool_capacity: u32,
    pub outcome_index_slots: u32,
}

/// The FlowMesh V2 protocol version bound into the domain.
pub const FM_VERSION: u32 = 1;

impl Profile {
    /// `DevRegtestProfile(ChainType::REGTEST)`.
    pub fn dev_regtest() -> Self {
        Self {
            profile_id: "fm2-dex1-regtest-provisional/1",
            fm_version: FM_VERSION,
            chain: "regtest",
            matching: 1,
            fee_ppm: 100,
            fee_incidence: 1,
            seat_share_percent: 80,
            max_actions_per_slot: 1_024,
            max_action_payload_bytes: 4_096,
            max_fills_per_order: 1_024,
            max_fills_per_slot: 16_384,
            max_changed_leaves_per_slot: 8_192,
            max_open_orders_per_account: 1_000,
            price_band_enabled: false,
            price_band_percent: 50,
            nonce_window: 100,
            max_delay_slots: 1_024,
            futures_enabled: false,
            signer_mode: 1,
            pool_capacity: 4_096,
            outcome_index_slots: 1_024,
        }
    }

    /// `EncodeProfile`: encoding version 1, every field in declaration order.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = vec![1u8];
        put_compact(&mut out, self.profile_id.len() as u64);
        out.extend_from_slice(self.profile_id.as_bytes());
        out.extend_from_slice(&self.fm_version.to_le_bytes());
        put_compact(&mut out, self.chain.len() as u64);
        out.extend_from_slice(self.chain.as_bytes());
        out.push(self.matching);
        out.extend_from_slice(&self.fee_ppm.to_le_bytes());
        out.push(self.fee_incidence);
        for value in [
            self.seat_share_percent,
            self.max_actions_per_slot,
            self.max_action_payload_bytes,
            self.max_fills_per_order,
            self.max_fills_per_slot,
            self.max_changed_leaves_per_slot,
            self.max_open_orders_per_account,
        ] {
            out.extend_from_slice(&value.to_le_bytes());
        }
        out.push(u8::from(self.price_band_enabled));
        out.extend_from_slice(&self.price_band_percent.to_le_bytes());
        out.extend_from_slice(&self.nonce_window.to_le_bytes());
        out.extend_from_slice(&self.max_delay_slots.to_le_bytes());
        out.push(u8::from(self.futures_enabled));
        out.push(self.signer_mode);
        out.extend_from_slice(&self.pool_capacity.to_le_bytes());
        out.extend_from_slice(&self.outcome_index_slots.to_le_bytes());
        out
    }

    /// `ProfileHash`.
    pub fn hash(&self) -> H256 {
        tagged(Tag::Profile, &[&self.encode()])
    }
}

/// `DomainHash`: the chain's domain, the FlowMesh version and the network
/// magic. An action signed for one network is not an action of another.
pub fn domain_hash(modern_chain_domain: &H256, magic: &[u8; 4]) -> H256 {
    tagged(
        Tag::Domain,
        &[modern_chain_domain, &FM_VERSION.to_le_bytes(), magic],
    )
}

/// `AccountId::FromKey`: the account of an x-only BIP340 key.
pub fn account_of(signer: &[u8; 32]) -> H256 {
    tagged(Tag::Account, &[signer])
}

// ------------------------------------------------------------------ markets

/// A market of the genesis document (core/market.h).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Market {
    pub base: H256,
    pub quote: H256,
    pub base_atoms_per_lot: u64,
    pub quote_atoms_per_tick: u64,
}

impl Market {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(80);
        out.extend_from_slice(&self.base);
        out.extend_from_slice(&self.quote);
        out.extend_from_slice(&self.base_atoms_per_lot.to_le_bytes());
        out.extend_from_slice(&self.quote_atoms_per_tick.to_le_bytes());
        out
    }

    /// `MarketDescriptor::Id`.
    pub fn id(&self) -> H256 {
        tagged(Tag::Market, &[&self.encode()])
    }

    /// `ValidateMarket`.
    pub fn valid(&self) -> bool {
        let max = MAX_ATOMS as u64;
        self.base != self.quote
            && (1..=max).contains(&self.base_atoms_per_lot)
            && (1..=max).contains(&self.quote_atoms_per_tick)
    }

    /// `BaseAtoms`: lots in atoms of the base asset; `None` above MAX_ATOMS.
    pub fn base_atoms(&self, lots: u64) -> Option<i64> {
        let atoms = u128::from(lots) * u128::from(self.base_atoms_per_lot);
        (self.valid() && atoms <= MAX_ATOMS as u128).then_some(atoms as i64)
    }

    /// `QuoteAtoms`: lots at a price in atoms of the quote asset; `None`
    /// when the notional or the result is above MAX_ATOMS.
    pub fn quote_atoms(&self, lots: u64, price_ticks: u64) -> Option<i64> {
        if !self.valid() {
            return None;
        }
        let notional = u128::from(lots) * u128::from(price_ticks);
        if notional > MAX_ATOMS as u128 {
            return None;
        }
        let atoms = notional * u128::from(self.quote_atoms_per_tick);
        (atoms <= MAX_ATOMS as u128).then_some(atoms as i64)
    }
}

// ------------------------------------------------------------------ actions

pub const ACTION_VERSION: u8 = 1;
/// version + domain + signer + nonce + ref_slot + max_delay + kind.
const ACTION_FIXED_PREFIX: usize = 1 + 32 + 32 + 8 + 8 + 2 + 1;

pub const KIND_PLACE: u8 = 1;
pub const KIND_CANCEL: u8 = 2;
pub const KIND_CANCEL_ALL: u8 = 3;
pub const KIND_WITHDRAW: u8 = 4;
pub const KIND_TRANSFER: u8 = 5;

pub const SIDE_BUY: u8 = 1;
pub const SIDE_SELL: u8 = 2;

pub const TIF_GTC: u8 = 1;
pub const TIF_IOC: u8 = 2;
pub const TIF_POST_ONLY: u8 = 3;

/// What an action asks for. The kinds B3 Hive signs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Payload {
    Place {
        market: H256,
        side: u8,
        tif: u8,
        price_ticks: u64,
        lots: u64,
    },
    Cancel {
        order: H256,
    },
    CancelAll,
    Transfer {
        to: H256,
        asset: H256,
        amount: i64,
    },
}

impl Payload {
    pub fn kind(&self) -> u8 {
        match self {
            Payload::Place { .. } => KIND_PLACE,
            Payload::Cancel { .. } => KIND_CANCEL,
            Payload::CancelAll => KIND_CANCEL_ALL,
            Payload::Transfer { .. } => KIND_TRANSFER,
        }
    }

    /// The canonical payload bytes; an error for a value the node's decoder
    /// would refuse.
    pub fn encode(&self) -> Result<Vec<u8>, Error> {
        let mut out = Vec::new();
        match self {
            Payload::Place {
                market,
                side,
                tif,
                price_ticks,
                lots,
            } => {
                if !matches!(*side, SIDE_BUY | SIDE_SELL) {
                    return Err(Error::Invalid("unknown order side".into()));
                }
                if !matches!(*tif, TIF_GTC | TIF_IOC | TIF_POST_ONLY) {
                    return Err(Error::Invalid("unknown time in force".into()));
                }
                out.extend_from_slice(market);
                out.push(*side);
                out.push(*tif);
                out.extend_from_slice(&price_ticks.to_le_bytes());
                out.extend_from_slice(&lots.to_le_bytes());
            }
            Payload::Cancel { order } => out.extend_from_slice(order),
            Payload::CancelAll => {}
            Payload::Transfer { to, asset, amount } => {
                if !in_atom_range(*amount) {
                    return Err(Error::Invalid("amount outside the protocol's range".into()));
                }
                out.extend_from_slice(to);
                out.extend_from_slice(asset);
                out.extend_from_slice(&amount.to_le_bytes());
            }
        }
        Ok(out)
    }

    /// The exact decode of `payload` as `kind`; `None` for a kind B3 Hive
    /// does not sign (it still shows such an action by its kind number).
    pub fn decode(kind: u8, payload: &[u8]) -> Result<Option<Self>, Error> {
        let mut reader = Reader::new(payload);
        let decoded = match kind {
            KIND_PLACE => {
                let market = reader.h256()?;
                let side = reader.u8()?;
                let tif = reader.u8()?;
                if !matches!(side, SIDE_BUY | SIDE_SELL)
                    || !matches!(tif, TIF_GTC | TIF_IOC | TIF_POST_ONLY)
                {
                    return Err(Error::Decode("unknown side or time in force"));
                }
                Payload::Place {
                    market,
                    side,
                    tif,
                    price_ticks: reader.u64()?,
                    lots: reader.u64()?,
                }
            }
            KIND_CANCEL => Payload::Cancel {
                order: reader.h256()?,
            },
            KIND_CANCEL_ALL => Payload::CancelAll,
            KIND_TRANSFER => {
                let to = reader.h256()?;
                let asset = reader.h256()?;
                let amount = reader.i64()?;
                if !in_atom_range(amount) {
                    return Err(Error::Decode("amount outside [0, MAX_ATOMS]"));
                }
                Payload::Transfer { to, asset, amount }
            }
            _ => return Ok(None),
        };
        reader.finish()?;
        Ok(Some(decoded))
    }
}

/// A signed user action (core/action.h, `ActionV2`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Action {
    pub domain: H256,
    pub signer: [u8; 32],
    pub nonce: u64,
    pub ref_slot: u64,
    pub max_delay: u16,
    pub kind: u8,
    pub payload: Vec<u8>,
    pub sig: [u8; 64],
}

impl Action {
    /// Every field except the signature, in wire order.
    pub fn unsigned_msg(&self) -> Vec<u8> {
        let mut out = Vec::with_capacity(ACTION_FIXED_PREFIX + 3 + self.payload.len());
        out.push(ACTION_VERSION);
        out.extend_from_slice(&self.domain);
        out.extend_from_slice(&self.signer);
        out.extend_from_slice(&self.nonce.to_le_bytes());
        out.extend_from_slice(&self.ref_slot.to_le_bytes());
        out.extend_from_slice(&self.max_delay.to_le_bytes());
        out.push(self.kind);
        put_compact(&mut out, self.payload.len() as u64);
        out.extend_from_slice(&self.payload);
        out
    }

    /// The exact bytes a seat admits: the unsigned message, then the signature.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = self.unsigned_msg();
        out.extend_from_slice(&self.sig);
        out
    }

    /// `ActionV2::Id`: the BIP340 message, and the order id of a PLACE.
    pub fn id(&self) -> H256 {
        tagged(Tag::Action, &[&self.unsigned_msg()])
    }

    pub fn account(&self) -> H256 {
        account_of(&self.signer)
    }

    /// `ActionV2::Decode`: exact and capped. It does not check the domain,
    /// the expiry window or the signature.
    pub fn decode(bytes: &[u8], profile: &Profile) -> Result<Self, Error> {
        let cap = profile.max_action_payload_bytes as usize;
        if bytes.len() > ACTION_FIXED_PREFIX + compact_len(cap as u64) + cap + 64 {
            return Err(Error::Oversize("action longer than its maximum encoding"));
        }
        let mut reader = Reader::new(bytes);
        if reader.u8()? != ACTION_VERSION {
            return Err(Error::Decode("unknown action version"));
        }
        let domain = reader.h256()?;
        let signer = reader.array()?;
        let nonce = reader.u64()?;
        let ref_slot = reader.u64()?;
        let max_delay = reader.u16()?;
        let kind = reader.u8()?;
        if !(1..=7).contains(&kind) {
            return Err(Error::Decode("unknown action kind"));
        }
        let length = reader.count(cap)?;
        let payload = reader.take(length)?.to_vec();
        let sig = reader.array()?;
        reader.finish()?;
        Ok(Self {
            domain,
            signer,
            nonce,
            ref_slot,
            max_delay,
            kind,
            payload,
            sig,
        })
    }
}

// ------------------------------------------------------- headers and proofs

pub const SLOT_HEADER_BYTES: usize = 366;
/// The largest seat set (core/seat_set.h) and its bitmap.
pub const MAX_SEATS: usize = 1_024;
const MAX_BITMAP_BYTES: usize = MAX_SEATS / 8;
const MAX_MERKLE_PATH: usize = 64;

/// A slot header (core/slot.h, `SlotHeaderV2`), decoded from its 366 bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    pub domain: H256,
    pub profile_hash: H256,
    pub authority_epoch: u64,
    pub height: u64,
    pub parent_hash: H256,
    pub parent_cert_hash: H256,
    pub anchor_height: u32,
    pub anchor_hash: H256,
    pub seat_set_root: H256,
    pub facts_root: H256,
    pub actions_root: H256,
    pub outcomes_root: H256,
    pub state_root: H256,
    pub action_count: u32,
    pub outcome_count: u32,
    pub commitment_version: u8,
    /// The exact bytes: what the hash and the certificate digest cover.
    bytes: Vec<u8>,
}

impl Header {
    fn read(reader: &mut Reader<'_>) -> Result<Self, Error> {
        let bytes = reader.take(SLOT_HEADER_BYTES)?.to_vec();
        let mut r = Reader::new(&bytes);
        if r.u8()? != 1 {
            return Err(Error::Decode("unknown slot header version"));
        }
        let domain = r.h256()?;
        let profile_hash = r.h256()?;
        let authority_epoch = r.u64()?;
        let height = r.u64()?;
        let parent_hash = r.h256()?;
        let parent_cert_hash = r.h256()?;
        let anchor_height = r.u32()?;
        let anchor_hash = r.h256()?;
        r.take(16)?; // facts_from and facts_to: two (height, index) cursors
        let seat_set_root = r.h256()?;
        let facts_root = r.h256()?;
        let actions_root = r.h256()?;
        let outcomes_root = r.h256()?;
        let state_root = r.h256()?;
        let action_count = r.u32()?;
        let outcome_count = r.u32()?;
        let commitment_version = r.u8()?;
        if !matches!(commitment_version, 0 | 2 | 3) {
            return Err(Error::Decode("unknown commitment version"));
        }
        r.finish()?;
        Ok(Self {
            domain,
            profile_hash,
            authority_epoch,
            height,
            parent_hash,
            parent_cert_hash,
            anchor_height,
            anchor_hash,
            seat_set_root,
            facts_root,
            actions_root,
            outcomes_root,
            state_root,
            action_count,
            outcome_count,
            commitment_version,
            bytes,
        })
    }

    /// `SlotHeaderV2::Hash`: the slot's identity.
    pub fn hash(&self) -> H256 {
        tagged(Tag::Slot, &[&self.bytes])
    }

    /// `SlotHeaderV2::CommitDigest`: what the seats sign for commitment
    /// versions 0 and 2.
    pub fn commit_digest(&self) -> H256 {
        tagged(Tag::Commit, &[&self.bytes])
    }

    /// `VersionedCommitDigest` for commitment version 3.
    pub fn view_bound_digest(&self, view: u64) -> H256 {
        tagged(Tag::Commit, &[&[3u8], &self.bytes, &view.to_le_bytes()])
    }
}

/// A slot certificate (core/certificate.h): which seats signed the header.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Certificate {
    pub header_hash: H256,
    pub bitmap: Vec<u8>,
    pub aggregate: [u8; 96],
    pub view: Option<u64>,
}

/// A header with its certificate (read/views.h, `CertifiedEnvelope`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Envelope {
    pub header: Header,
    pub cert: Certificate,
}

impl Envelope {
    /// `CertifiedEnvelope::Decode`: commitment versions 0 and 2 carry no
    /// view; version 3 carries exactly its eight-byte view.
    pub fn decode(bytes: &[u8]) -> Result<Self, Error> {
        if bytes.len() > SLOT_HEADER_BYTES + 32 + 1 + MAX_BITMAP_BYTES + 96 + 8 {
            return Err(Error::Oversize("envelope longer than its maximum encoding"));
        }
        let mut reader = Reader::new(bytes);
        let header = Header::read(&mut reader)?;
        let header_hash = reader.h256()?;
        let length = reader.count(MAX_BITMAP_BYTES)?;
        let bitmap = reader.take(length)?.to_vec();
        let aggregate = reader.array()?;
        let view = if header.commitment_version == 3 {
            Some(reader.u64()?)
        } else {
            None
        };
        reader.finish()?;
        Ok(Self {
            header,
            cert: Certificate {
                header_hash,
                bitmap,
                aggregate,
                view,
            },
        })
    }
}

/// A Merkle inclusion proof (core/merkle.h).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MerkleProof {
    pub index: u64,
    pub size: u64,
    pub path: Vec<H256>,
}

impl MerkleProof {
    pub fn decode(bytes: &[u8]) -> Result<Self, Error> {
        let mut reader = Reader::new(bytes);
        let index = reader.u64()?;
        let size = reader.u64()?;
        let count = reader.count(MAX_MERKLE_PATH)?;
        let path = (0..count)
            .map(|_| reader.h256())
            .collect::<Result<_, _>>()?;
        reader.finish()?;
        Ok(Self { index, size, path })
    }
}

fn merkle_leaf(value: &H256) -> H256 {
    tagged(Tag::MerkleLeaf, &[value])
}

fn merkle_node(left: &H256, right: &H256) -> H256 {
    tagged(Tag::MerkleNode, &[left, right])
}

fn merkle_bind(size: u64, tree_hash: &H256) -> H256 {
    tagged(Tag::MerkleRoot, &[&size.to_le_bytes(), tree_hash])
}

/// `MerkleRoot`: the committed root of `leaves`.
pub fn merkle_root(leaves: &[H256]) -> H256 {
    if leaves.is_empty() {
        return merkle_bind(0, &tagged(Tag::MerkleEmpty, &[]));
    }
    let mut level: Vec<H256> = leaves.iter().map(merkle_leaf).collect();
    while level.len() > 1 {
        level = level
            .chunks(2)
            .map(|pair| {
                if pair.len() == 2 {
                    merkle_node(&pair[0], &pair[1])
                } else {
                    pair[0]
                }
            })
            .collect();
    }
    merkle_bind(leaves.len() as u64, &level[0])
}

/// `MerkleVerify` (RFC 9162 section 2.1.3.2, then the root binding).
pub fn merkle_verify(leaf: &H256, proof: &MerkleProof, root: &H256) -> bool {
    if proof.index >= proof.size || proof.path.len() > MAX_MERKLE_PATH {
        return false;
    }
    let mut fn_ = proof.index;
    let mut sn = proof.size - 1;
    let mut r = merkle_leaf(leaf);
    for p in &proof.path {
        if sn == 0 {
            return false;
        }
        if fn_ & 1 != 0 || fn_ == sn {
            r = merkle_node(p, &r);
            while fn_ & 1 == 0 && fn_ != 0 {
                fn_ >>= 1;
                sn >>= 1;
            }
        } else {
            r = merkle_node(&r, p);
        }
        fn_ >>= 1;
        sn >>= 1;
    }
    sn == 0 && merkle_bind(proof.size, &r) == *root
}

// ------------------------------------------------------------ account state

const MAX_LEAF_ASSETS: usize = 256;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Balance {
    pub asset: H256,
    pub available: i64,
    pub reserved: i64,
}

/// One account's committed state (core/account_leaf.h, `AccountLeafV0`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccountLeaf {
    pub account: H256,
    pub balances: Vec<Balance>,
    pub used_nonces: Vec<u64>,
    pub open_orders: u32,
    bytes: Vec<u8>,
}

impl AccountLeaf {
    /// `AccountLeafV0::Decode`: exact, capped and canonical (balances
    /// ascending by asset and never both zero, nonces strictly ascending).
    pub fn decode(bytes: &[u8], profile: &Profile) -> Result<Self, Error> {
        let window = profile.nonce_window as usize;
        let max = 32
            + compact_len(MAX_LEAF_ASSETS as u64)
            + MAX_LEAF_ASSETS * 48
            + compact_len(window as u64)
            + window * 8
            + 4;
        if bytes.len() > max {
            return Err(Error::Oversize(
                "account leaf longer than its maximum encoding",
            ));
        }
        let mut reader = Reader::new(bytes);
        let account = reader.h256()?;
        let count = reader.count(MAX_LEAF_ASSETS)?;
        let mut balances = Vec::with_capacity(count);
        for _ in 0..count {
            let balance = Balance {
                asset: reader.h256()?,
                available: reader.i64()?,
                reserved: reader.i64()?,
            };
            if !in_atom_range(balance.available) || !in_atom_range(balance.reserved) {
                return Err(Error::Decode("balance outside [0, MAX_ATOMS]"));
            }
            balances.push(balance);
        }
        let count = reader.count(window)?;
        let used_nonces = (0..count)
            .map(|_| reader.u64())
            .collect::<Result<Vec<_>, _>>()?;
        let open_orders = reader.u32()?;
        reader.finish()?;
        let leaf = Self {
            account,
            balances,
            used_nonces,
            open_orders,
            bytes: bytes.to_vec(),
        };
        if !leaf.is_canonical(profile) {
            return Err(Error::Decode("account leaf is not canonical"));
        }
        Ok(leaf)
    }

    pub fn is_canonical(&self, profile: &Profile) -> bool {
        self.balances.len() <= MAX_LEAF_ASSETS
            && self.used_nonces.len() <= profile.nonce_window as usize
            && self.balances.iter().all(|b| {
                in_atom_range(b.available)
                    && in_atom_range(b.reserved)
                    && (b.available != 0 || b.reserved != 0)
            })
            && self
                .balances
                .windows(2)
                .all(|pair| pair[0].asset < pair[1].asset)
            && self.used_nonces.windows(2).all(|pair| pair[0] < pair[1])
            && self.open_orders <= profile.max_open_orders_per_account
    }

    /// `AccountLeafV0::LeafValue`: the leaf of the accounts tree.
    pub fn leaf_value(&self) -> H256 {
        tagged(Tag::StateAccounts, &[&self.bytes])
    }
}

/// The five component roots of commitment version 0 (core/state_root.h).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StateRoots {
    pub accounts: H256,
    bytes: [u8; 160],
}

impl StateRoots {
    pub fn decode(bytes: &[u8]) -> Result<Self, Error> {
        let bytes: [u8; 160] = bytes
            .try_into()
            .map_err(|_| Error::Decode("state roots are not 160 bytes"))?;
        let accounts = bytes[..32].try_into().expect("32 bytes");
        Ok(Self { accounts, bytes })
    }

    /// `StateRootsV0::Root` for commitment version 0.
    pub fn root(&self) -> H256 {
        tagged(Tag::State, &[&[0u8], &self.bytes])
    }
}

// ----------------------------------------------------------------- outcomes

/// One match (core/outcome.h, `Fill`), 137 bytes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Fill {
    pub maker: H256,
    pub taker: H256,
    pub market: H256,
    pub taker_side: u8,
    pub price_ticks: u64,
    pub lots: u64,
    pub quote_atoms: i64,
    pub seller_fee: i64,
    pub seller_quote_before: i64,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BalanceDelta {
    pub account: H256,
    pub asset: H256,
    pub available_delta: i64,
    pub reserved_delta: i64,
}

pub const SOURCE_ACTION: u8 = 2;

/// The certified result of one fact, action or slot (core/outcome.h).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Outcome {
    pub source: u8,
    pub subject: H256,
    pub account: H256,
    pub nonce: u64,
    pub code: u16,
    pub remaining_lots: u64,
    pub fills: Vec<Fill>,
    pub deltas: Vec<BalanceDelta>,
    bytes: Vec<u8>,
}

/// The registry name of an outcome code (core/codes.h), or `None`.
pub fn outcome_code_name(code: u16) -> Option<&'static str> {
    Some(match code {
        1 => "DEPOSIT_CREDITED",
        2 => "RESTING",
        3 => "FILLED",
        4 => "PARTIAL_RESTING",
        5 => "IOC_DONE",
        6 => "CANCELLED",
        7 => "TRANSFERRED",
        8 => "FEES_DISTRIBUTED",
        9 => "WITHDRAWAL_PENDING",
        10 => "WITHDRAWAL_PAID",
        64 => "REJ_INSUFFICIENT_AVAILABLE",
        65 => "REJ_UNKNOWN_MARKET",
        66 => "REJ_UNKNOWN_ORDER",
        67 => "REJ_NOT_OWN_ORDER",
        68 => "REJ_ORDER_CLOSED",
        69 => "REJ_WOULD_CROSS",
        70 => "REJ_BOOK_FULL",
        71 => "REJ_FILL_CAP",
        72 => "REJ_PRICE_BAND",
        73 => "REJ_OPEN_ORDER_CAP",
        74 => "REJ_BAD_PARAMS",
        75 => "REJ_UNKNOWN_ASSET",
        76 => "REJ_NO_RECIPIENT",
        96 => "REJ_RESIDUAL_CROSS_UNSUPPORTED",
        _ => return None,
    })
}

impl Outcome {
    /// `OutcomeV2::Decode`: exact and capped by the profile.
    pub fn decode(bytes: &[u8], profile: &Profile) -> Result<Self, Error> {
        let max_fills = profile.max_fills_per_order as usize;
        let max_deltas = 2 * (max_fills + 1);
        let max = 83
            + compact_len(max_fills as u64)
            + max_fills * 137
            + compact_len(max_deltas as u64)
            + max_deltas * 80;
        if bytes.len() > max {
            return Err(Error::Oversize("outcome longer than its maximum encoding"));
        }
        let mut reader = Reader::new(bytes);
        let source = reader.u8()?;
        if !(1..=3).contains(&source) {
            return Err(Error::Decode("unknown outcome source"));
        }
        let subject = reader.h256()?;
        let account = reader.h256()?;
        let nonce = reader.u64()?;
        let code = reader.u16()?;
        if outcome_code_name(code).is_none() {
            return Err(Error::Decode("unknown outcome code"));
        }
        let remaining_lots = reader.u64()?;
        let count = reader.count(max_fills)?;
        let mut fills = Vec::with_capacity(count);
        for _ in 0..count {
            let fill = Fill {
                maker: reader.h256()?,
                taker: reader.h256()?,
                market: reader.h256()?,
                taker_side: reader.u8()?,
                price_ticks: reader.u64()?,
                lots: reader.u64()?,
                quote_atoms: reader.i64()?,
                seller_fee: reader.i64()?,
                seller_quote_before: reader.i64()?,
            };
            if !matches!(fill.taker_side, SIDE_BUY | SIDE_SELL)
                || !in_atom_range(fill.quote_atoms)
                || !in_atom_range(fill.seller_fee)
                || !in_atom_range(fill.seller_quote_before)
            {
                return Err(Error::Decode("fill outside the protocol's ranges"));
            }
            fills.push(fill);
        }
        let count = reader.count(max_deltas)?;
        let mut deltas = Vec::with_capacity(count);
        for _ in 0..count {
            let delta = BalanceDelta {
                account: reader.h256()?,
                asset: reader.h256()?,
                available_delta: reader.i64()?,
                reserved_delta: reader.i64()?,
            };
            let in_range = |d: i64| (-MAX_ATOMS..=MAX_ATOMS).contains(&d);
            if !in_range(delta.available_delta) || !in_range(delta.reserved_delta) {
                return Err(Error::Decode(
                    "balance delta outside [-MAX_ATOMS, MAX_ATOMS]",
                ));
            }
            deltas.push(delta);
        }
        reader.finish()?;
        Ok(Self {
            source,
            subject,
            account,
            nonce,
            code,
            remaining_lots,
            fills,
            deltas,
            bytes: bytes.to_vec(),
        })
    }

    /// `OutcomeV2::LeafHash`: the leaf of the slot's outcomes tree.
    pub fn leaf_hash(&self) -> H256 {
        tagged(Tag::Outcome, &[&self.bytes])
    }

    /// The exact bytes, as the seat sent them.
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}

// --------------------------------------------------------------- fee, nonce

/// `MulDivCeil` for the fee: ceil(cum_quote * fee_ppm / 1,000,000).
fn seller_fee_total(cum_quote: i64, fee_ppm: u32) -> Option<i64> {
    if !in_atom_range(cum_quote) || fee_ppm > 1_000_000 {
        return None;
    }
    let product = cum_quote as u128 * u128::from(fee_ppm);
    Some(product.div_ceil(1_000_000) as i64)
}

/// `SellerFeeIncrement`: the fee a fill adds when the seller's cumulative
/// quote goes from `before` to `after`.
pub fn seller_fee_increment(before: i64, after: i64, fee_ppm: u32) -> Option<i64> {
    if before > after {
        return None;
    }
    let increment = seller_fee_total(after, fee_ppm)? - seller_fee_total(before, fee_ppm)?;
    in_atom_range(increment).then_some(increment)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NonceVerdict {
    Acceptable,
    Used,
    TooLow,
}

/// `ClassifyNonce` (core/nonce_rule.h): `used` is the account's window of
/// its highest used nonces, strictly ascending. `None` for a malformed window.
pub fn classify_nonce(used: &[u64], nonce: u64, window: u32) -> Option<NonceVerdict> {
    if window == 0 || used.len() > window as usize || !used.windows(2).all(|pair| pair[0] < pair[1])
    {
        return None;
    }
    Some(if used.binary_search(&nonce).is_ok() {
        NonceVerdict::Used
    } else if used.len() < window as usize || nonce > used[0] {
        NonceVerdict::Acceptable
    } else {
        NonceVerdict::TooLow
    })
}

/// `ExpiredAt`: no slot at or after `slot` may include an action with this
/// window.
pub fn expired_at(ref_slot: u64, max_delay: u16, slot: u64) -> bool {
    slot > ref_slot && slot - ref_slot > u64::from(max_delay)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_action() -> Action {
        let payload = Payload::Place {
            market: [9; 32],
            side: SIDE_BUY,
            tif: TIF_GTC,
            price_ticks: 100,
            lots: 5,
        };
        Action {
            domain: [1; 32],
            signer: [2; 32],
            nonce: 7,
            ref_slot: 40,
            max_delay: 64,
            kind: payload.kind(),
            payload: payload.encode().unwrap(),
            sig: [3; 64],
        }
    }

    #[test]
    fn an_action_round_trips_and_keeps_its_kind_at_byte_83() {
        let action = sample_action();
        let bytes = action.encode();
        assert_eq!(bytes.len(), 84 + 1 + 50 + 64);
        assert_eq!(bytes[83], KIND_PLACE);
        assert_eq!(Action::decode(&bytes, &Profile::dev_regtest()), Ok(action));
    }

    #[test]
    fn the_action_id_ignores_the_signature() {
        let action = sample_action();
        let mut other = action.clone();
        other.sig = [4; 64];
        assert_eq!(action.id(), other.id());
        other.nonce += 1;
        assert_ne!(action.id(), other.id());
    }

    #[test]
    fn a_malformed_action_is_refused() {
        let profile = Profile::dev_regtest();
        let bytes = sample_action().encode();
        let mut trailing = bytes.clone();
        trailing.push(0);
        assert_eq!(
            Action::decode(&trailing, &profile),
            Err(Error::Decode("trailing bytes"))
        );
        assert_eq!(
            Action::decode(&bytes[..bytes.len() - 1], &profile),
            Err(Error::Decode("truncated"))
        );
        let mut version = bytes.clone();
        version[0] = 2;
        assert_eq!(
            Action::decode(&version, &profile),
            Err(Error::Decode("unknown action version"))
        );
        let mut kind = bytes.clone();
        kind[83] = 8;
        assert_eq!(
            Action::decode(&kind, &profile),
            Err(Error::Decode("unknown action kind"))
        );
        let huge = vec![0u8; 84 + 3 + 4_096 + 64 + 1];
        assert!(matches!(
            Action::decode(&huge, &profile),
            Err(Error::Oversize(_))
        ));
    }

    #[test]
    fn payloads_round_trip_and_refuse_bad_values() {
        let payloads = [
            Payload::Place {
                market: [9; 32],
                side: SIDE_SELL,
                tif: TIF_POST_ONLY,
                price_ticks: 1,
                lots: 2,
            },
            Payload::Cancel { order: [8; 32] },
            Payload::CancelAll,
            Payload::Transfer {
                to: [7; 32],
                asset: [6; 32],
                amount: MAX_ATOMS,
            },
        ];
        for payload in payloads {
            let bytes = payload.encode().unwrap();
            assert_eq!(Payload::decode(payload.kind(), &bytes), Ok(Some(payload)));
        }
        assert!(Payload::Place {
            market: [0; 32],
            side: 3,
            tif: TIF_GTC,
            price_ticks: 1,
            lots: 1
        }
        .encode()
        .is_err());
        assert!(Payload::Place {
            market: [0; 32],
            side: SIDE_BUY,
            tif: 0,
            price_ticks: 1,
            lots: 1
        }
        .encode()
        .is_err());
        assert!(Payload::Transfer {
            to: [0; 32],
            asset: [0; 32],
            amount: -1
        }
        .encode()
        .is_err());
        assert!(Payload::Transfer {
            to: [0; 32],
            asset: [0; 32],
            amount: MAX_ATOMS + 1
        }
        .encode()
        .is_err());
        assert_eq!(
            Payload::decode(KIND_CANCEL_ALL, &[0]),
            Err(Error::Decode("trailing bytes"))
        );
        assert_eq!(Payload::decode(KIND_WITHDRAW, &[]), Ok(None));
    }

    #[test]
    fn market_amounts_are_checked_against_the_atom_range() {
        let market = Market {
            base: [0; 32],
            quote: [1; 32],
            base_atoms_per_lot: 1_000_000,
            quote_atoms_per_tick: 10,
        };
        assert_eq!(market.base_atoms(5), Some(5_000_000));
        assert_eq!(market.quote_atoms(5, 100), Some(5_000));
        assert_eq!(market.base_atoms(u64::MAX), None);
        assert_eq!(market.quote_atoms(1 << 40, 1 << 40), None);
        let same = Market {
            base: [1; 32],
            ..market.clone()
        };
        assert!(!same.valid());
        assert_eq!(same.base_atoms(1), None);
    }

    #[test]
    fn the_merkle_root_matches_its_proofs() {
        // A three-leaf tree: the third leaf is promoted, so its path has one entry.
        let leaves = [[1u8; 32], [2u8; 32], [3u8; 32]];
        let root = merkle_root(&leaves);
        let l: Vec<H256> = leaves.iter().map(merkle_leaf).collect();
        let n01 = merkle_node(&l[0], &l[1]);
        let proofs = [
            MerkleProof {
                index: 0,
                size: 3,
                path: vec![l[1], l[2]],
            },
            MerkleProof {
                index: 1,
                size: 3,
                path: vec![l[0], l[2]],
            },
            MerkleProof {
                index: 2,
                size: 3,
                path: vec![n01],
            },
        ];
        for (leaf, proof) in leaves.iter().zip(&proofs) {
            assert!(merkle_verify(leaf, proof, &root));
        }
        // A wrong leaf, a wrong size and a wrong index all fail.
        assert!(!merkle_verify(&leaves[1], &proofs[0], &root));
        assert!(!merkle_verify(
            &leaves[0],
            &MerkleProof {
                size: 4,
                ..proofs[0].clone()
            },
            &root
        ));
        assert!(!merkle_verify(
            &leaves[0],
            &MerkleProof {
                index: 3,
                ..proofs[0].clone()
            },
            &root
        ));
        assert_ne!(merkle_root(&[]), merkle_root(&leaves[..1]));
    }

    #[test]
    fn the_fee_is_the_ceiling_of_the_cumulative_rate() {
        // 100 ppm: one atom of fee per 10,000 atoms of quote, rounded up once
        // over the cumulative quote.
        assert_eq!(seller_fee_increment(0, 1, 100), Some(1));
        assert_eq!(seller_fee_increment(1, 10_000, 100), Some(0));
        assert_eq!(seller_fee_increment(10_000, 10_001, 100), Some(1));
        assert_eq!(seller_fee_increment(0, 5_000_000, 100), Some(500));
        assert_eq!(seller_fee_increment(5, 4, 100), None);
        assert_eq!(seller_fee_increment(0, 1, 1_000_001), None);
    }

    #[test]
    fn the_nonce_rule_is_the_ledgers() {
        assert_eq!(classify_nonce(&[1, 2, 3], 2, 100), Some(NonceVerdict::Used));
        assert_eq!(
            classify_nonce(&[1, 2, 3], 9, 100),
            Some(NonceVerdict::Acceptable)
        );
        // A window that is not full accepts a nonce below its lowest.
        assert_eq!(
            classify_nonce(&[5, 6], 1, 100),
            Some(NonceVerdict::Acceptable)
        );
        // A full window refuses one below its lowest.
        assert_eq!(classify_nonce(&[5, 6], 1, 2), Some(NonceVerdict::TooLow));
        assert_eq!(classify_nonce(&[6, 5], 1, 2), None);
        assert_eq!(classify_nonce(&[], 1, 0), None);
    }

    #[test]
    fn an_action_expires_only_after_its_window() {
        assert!(!expired_at(10, 5, 15));
        assert!(expired_at(10, 5, 16));
        assert!(!expired_at(10, 5, 10));
    }
}
