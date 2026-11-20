//! The FlowMesh V2 client (Josh's stage 2 core, src/flowmesh2): signed
//! actions over pinned HTTPS to every seat, and proof checks of everything a
//! seat answers, in the trusted core. See hive/README.md.

pub mod bytes;
pub mod crypto;
pub mod hash;

/// Why an operation of the client failed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    /// Bytes that are not the expected encoding.
    Decode(&'static str),
    /// Bytes or a count above a protocol cap.
    Oversize(&'static str),
    /// A proof, certificate or signature that does not hold.
    Verify(&'static str),
    /// A value the caller gave that cannot be used.
    Invalid(String),
    /// A seat could not be reached or answered outside the protocol.
    Transport(String),
    /// Local storage failed.
    Storage(String),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Decode(what) => write!(f, "malformed data: {what}"),
            Error::Oversize(what) => write!(f, "data above a protocol limit: {what}"),
            Error::Verify(what) => write!(f, "verification failed: {what}"),
            Error::Invalid(what) | Error::Transport(what) | Error::Storage(what) => {
                f.write_str(what)
            }
        }
    }
}

impl From<Error> for String {
    fn from(error: Error) -> Self {
        error.to_string()
    }
}
