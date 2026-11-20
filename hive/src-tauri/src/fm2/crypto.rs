//! The two signature schemes of FlowMesh V2, from the node's own libraries
//! (build.rs compiles src/secp256k1 and src/blst):
//!
//! - BIP340 Schnorr over secp256k1 for client actions, as `CKey::SignSchnorr`
//!   signs them with no tweak (src/flowmesh2/cli/signer.cpp).
//! - BLS12-381 with public keys in G1 and signatures in G2 for seat
//!   certificates, with the exact rules of src/crypto/bls.cpp: canonical
//!   point decoding, proof-of-possession checks and FastAggregateVerify over
//!   the IETF proof-of-possession ciphersuite.
//!
//! This file holds every `unsafe` block of the client. Each call passes
//! fixed-size buffers that the C function reads or fills completely.

use std::os::raw::{c_int, c_uint, c_void};
use std::sync::OnceLock;

pub const PUBKEY_BYTES: usize = 48;
pub const SIGNATURE_BYTES: usize = 96;

const SIG_DST: &[u8] = b"BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";
const POP_DST: &[u8] = b"BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_";

// ---------------------------------------------------------------- secp256k1

const SECP256K1_CONTEXT_NONE: c_uint = 1;

#[repr(C)]
struct Keypair([u8; 96]);
#[repr(C)]
struct XOnlyPubkey([u8; 64]);

extern "C" {
    fn secp256k1_context_create(flags: c_uint) -> *mut c_void;
    fn secp256k1_context_randomize(ctx: *mut c_void, seed32: *const u8) -> c_int;
    fn secp256k1_keypair_create(
        ctx: *const c_void,
        keypair: *mut Keypair,
        seckey: *const u8,
    ) -> c_int;
    fn secp256k1_keypair_xonly_pub(
        ctx: *const c_void,
        pubkey: *mut XOnlyPubkey,
        pk_parity: *mut c_int,
        keypair: *const Keypair,
    ) -> c_int;
    fn secp256k1_xonly_pubkey_serialize(
        ctx: *const c_void,
        output32: *mut u8,
        pubkey: *const XOnlyPubkey,
    ) -> c_int;
    fn secp256k1_xonly_pubkey_parse(
        ctx: *const c_void,
        pubkey: *mut XOnlyPubkey,
        input32: *const u8,
    ) -> c_int;
    fn secp256k1_schnorrsig_sign32(
        ctx: *const c_void,
        sig64: *mut u8,
        msg32: *const u8,
        keypair: *const Keypair,
        aux_rand32: *const u8,
    ) -> c_int;
    fn secp256k1_schnorrsig_verify(
        ctx: *const c_void,
        sig64: *const u8,
        msg: *const u8,
        msglen: usize,
        pubkey: *const XOnlyPubkey,
    ) -> c_int;
}

/// The library's context. It is created and blinded once and only read
/// afterwards, which libsecp256k1 allows from any thread.
struct Context(*mut c_void);
unsafe impl Send for Context {}
unsafe impl Sync for Context {}

fn context() -> *const c_void {
    static CONTEXT: OnceLock<Context> = OnceLock::new();
    CONTEXT
        .get_or_init(|| {
            let mut seed = [0u8; 32];
            getrandom::fill(&mut seed).expect("the system has no random source");
            // SAFETY: context_create returns a context that lives for the
            // process; randomize reads 32 bytes of `seed`.
            unsafe {
                let ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
                assert!(!ctx.is_null(), "secp256k1 context");
                assert_eq!(secp256k1_context_randomize(ctx, seed.as_ptr()), 1);
                Context(ctx)
            }
        })
        .0
}

/// A client's signing key. The secret is wiped when the value is dropped.
pub struct SecretKey {
    keypair: Keypair,
    public: [u8; 32],
}

impl Drop for SecretKey {
    fn drop(&mut self) {
        wipe(&mut self.keypair.0);
    }
}

/// Overwrites a secret in a way the optimiser may not remove.
pub fn wipe(bytes: &mut [u8]) {
    for byte in bytes.iter_mut() {
        // SAFETY: `byte` is a valid, exclusive reference.
        unsafe { std::ptr::write_volatile(byte, 0) };
    }
    std::sync::atomic::compiler_fence(std::sync::atomic::Ordering::SeqCst);
}

