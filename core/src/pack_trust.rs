//! Production trust policy for signed `.ay` packs.
//!
//! Private signing keys never belong in this module. Runtimes load an explicit
//! public-key registry and signatures identify the registry entry that must be
//! used for verification.

use std::collections::HashMap;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use ed25519_dalek::{Signature, Verifier, VerifyingKey};
use serde::Deserialize;

/// Stable identifier used by packs signed with the public RFC 8032 test key.
pub const DEVELOPMENT_KEY_ID: &str = "ayther-development-rfc8032";

/// Public half of the RFC 8032 development key used by authoring builds.
pub const DEVELOPMENT_PUBLIC_KEY: [u8; 32] = [
    0xad, 0x25, 0xd7, 0x0a, 0x95, 0xc2, 0xc0, 0x8d, 0x12, 0x0f, 0x43, 0x71, 0x28, 0x12, 0x53, 0xe9,
    0xfb, 0xe6, 0x07, 0x90, 0x67, 0x22, 0x30, 0xcb, 0xc2, 0x7a, 0x68, 0x7a, 0x27, 0x89, 0x42, 0x3b,
];

const SIGNATURE_MAGIC: &[u8; 8] = b"AYTHSIG\0";
const SIGNATURE_VERSION: u8 = 1;
const MAX_KEY_ID_BYTES: usize = 64;

/// A detached Ed25519 signature carrying the trusted-key identifier.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SignatureEnvelope {
    /// Registry identifier of the signing key.
    pub key_id: String,
    /// Detached Ed25519 signature bytes.
    pub signature: [u8; 64],
}

impl SignatureEnvelope {
    /// Encodes a versioned signature envelope for `signature.bin`.
    ///
    /// # Errors
    ///
    /// Returns [`TrustError::MalformedSignature`] when `key_id` is not a
    /// printable ASCII identifier between 1 and 64 bytes.
    pub fn encode(key_id: &str, signature: [u8; 64]) -> Result<Vec<u8>, TrustError> {
        validate_key_id(key_id).map_err(TrustError::MalformedSignature)?;
        let mut encoded = Vec::with_capacity(10 + key_id.len() + signature.len());
        encoded.extend_from_slice(SIGNATURE_MAGIC);
        encoded.push(SIGNATURE_VERSION);
        encoded.push(key_id.len() as u8);
        encoded.extend_from_slice(key_id.as_bytes());
        encoded.extend_from_slice(&signature);
        Ok(encoded)
    }

    /// Decodes a versioned `signature.bin` envelope.
    ///
    /// # Errors
    ///
    /// Returns [`TrustError::MalformedSignature`] for a bad magic value,
    /// unsupported version, invalid key identifier, or incorrect length.
    pub fn decode(encoded: &[u8]) -> Result<Self, TrustError> {
        if encoded.len() < SIGNATURE_MAGIC.len() + 2
            || &encoded[..SIGNATURE_MAGIC.len()] != SIGNATURE_MAGIC
        {
            return Err(TrustError::MalformedSignature(
                "missing AYTHER signature envelope magic".into(),
            ));
        }
        if encoded[SIGNATURE_MAGIC.len()] != SIGNATURE_VERSION {
            return Err(TrustError::MalformedSignature(format!(
                "unsupported signature envelope version {}",
                encoded[SIGNATURE_MAGIC.len()]
            )));
        }
        let key_len = encoded[SIGNATURE_MAGIC.len() + 1] as usize;
        let expected = SIGNATURE_MAGIC.len() + 2 + key_len + 64;
        if encoded.len() != expected {
            return Err(TrustError::MalformedSignature(
                "signature envelope has an invalid length".into(),
            ));
        }
        let key_start = SIGNATURE_MAGIC.len() + 2;
        let key_end = key_start + key_len;
        let key_id = std::str::from_utf8(&encoded[key_start..key_end])
            .map_err(|_| TrustError::MalformedSignature("key id is not UTF-8".into()))?;
        validate_key_id(key_id).map_err(TrustError::MalformedSignature)?;
        let signature = encoded[key_end..]
            .try_into()
            .map_err(|_| TrustError::MalformedSignature("signature is not 64 bytes".into()))?;
        Ok(Self {
            key_id: key_id.into(),
            signature,
        })
    }
}

/// A verified signer whose pack scope still needs to be authorized.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VerifiedSigner {
    key_id: String,
    games: Vec<String>,
}

impl VerifiedSigner {
    /// Identifier of the key that authenticated the pack.
    pub fn key_id(&self) -> &str {
        &self.key_id
    }

