//! Session-owned desktop services behind the existing keyring module boundary.
#[path = "session_services.rs"]
mod session_services;

pub use session_services::{connect_private_bus, Services};