impl SecretKey {
    /// `None` when the 32 bytes are not a valid secp256k1 secret (zero, or
    /// not below the group order).
    pub fn from_bytes(secret: &[u8; 32]) -> Option<Self> {
        let mut keypair = Keypair([0; 96]);
        let mut xonly = XOnlyPubkey([0; 64]);
        let mut public = [0u8; 32];
        // SAFETY: every pointer is to a buffer of the size the function
        // documents; the context is valid for the process.
        unsafe {
            if secp256k1_keypair_create(context(), &mut keypair, secret.as_ptr()) != 1 {
                return None;
            }
            assert_eq!(
                secp256k1_keypair_xonly_pub(context(), &mut xonly, std::ptr::null_mut(), &keypair),
                1
            );
            assert_eq!(
                secp256k1_xonly_pubkey_serialize(context(), public.as_mut_ptr(), &xonly),
                1
            );
        }
        Some(Self { keypair, public })
    }

    /// A new random key.
    pub fn generate() -> ([u8; 32], Self) {
        loop {
            let mut secret = [0u8; 32];
            getrandom::fill(&mut secret).expect("the system has no random source");
            if let Some(key) = Self::from_bytes(&secret) {
                return (secret, key);
            }
        }
    }

    /// The x-only public key: an action's `signer`.
    pub fn public(&self) -> [u8; 32] {
        self.public
    }

    /// A BIP340 signature of `message` with fresh auxiliary randomness. The
    /// signature is verified before it is returned, as the node's signer does.
    pub fn sign(&self, message: &[u8; 32]) -> [u8; 64] {
        let mut aux = [0u8; 32];
        getrandom::fill(&mut aux).expect("the system has no random source");
        let mut signature = [0u8; 64];
        // SAFETY: fixed-size buffers as documented by secp256k1_schnorrsig.h.
        let signed = unsafe {
            secp256k1_schnorrsig_sign32(
                context(),
                signature.as_mut_ptr(),
                message.as_ptr(),
                &self.keypair,
                aux.as_ptr(),
            )
        };
        assert_eq!(signed, 1, "BIP340 signing failed");
        assert!(
            schnorr_verify(&self.public, message, &signature),
            "BIP340 self-check failed"
        );
        signature
    }
}

/// True iff `signature` is a valid BIP340 signature of `message` by the
/// x-only key `public`.
pub fn schnorr_verify(public: &[u8; 32], message: &[u8; 32], signature: &[u8; 64]) -> bool {
    let mut xonly = XOnlyPubkey([0; 64]);
    // SAFETY: fixed-size buffers as documented by secp256k1_extrakeys.h and
    // secp256k1_schnorrsig.h.
    unsafe {
        secp256k1_xonly_pubkey_parse(context(), &mut xonly, public.as_ptr()) == 1
            && secp256k1_schnorrsig_verify(
                context(),
                signature.as_ptr(),
                message.as_ptr(),
                32,
                &xonly,
            ) == 1
    }
}

// --------------------------------------------------------------- BLS12-381

#[repr(C)]
#[derive(Clone, Copy)]
struct P1Affine([u64; 12]);
#[repr(C)]
struct P1([u64; 18]);
#[repr(C)]
#[derive(Clone, Copy)]
struct P2Affine([u64; 24]);

const BLST_SUCCESS: c_int = 0;

extern "C" {
    fn blst_p1_uncompress(out: *mut P1Affine, input: *const u8) -> c_int;
    fn blst_p1_affine_is_inf(point: *const P1Affine) -> bool;
    fn blst_p1_affine_in_g1(point: *const P1Affine) -> bool;
    fn blst_p1_affine_compress(out: *mut u8, point: *const P1Affine);
    fn blst_p1s_add(ret: *mut P1, points: *const *const P1Affine, npoints: usize);
    fn blst_p1_to_affine(out: *mut P1Affine, input: *const P1);
    fn blst_p2_uncompress(out: *mut P2Affine, input: *const u8) -> c_int;
    fn blst_p2_affine_is_inf(point: *const P2Affine) -> bool;
    fn blst_p2_affine_in_g2(point: *const P2Affine) -> bool;
    fn blst_p2_affine_compress(out: *mut u8, point: *const P2Affine);
    fn blst_core_verify_pk_in_g1(
        pk: *const P1Affine,
        signature: *const P2Affine,
        hash_or_encode: bool,
        msg: *const u8,
        msg_len: usize,
        dst: *const u8,
        dst_len: usize,
        aug: *const u8,
        aug_len: usize,
    ) -> c_int;
}