    /// Enforces the signer's declared game scope.
    ///
    /// # Errors
    ///
    /// Returns [`TrustError::GameNotAllowed`] when the registry entry does not
    /// include the pack's `game_id` or the wildcard scope.
    pub fn authorize_game(&self, game_id: &str) -> Result<(), TrustError> {
        if self.games.iter().any(|game| game == "*" || game == game_id) {
            Ok(())
        } else {
            Err(TrustError::GameNotAllowed {
                key_id: self.key_id.clone(),
                game_id: game_id.into(),
            })
        }
    }
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RawRegistry {
    version: u32,
    #[serde(default)]
    keys: Vec<RawTrustedKey>,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct RawTrustedKey {
    id: String,
    algorithm: String,
    public_key: String,
    not_before_unix: u64,
    not_after_unix: u64,
    #[serde(default)]
    revoked: bool,
    games: Vec<String>,
}

#[derive(Debug)]
struct TrustedKey {
    verifying_key: VerifyingKey,
    not_before_unix: u64,
    not_after_unix: u64,
    revoked: bool,
    games: Vec<String>,
}

/// Explicit public-key registry used by production pack verification.
#[derive(Debug, Default)]
pub struct TrustStore {
    keys: HashMap<String, TrustedKey>,
}

impl TrustStore {
    /// Parses and validates a version-1 TOML trust registry.
    ///
    /// # Errors
    ///
    /// Returns [`TrustError::MalformedRegistry`] for malformed fields,
    /// duplicate identifiers, unsupported algorithms, invalid validity ranges,
    /// empty scopes, or the known development test key.
    pub fn from_toml(input: &str) -> Result<Self, TrustError> {
        let raw: RawRegistry = toml::from_str(input)
            .map_err(|error| TrustError::MalformedRegistry(error.to_string()))?;
        if raw.version != 1 {
            return Err(TrustError::MalformedRegistry(format!(
                "unsupported trust registry version {}",
                raw.version
            )));
        }
        let mut keys = HashMap::with_capacity(raw.keys.len());
        for key in raw.keys {
            validate_key_id(&key.id).map_err(TrustError::MalformedRegistry)?;
            if key.algorithm != "ed25519" {
                return Err(TrustError::MalformedRegistry(format!(
                    "key '{}' uses unsupported algorithm '{}'",
                    key.id, key.algorithm
                )));
            }
            if key.not_before_unix > key.not_after_unix {
                return Err(TrustError::MalformedRegistry(format!(
                    "key '{}' has an inverted validity interval",
                    key.id
                )));
            }
            if key.games.is_empty() || key.games.iter().any(|game| !valid_game_scope(game)) {
                return Err(TrustError::MalformedRegistry(format!(
                    "key '{}' has an invalid or empty game scope",
                    key.id
                )));
            }
            let bytes = decode_hex_32(&key.public_key).ok_or_else(|| {
                TrustError::MalformedRegistry(format!(
                    "key '{}' does not contain a 32-byte lowercase hex public key",
                    key.id
                ))
            })?;
            if bytes == DEVELOPMENT_PUBLIC_KEY {
                return Err(TrustError::DevelopmentKeyForbidden);
            }
            let verifying_key = VerifyingKey::from_bytes(&bytes).map_err(|_| {
                TrustError::MalformedRegistry(format!("key '{}' is not valid Ed25519", key.id))
            })?;
            let id = key.id;
            let trusted = TrustedKey {
                verifying_key,
                not_before_unix: key.not_before_unix,
                not_after_unix: key.not_after_unix,
                revoked: key.revoked,
                games: key.games,
            };
            if keys.insert(id.clone(), trusted).is_some() {
                return Err(TrustError::MalformedRegistry(format!(
                    "duplicate key id '{id}'"
                )));
            }
        }
        Ok(Self { keys })
    }

    /// Loads a TOML trust registry from disk.
    ///
    /// # Errors
    ///
    /// Returns [`TrustError::RegistryIo`] on I/O failure and otherwise the same
    /// validation errors as [`Self::from_toml`].
    pub fn from_path(path: &Path) -> Result<Self, TrustError> {
        let input = std::fs::read_to_string(path)
            .map_err(|error| TrustError::RegistryIo(error.to_string()))?;
        Self::from_toml(&input)
    }

    /// Verifies a signature envelope using the current system time.
    ///
    /// The returned signer must subsequently authorize the authenticated
    /// manifest's `game_id` with [`VerifiedSigner::authorize_game`].
    ///
    /// # Errors
    ///
    /// Returns a precise [`TrustError`] for malformed, unknown, inactive,
    /// revoked, or cryptographically invalid signatures.
    pub fn verify(&self, message: &[u8], encoded: &[u8]) -> Result<VerifiedSigner, TrustError> {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|_| TrustError::ClockBeforeUnixEpoch)?
            .as_secs();
        self.verify_at(message, encoded, now)
    }

    pub(crate) fn verify_at(
        &self,
        message: &[u8],
        encoded: &[u8],
        now_unix: u64,
    ) -> Result<VerifiedSigner, TrustError> {
        let envelope = SignatureEnvelope::decode(encoded)?;
        let key = self
            .keys
            .get(&envelope.key_id)
            .ok_or_else(|| TrustError::UnknownKey(envelope.key_id.clone()))?;
        if key.revoked {
            return Err(TrustError::RevokedKey(envelope.key_id));
        }
        if now_unix < key.not_before_unix {
            return Err(TrustError::KeyNotYetValid(envelope.key_id));
        }
        if now_unix > key.not_after_unix {
            return Err(TrustError::ExpiredKey(envelope.key_id));
        }
        key.verifying_key
            .verify(message, &Signature::from_bytes(&envelope.signature))
            .map_err(|_| TrustError::BadSignature)?;
        Ok(VerifiedSigner {
            key_id: envelope.key_id,
            games: key.games.clone(),
        })
    }
}

/// Failure while loading or applying the production pack trust policy.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum TrustError {
    /// The registry file could not be read.
    RegistryIo(String),
    /// The registry schema or one of its entries is invalid.
    MalformedRegistry(String),
    /// The detached signature envelope is invalid.
    MalformedSignature(String),
    /// Production policy requires a detached signature.
    MissingSignature,
    /// The signature names a key that is not trusted.
    UnknownKey(String),
    /// The signing key has been revoked.
    RevokedKey(String),
    /// The signing key is not active yet.
    KeyNotYetValid(String),
    /// The signing key has expired.
    ExpiredKey(String),
    /// The key is not authorized for this pack identity.
    GameNotAllowed {
        /// Registry key identifier.
        key_id: String,
        /// Pack `game_id` rejected by the key scope.
        game_id: String,
    },
    /// The Ed25519 signature does not authenticate the signed bytes.
    BadSignature,
    /// The public RFC test key cannot be promoted into a production registry.
    DevelopmentKeyForbidden,
    /// The local clock predates the Unix epoch and cannot evaluate validity.
    ClockBeforeUnixEpoch,
}

