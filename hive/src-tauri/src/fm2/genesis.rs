//! The public genesis document of a FlowMesh V2 network
//! (src/flowmesh2/netio/genesis_doc.h, format `fm2-genesis/1`): the seats
//! with their BLS keys and API addresses, the assets and the markets. It is a
//! trust input: it reaches the client out of band and is never taken from a
//! seat. Only what a client needs is read; the rest is ignored.

use std::net::IpAddr;

use serde_json::Value;

use super::bytes::{id_from_hex, unhex, H256};
use super::types::Market;
use super::Error;

/// A genesis document is a few kilobytes; a megabyte is not one.
pub const MAX_GENESIS_BYTES: u64 = 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GenesisSeat {
    pub index: u32,
    pub bls_pubkey: [u8; 48],
    pub pop: [u8; 96],
    pub api_address: IpAddr,
    pub api_port: u16,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GenesisAsset {
    pub name: String,
    pub id: H256,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GenesisMarket {
    pub base: String,
    pub quote: String,
    pub descriptor: Market,
}

impl GenesisMarket {
    /// `BASE/QUOTE`, as fm2-cli names a market.
    pub fn name(&self) -> String {
        format!("{}/{}", self.base, self.quote)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Genesis {
    pub magic: [u8; 4],
    pub modern_chain_domain: H256,
    pub seats: Vec<GenesisSeat>,
    /// Sorted by name.
    pub assets: Vec<GenesisAsset>,
    pub markets: Vec<GenesisMarket>,
}

const SHAPE: Error = Error::Decode("genesis document is not the expected JSON");
const HEX: Error = Error::Decode("a genesis field is not hex of the expected length");

fn array<const N: usize>(value: &Value) -> Result<[u8; N], Error> {
    value
        .as_str()
        .and_then(unhex)
        .and_then(|bytes| bytes.try_into().ok())
        .ok_or(HEX)
}

fn id(value: &Value) -> Result<H256, Error> {
    value.as_str().and_then(id_from_hex).ok_or(HEX)
}

/// An unsigned integer written as digits only.
fn number(value: &Value) -> Result<u64, Error> {
    match value {
        Value::Number(n) if n.to_string().bytes().all(|b| b.is_ascii_digit()) => {
            n.as_u64().ok_or(SHAPE)
        }
        _ => Err(SHAPE),
    }
}

impl Genesis {
    pub fn parse(json: &str) -> Result<Self, Error> {
        let root: Value = serde_json::from_str(json).map_err(|_| SHAPE)?;
        if root["format"].as_str() != Some("fm2-genesis/1") {
            return Err(Error::Decode("genesis document of another format"));
        }
        let network = &root["network"];
        if network["chain"].as_str() != Some("regtest") {
            return Err(Error::Invalid(
                "this genesis document is not for regtest; B3 Hive trades on FlowMesh V2 regtest only".into(),
            ));
        }
        let magic = array(&network["magic"])?;
        let modern_chain_domain = id(&network["domain"])?;

        let listed = root["seats"]
            .as_array()
            .filter(|seats| !seats.is_empty())
            .ok_or(SHAPE)?;
        let mut seats = Vec::with_capacity(listed.len());
        for (position, seat) in listed.iter().enumerate() {
            if number(&seat["index"])? != position as u64 {
                return Err(Error::Decode("genesis seats are not 0..k-1 in order"));
            }
            let api = &seat["api"];
            let api_address = api["address"]
                .as_str()
                .and_then(|text| text.parse::<IpAddr>().ok())
                .ok_or(Error::Invalid(
                    "a seat's API address is not an IP address".into(),
                ))?;
            let api_port = u16::try_from(number(&api["port"])?).map_err(|_| SHAPE)?;
            seats.push(GenesisSeat {
                index: position as u32,
                bls_pubkey: array(&seat["bls_pubkey"])?,
                pop: array(&seat["pop"])?,
                api_address,
                api_port,
            });
        }

        let table = root["assets"].as_object().ok_or(SHAPE)?;
        let mut assets = Vec::with_capacity(table.len());
        for (name, value) in table {
            assets.push(GenesisAsset {
                name: name.clone(),
                id: id(value)?,
            });
        }
        assets.sort_by(|a, b| a.name.cmp(&b.name));

        let listed = root["markets"].as_array().ok_or(SHAPE)?;
        let mut markets = Vec::with_capacity(listed.len());
        let asset = |name: &str| {
            assets
                .iter()
                .find(|asset| asset.name == name)
                .map(|asset| asset.id)
        };
        for market in listed {
            let base = market["base"].as_str().ok_or(SHAPE)?;
            let quote = market["quote"].as_str().ok_or(SHAPE)?;
            let unknown = Error::Decode("a genesis market names an asset outside the table");
            let descriptor = Market {
                base: asset(base).ok_or(unknown.clone())?,
                quote: asset(quote).ok_or(unknown)?,
                base_atoms_per_lot: number(&market["base_atoms_per_lot"])?,
                quote_atoms_per_tick: number(&market["quote_atoms_per_tick"])?,
            };
            if !descriptor.valid() {
                return Err(Error::Invalid(format!(
                    "the genesis market {base}/{quote} is not a valid market"
                )));
            }
            markets.push(GenesisMarket {
                base: base.to_owned(),
                quote: quote.to_owned(),
                descriptor,
            });
        }
        Ok(Self {
            magic,
            modern_chain_domain,
            seats,
            assets,
            markets,
        })
    }

    pub fn asset_name(&self, id: &H256) -> Option<&str> {
        self.assets
            .iter()
            .find(|asset| asset.id == *id)
            .map(|asset| asset.name.as_str())
    }

    pub fn market(&self, id: &H256) -> Option<&GenesisMarket> {
        self.markets
            .iter()
            .find(|market| market.descriptor.id() == *id)
    }
}

/// The SPKI pins of the seats' API certificates: a JSON object of seat index
/// to the SHA-256 of the certificate's SubjectPublicKeyInfo, hex (what
/// fm2-cli takes as `--pins`). One pin per genesis seat, in seat order.
pub fn parse_pins(json: &str, seats: usize) -> Result<Vec<[u8; 32]>, Error> {
    let shape = Error::Decode("the pins file is not a JSON object of seat index to fingerprint");
    let root: Value = serde_json::from_str(json).map_err(|_| shape.clone())?;
    let table = root.as_object().ok_or(shape)?;
    (0..seats)
        .map(|seat| {
            table
                .get(&seat.to_string())
                .and_then(Value::as_str)
                .and_then(unhex)
                .and_then(|bytes| bytes.try_into().ok())
                .ok_or(Error::Invalid(format!(
                    "the pins file lacks a fingerprint for seat {seat}"
                )))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fm2::bytes::id_hex;

    pub const GENESIS: &str = include_str!("../../tests/fixtures/fm2/genesis.json");
    const PINS: &str = include_str!("../../tests/fixtures/fm2/pins.json");

    #[test]
    fn a_genesis_document_from_fm2_keygen_parses() {
        let genesis = Genesis::parse(GENESIS).unwrap();
        assert_eq!(genesis.seats.len(), 4);
        assert_eq!(genesis.seats[2].index, 2);
        assert_eq!(genesis.seats[0].api_address.to_string(), "127.0.0.1");
        assert_eq!(genesis.seats[3].api_port, 28803);
        let names: Vec<&str> = genesis
            .assets
            .iter()
            .map(|asset| asset.name.as_str())
            .collect();
        assert_eq!(names, ["B3", "TOK", "rUSD"]);
        assert_eq!(genesis.assets[0].id, [0u8; 32]);
        assert_eq!(genesis.markets.len(), 2);
        assert_eq!(genesis.markets[0].name(), "B3/rUSD");
        // The market id fm2-cli prints for B3/rUSD of this document.
        assert_eq!(
            id_hex(&genesis.markets[0].descriptor.id()),
            "89e36bf079571cf562b1b340c2b29d332f58aec4d4bb97008744d891b2521931"
        );
        assert_eq!(genesis.asset_name(&genesis.assets[2].id), Some("rUSD"));
    }

    fn changed(edit: impl FnOnce(&mut Value)) -> Result<Genesis, Error> {
        let mut root: Value = serde_json::from_str(GENESIS).unwrap();
        edit(&mut root);
        Genesis::parse(&root.to_string())
    }

    #[test]
    fn a_document_that_is_not_regtest_or_not_this_format_is_refused() {
        assert!(matches!(
            changed(|g| g["network"]["chain"] = "main".into()),
            Err(Error::Invalid(_))
        ));
        assert!(matches!(
            changed(|g| g["format"] = "fm2-genesis/2".into()),
            Err(Error::Decode(_))
        ));
        assert!(Genesis::parse("[]").is_err());
        assert!(Genesis::parse("not json").is_err());
    }

    #[test]
    fn malformed_fields_are_refused() {
        assert!(changed(|g| g["seats"][1]["index"] = 2.into()).is_err());
        assert!(changed(|g| g["seats"] = Value::Array(vec![])).is_err());
        assert!(changed(|g| g["seats"][0]["bls_pubkey"] = "00".into()).is_err());
        assert!(changed(|g| g["seats"][0]["api"]["address"] = "localhost".into()).is_err());
        assert!(changed(|g| g["seats"][0]["api"]["port"] = 70000.into()).is_err());
        assert!(changed(|g| g["seats"][0]["api"]["port"] = serde_json::json!(1.5)).is_err());
        assert!(changed(|g| g["assets"]["TOK"] = "abcd".into()).is_err());
        assert!(changed(|g| g["markets"][0]["base"] = "NOPE".into()).is_err());
        assert!(changed(|g| g["markets"][0]["base_atoms_per_lot"] = 0.into()).is_err());
        assert!(changed(|g| g["markets"][0]["quote"] = "B3".into()).is_err());
    }

    #[test]
    fn pins_name_every_seat() {
        let pins = parse_pins(PINS, 4).unwrap();
        assert_eq!(pins.len(), 4);
        assert!(matches!(parse_pins(PINS, 5), Err(Error::Invalid(_))));
        assert!(parse_pins("[]", 1).is_err());
        assert!(parse_pins(r#"{"0":"abcd"}"#, 1).is_err());
    }
}