/// A seat's BLS public key: a canonical, non-infinity point of G1.
#[derive(Clone, Copy)]
pub struct BlsPublicKey {
    bytes: [u8; PUBKEY_BYTES],
    point: P1Affine,
}

/// A BLS signature: a canonical, non-infinity point of G2.
#[derive(Clone, Copy)]
pub struct BlsSignature {
    point: P2Affine,
}

impl BlsPublicKey {
    /// Uncompress, refuse infinity and points outside G1, and require that
    /// compressing again gives the same bytes (`bls::PublicKey::Decode`).
    pub fn decode(bytes: &[u8; PUBKEY_BYTES]) -> Option<Self> {
        let mut point = P1Affine([0; 12]);
        let mut again = [0u8; PUBKEY_BYTES];
        // SAFETY: 48-byte input and output buffers, one affine point.
        unsafe {
            if blst_p1_uncompress(&mut point, bytes.as_ptr()) != BLST_SUCCESS
                || blst_p1_affine_is_inf(&point)
                || !blst_p1_affine_in_g1(&point)
            {
                return None;
            }
            blst_p1_affine_compress(again.as_mut_ptr(), &point);
        }
        (again == *bytes).then_some(Self {
            bytes: *bytes,
            point,
        })
    }

    pub fn bytes(&self) -> &[u8; PUBKEY_BYTES] {
        &self.bytes
    }

    /// The proof of possession: a signature of the key's own bytes under the
    /// PoP domain (`bls::VerifyPoP`).
    pub fn pop_verifies(&self, pop: &BlsSignature) -> bool {
        core_verify(&self.point, &pop.point, &self.bytes, POP_DST)
    }
}

impl BlsSignature {
    /// The same rules as `BlsPublicKey::decode`, in G2.
    pub fn decode(bytes: &[u8; SIGNATURE_BYTES]) -> Option<Self> {
        let mut point = P2Affine([0; 24]);
        let mut again = [0u8; SIGNATURE_BYTES];
        // SAFETY: 96-byte input and output buffers, one affine point.
        unsafe {
            if blst_p2_uncompress(&mut point, bytes.as_ptr()) != BLST_SUCCESS
                || blst_p2_affine_is_inf(&point)
                || !blst_p2_affine_in_g2(&point)
            {
                return None;
            }
            blst_p2_affine_compress(again.as_mut_ptr(), &point);
        }
        (again == *bytes).then_some(Self { point })
    }
}

fn core_verify(pk: &P1Affine, signature: &P2Affine, message: &[u8], dst: &[u8]) -> bool {
    // SAFETY: the points are valid; message and dst are slices with their
    // own lengths; there is no augmentation.
    unsafe {
        blst_core_verify_pk_in_g1(
            pk,
            signature,
            true,
            message.as_ptr(),
            message.len(),
            dst.as_ptr(),
            dst.len(),
            std::ptr::null(),
            0,
        ) == BLST_SUCCESS
    }
}