impl std::fmt::Display for TrustError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::RegistryIo(message) => write!(formatter, "trust registry I/O error: {message}"),
            Self::MalformedRegistry(message) => {
                write!(formatter, "malformed trust registry: {message}")
            }
            Self::MalformedSignature(message) => {
                write!(formatter, "malformed signature: {message}")
            }
            Self::MissingSignature => formatter.write_str("production signature is missing"),
            Self::UnknownKey(key) => write!(formatter, "unknown signing key '{key}'"),
            Self::RevokedKey(key) => write!(formatter, "revoked signing key '{key}'"),
            Self::KeyNotYetValid(key) => write!(formatter, "signing key '{key}' is not valid yet"),
            Self::ExpiredKey(key) => write!(formatter, "signing key '{key}' has expired"),
            Self::GameNotAllowed { key_id, game_id } => {
                write!(
                    formatter,
                    "signing key '{key_id}' is not authorized for game '{game_id}'"
                )
            }
            Self::BadSignature => formatter.write_str("Ed25519 signature verification failed"),
            Self::DevelopmentKeyForbidden => {
                formatter.write_str("the public development key cannot be trusted in production")
            }
            Self::ClockBeforeUnixEpoch => {
                formatter.write_str("system clock predates the Unix epoch")
            }
        }
    }
}

impl std::error::Error for TrustError {}

fn validate_key_id(key_id: &str) -> Result<(), String> {
    if key_id.is_empty()
        || key_id.len() > MAX_KEY_ID_BYTES
        || !key_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'))
    {
        return Err("key id must contain 1-64 ASCII letters, digits, '.', '_' or '-'".into());
    }
    Ok(())
}

fn valid_game_scope(game: &str) -> bool {
    game == "*"
        || (!game.is_empty()
            && game
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-')))
}

fn decode_hex_32(value: &str) -> Option<[u8; 32]> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return None;
    }
    let mut decoded = [0_u8; 32];
    for (index, pair) in value.as_bytes().chunks_exact(2).enumerate() {
        let hex = std::str::from_utf8(pair).ok()?;
        decoded[index] = u8::from_str_radix(hex, 16).ok()?;
    }
    Some(decoded)
}

