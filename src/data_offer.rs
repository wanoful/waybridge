//! Immutable MIME representations, independent of clipboard and input state.
use anyhow::{bail, Result};
use std::{collections::BTreeMap, sync::Arc};
pub type Payload = Arc<BTreeMap<String, Vec<u8>>>;
#[derive(Clone)]
pub struct Offer {
    payload: Payload,
}
impl Offer {
    pub fn text(bytes: Vec<u8>) -> Result<Self> {
        if bytes.len() > crate::drag::MAX_TEXT
            || bytes.contains(&0)
            || std::str::from_utf8(&bytes).is_err()
        {
            bail!("unsupported text encoding or size");
        }
        Ok(Self {
            payload: Arc::new(BTreeMap::from([
                ("text/plain;charset=utf-8".into(), bytes.clone()),
                ("text/plain".into(), bytes),
            ])),
        })
    }
    pub fn one(mime: &str, bytes: Vec<u8>) -> Self {
        Self {
            payload: Arc::new(BTreeMap::from([(mime.into(), bytes)])),
        }
    }
    pub fn mime_types(&self) -> Vec<String> {
        self.payload.keys().cloned().collect()
    }
    pub fn snapshot(&self) -> Payload {
        self.payload.clone()
    }
}