/// `bls::FastAggregateVerify`: the sum of `keys` signed the 32-byte `digest`.
/// The keys must be proof-of-possession checked by the caller. False for no
/// keys and for a sum at infinity.
pub fn fast_aggregate_verify(
    keys: &[&BlsPublicKey],
    digest: &[u8; 32],
    signature: &BlsSignature,
) -> bool {
    if keys.is_empty() {
        return false;
    }
    let points: Vec<*const P1Affine> = keys
        .iter()
        .map(|key| &key.point as *const P1Affine)
        .collect();
    let mut sum = P1([0; 18]);
    let mut aggregate = P1Affine([0; 12]);
    // SAFETY: `points` holds keys.len() valid pointers that outlive the call.
    unsafe {
        blst_p1s_add(&mut sum, points.as_ptr(), points.len());
        blst_p1_to_affine(&mut aggregate, &sum);
        if blst_p1_affine_is_inf(&aggregate) {
            return false;
        }
    }
    core_verify(&aggregate, &signature.point, digest, SIG_DST)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fm2::bytes::unhex;

    fn bytes<const N: usize>(text: &str) -> [u8; N] {
        unhex(text).unwrap().try_into().unwrap()
    }

    // BIP340 test vector 0 (bip-0340/test-vectors.csv).
    const SECRET_0: &str = "0000000000000000000000000000000000000000000000000000000000000003";
    const PUBLIC_0: &str = "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9";
    const MESSAGE_0: &str = "0000000000000000000000000000000000000000000000000000000000000000";
    const SIGNATURE_0: &str = "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca821525f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0";

    #[test]
    fn the_public_key_is_the_bip340_vector() {
        let key = SecretKey::from_bytes(&bytes(SECRET_0)).unwrap();
        assert_eq!(key.public(), bytes::<32>(PUBLIC_0));
    }

    #[test]
    fn the_bip340_vector_verifies_and_a_changed_one_does_not() {
        let public: [u8; 32] = bytes(PUBLIC_0);
        let message: [u8; 32] = bytes(MESSAGE_0);
        let signature: [u8; 64] = bytes(SIGNATURE_0);
        assert!(schnorr_verify(&public, &message, &signature));
        let mut other = message;
        other[0] ^= 1;
        assert!(!schnorr_verify(&public, &other, &signature));
        let mut bad = signature;
        bad[63] ^= 1;
        assert!(!schnorr_verify(&public, &message, &bad));
    }

    #[test]
    fn a_fresh_signature_verifies_under_its_own_key_only() {
        let (_, key) = SecretKey::generate();
        let (_, other) = SecretKey::generate();
        let message = [7u8; 32];
        let signature = key.sign(&message);
        assert!(schnorr_verify(&key.public(), &message, &signature));
        assert!(!schnorr_verify(&other.public(), &message, &signature));
    }

    #[test]
    fn zero_and_the_group_order_are_not_secret_keys() {
        assert!(SecretKey::from_bytes(&[0u8; 32]).is_none());
        let order: [u8; 32] =
            bytes("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141");
        assert!(SecretKey::from_bytes(&order).is_none());
    }

    // A seat of a regtest genesis document written by fm2-keygen: its key
    // and its proof of possession.
    const SEAT_KEY: &str = "a72219c07e6702c27c58e6edc32c819e55e545029126c8128054956792bd2a9964b6c7882ea1e0d658bec24a6964bb42";
    const SEAT_POP: &str = "adbc648d629687bac8ac76e3e896e91cfd472d8fc53af149e73c00f5e45b42215924b8e6e34b1762e04167cd2cb2612d091795fa8e17f0c5a72144f3009ef307094ea690da00e1e31d1082359f3a541ecebbcb49975c2519666714a611b2b0de";

    #[test]
    fn a_seat_key_from_fm2_keygen_proves_possession() {
        let key = BlsPublicKey::decode(&bytes(SEAT_KEY)).unwrap();
        let pop = BlsSignature::decode(&bytes(SEAT_POP)).unwrap();
        assert!(key.pop_verifies(&pop));
    }

    #[test]
    fn a_changed_proof_of_possession_is_refused() {
        let key = BlsPublicKey::decode(&bytes(SEAT_KEY)).unwrap();
        let mut pop: [u8; SIGNATURE_BYTES] = bytes(SEAT_POP);
        pop[95] ^= 1;
        // Either the point no longer decodes or the pairing check fails.
        assert!(!BlsSignature::decode(&pop).is_some_and(|pop| key.pop_verifies(&pop)));
    }

    #[test]
    fn infinity_and_garbage_are_not_points() {
        // The compressed point at infinity of G1 and of G2.
        let mut inf1 = [0u8; PUBKEY_BYTES];
        inf1[0] = 0xc0;
        assert!(BlsPublicKey::decode(&inf1).is_none());
        let mut inf2 = [0u8; SIGNATURE_BYTES];
        inf2[0] = 0xc0;
        assert!(BlsSignature::decode(&inf2).is_none());
        assert!(BlsPublicKey::decode(&[0xff; PUBKEY_BYTES]).is_none());
        assert!(BlsSignature::decode(&[0xff; SIGNATURE_BYTES]).is_none());
    }

    #[test]
    fn an_aggregate_needs_keys() {
        let pop = BlsSignature::decode(&bytes(SEAT_POP)).unwrap();
        assert!(!fast_aggregate_verify(&[], &[0u8; 32], &pop));
    }
}