#[cfg(test)]
mod tests {
    use ed25519_dalek::{Signer, SigningKey};

    use super::*;

    const PROD_SEED: [u8; 32] = [7; 32];

    fn registry(revoked: bool, games: &str) -> String {
        let public = SigningKey::from_bytes(&PROD_SEED)
            .verifying_key()
            .to_bytes();
        format!(
            "version = 1\n\n[[keys]]\n\
             id = \"hub-2026-01\"\n\
             algorithm = \"ed25519\"\n\
             public_key = \"{}\"\n\
             not_before_unix = 100\n\
             not_after_unix = 200\n\
             revoked = {revoked}\n\
             games = [{games}]\n",
            public
                .iter()
                .map(|byte| format!("{byte:02x}"))
                .collect::<String>()
        )
    }

    fn signed(message: &[u8]) -> Vec<u8> {
        let signature = SigningKey::from_bytes(&PROD_SEED).sign(message).to_bytes();
        SignatureEnvelope::encode("hub-2026-01", signature).expect("valid test envelope")
    }

    #[test]
    fn active_key_verifies_and_authorizes_declared_game() {
        let store = TrustStore::from_toml(&registry(false, "\"sonic2\""))
            .expect("valid production registry");
        let signer = store
            .verify_at(b"integrity", &signed(b"integrity"), 150)
            .expect("active signature");

        assert_eq!(signer.authorize_game("sonic2"), Ok(()));
    }

    #[test]
    fn revoked_key_is_rejected_before_signature_use() {
        let store =
            TrustStore::from_toml(&registry(true, "\"*\"")).expect("valid revoked registry entry");

        assert_eq!(
            store.verify_at(b"integrity", &signed(b"integrity"), 150),
            Err(TrustError::RevokedKey("hub-2026-01".into()))
        );
    }

    #[test]
    fn expired_key_is_rejected() {
        let store =
            TrustStore::from_toml(&registry(false, "\"*\"")).expect("valid production registry");

        assert_eq!(
            store.verify_at(b"integrity", &signed(b"integrity"), 201),
            Err(TrustError::ExpiredKey("hub-2026-01".into()))
        );
    }

    #[test]
    fn key_before_its_activation_is_rejected() {
        let store =
            TrustStore::from_toml(&registry(false, "\"*\"")).expect("valid production registry");

        assert_eq!(
            store.verify_at(b"integrity", &signed(b"integrity"), 99),
            Err(TrustError::KeyNotYetValid("hub-2026-01".into()))
        );
    }

    #[test]
    fn unknown_key_is_rejected() {
        let store = TrustStore::from_toml("version = 1\n").expect("empty registry is valid");

        assert_eq!(
            store.verify_at(b"integrity", &signed(b"integrity"), 150),
            Err(TrustError::UnknownKey("hub-2026-01".into()))
        );
    }

    #[test]
    fn game_outside_key_scope_is_rejected() {
        let store = TrustStore::from_toml(&registry(false, "\"sonic2\""))
            .expect("valid production registry");
        let signer = store
            .verify_at(b"integrity", &signed(b"integrity"), 150)
            .expect("active signature");

        assert_eq!(
            signer.authorize_game("streets_of_rage"),
            Err(TrustError::GameNotAllowed {
                key_id: "hub-2026-01".into(),
                game_id: "streets_of_rage".into(),
            })
        );
    }

    #[test]
    fn development_key_cannot_enter_production_registry() {
        let public = DEVELOPMENT_PUBLIC_KEY
            .iter()
            .map(|byte| format!("{byte:02x}"))
            .collect::<String>();
        let input = format!(
            "version = 1\n[[keys]]\nid = \"forbidden\"\nalgorithm = \"ed25519\"\n\
             public_key = \"{public}\"\nnot_before_unix = 0\nnot_after_unix = 1\n\
             games = [\"*\"]\n"
        );

        assert!(matches!(
            TrustStore::from_toml(&input),
            Err(TrustError::DevelopmentKeyForbidden)
        ));
    }

    #[test]
    fn tampered_message_is_rejected() {
        let store =
            TrustStore::from_toml(&registry(false, "\"*\"")).expect("valid production registry");

        assert_eq!(
            store.verify_at(b"tampered", &signed(b"integrity"), 150),
            Err(TrustError::BadSignature)
        );
    }
}
