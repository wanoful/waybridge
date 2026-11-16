//! Drives the independent XDND conformance peer against an actual toolkit target.
#[path = "xdnd_peer.rs"]
mod xdnd;
use anyhow::{bail, Result};
use std::{
    collections::BTreeMap,
    sync::Arc,
    time::{Duration, Instant},
};
fn main() -> Result<()> {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 8 {
        bail!("usage: probe DISPLAY_NUMBER XID ROOT_X ROOT_Y MIME PAYLOAD_FILE MODE");
    }
    let source = xdnd::Xdnd::new(args[1].parse()?);
    let target = args[2].parse()?;
    let position = (args[3].parse()?, args[4].parse()?);
    let payload = Arc::new(BTreeMap::from([(
        args[5].clone(),
        std::fs::read(&args[6])?,
    )]));
    let deadline = Instant::now() + Duration::from_secs(4);
    while !source.available() {
        if Instant::now() > deadline {
            bail!("XDND worker startup failed");
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    if !source.begin(1, target, position, payload) {
        bail!("XDND begin refused");
    }
    source.motion(1, target, position);
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        for event in source.poll() {
            println!(
                "generation={} target={} kind={:?}",
                event.generation, event.target, event.kind
            );
            match event.kind {
                xdnd::OutcomeKind::Accepted(true) => source.drop(1),
                xdnd::OutcomeKind::Finished(true) => {
                    source.cancel_all();
                    return Ok(());
                }
                xdnd::OutcomeKind::Finished(false) => bail!("drop failed"),
                xdnd::OutcomeKind::Error(error) => bail!(error),
                _ => (),
            }
        }
        if Instant::now() > deadline {
            source.cancel(1);
            bail!("XDND target completion timeout");
        }
        std::thread::sleep(Duration::from_millis(5));
    }
}
