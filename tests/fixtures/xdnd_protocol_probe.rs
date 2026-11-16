//! Real-X-server protocol adversarial fixture, using the independent source peer.
#[path = "xdnd_peer.rs"]
mod worker;
use anyhow::{bail, Result};
use std::{
    collections::BTreeMap,
    sync::Arc,
    time::{Duration, Instant},
};
use x11rb::{
    connection::Connection,
    protocol::{xproto::*, Event},
    wrapper::ConnectionExt as _,
    COPY_DEPTH_FROM_PARENT, CURRENT_TIME, NONE,
};
fn atom(bus: &impl Connection, name: &str) -> Result<Atom> {
    Ok(bus.intern_atom(false, name.as_bytes())?.reply()?.atom)
}
fn send(
    bus: &impl Connection,
    destination: Window,
    event_window: Window,
    kind: Atom,
    data: [u32; 5],
) -> Result<()> {
    bus.send_event(
        false,
        destination,
        EventMask::NO_EVENT,
        ClientMessageEvent::new(32, event_window, kind, data),
    )?;
    bus.flush()?;
    Ok(())
}
fn window(bus: &impl Connection, aware: Option<u32>) -> Result<Window> {
    let w = bus.generate_id()?;
    bus.create_window(
        COPY_DEPTH_FROM_PARENT,
        w,
        bus.setup().roots[0].root,
        100,
        100,
        300,
        200,
        0,
        WindowClass::INPUT_OUTPUT,
        0,
        &CreateWindowAux::new()
            .event_mask(EventMask::PROPERTY_CHANGE | EventMask::STRUCTURE_NOTIFY),
    )?
    .check()?;
    if let Some(v) = aware {
        bus.change_property32(
            PropMode::REPLACE,
            w,
            atom(bus, "XdndAware")?,
            AtomEnum::ATOM,
            &[v],
        )?;
    }
    bus.map_window(w)?;
    bus.flush()?;
    Ok(w)
}
fn main() -> Result<()> {
    let display = std::env::args().nth(1).unwrap().parse()?;
    let (bus, _) = x11rb::connect(Some(&format!(":{display}")))?;
    let source = worker::Xdnd::new(display);
    let until = Instant::now() + Duration::from_secs(3);
    while !source.available() {
        assert!(Instant::now() < until);
        std::thread::sleep(Duration::from_millis(5));
    }
    let enter = atom(&bus, "XdndEnter")?;
    let position = atom(&bus, "XdndPosition")?;
    let status = atom(&bus, "XdndStatus")?;
    let drop_atom = atom(&bus, "XdndDrop")?;
    let finished = atom(&bus, "XdndFinished")?;
    let selection = atom(&bus, "XdndSelection")?;
    let copy = atom(&bus, "XdndActionCopy")?;
    let move_action = atom(&bus, "XdndActionMove")?;
    let data_atom = atom(&bus, "_WAYBRIDGE_XDND_TEST_DATA")?;
    let utf8 = atom(&bus, "UTF8_STRING")?;
    let incr = atom(&bus, "INCR")?;
    let clipboard = atom(&bus, "CLIPBOARD")?;
    let clipboard_owner = window(&bus, None)?;
    bus.set_selection_owner(clipboard_owner, clipboard, CURRENT_TIME)?
        .check()?;
    for (index, (case, version, proxy, refuse, destroy, large)) in [
        ("v5-direct", 5, false, false, false, false),
        ("v3-finished", 3, false, false, false, false),
        ("proxy-incr", 5, true, false, false, true),
        ("refuse-move", 5, false, true, false, false),
        ("target-destroy", 5, false, false, true, false),
        ("stale-generation", 5, false, false, false, false),
        ("coalesced-motion", 5, false, false, false, false),
        ("text-polymorphic", 5, false, false, false, false),
        ("latin1-plain", 5, false, false, false, false),
        ("opaque-plain", 5, false, false, false, false),
        ("cancel-incr", 5, false, false, false, true),
    ]
    .into_iter()
    .enumerate()
    {
        let target = window(&bus, Some(version))?;
        let recipient = if proxy {
            let p = window(&bus, Some(version))?;
            let pa = atom(&bus, "XdndProxy")?;
            bus.change_property32(PropMode::REPLACE, target, pa, AtomEnum::WINDOW, &[p])?;
            bus.change_property32(PropMode::REPLACE, p, pa, AtomEnum::WINDOW, &[p])?;
            p
        } else {
            target
        };
        bus.flush()?;
        let expected = if large {
            "data世界0123456789".repeat(30000).into_bytes()
        } else {
            b"copy-data".to_vec()
        };
        let original = if case == "latin1-plain" {
            "copy café".as_bytes().to_vec()
        } else if case == "opaque-plain" {
            b"copy caf\xe9".to_vec()
        } else {
            expected.clone()
        };
        let expected = if case == "latin1-plain" {
            b"copy caf\xe9".to_vec()
        } else if case == "opaque-plain" {
            original.clone()
        } else {
            expected
        };
        let requested = if case == "text-polymorphic" {
            atom(&bus, "TEXT")?
        } else if case.ends_with("-plain") {
            atom(&bus, "text/plain")?
        } else {
            utf8
        };
        let returned_type = if requested == atom(&bus, "TEXT")? {
            utf8
        } else {
            requested
        };
        let initial_generation = index as u64 * 4 + 1;
        let mut generation = initial_generation;
        assert!(source.begin(
            generation,
            target,
            (150, 150),
            Arc::new(BTreeMap::from([(
                if case == "opaque-plain" {
                    "text/plain".into()
                } else {
                    "text/plain;charset=utf-8".into()
                },
                original
            )]))
        ));
        if case != "coalesced-motion" {
            source.drop(generation);
        }
        let mut source_xid = 0;
        let mut old_source = 0;
        let mut got_drop = false;
        let mut incremental = false;
        let mut received = Vec::new();
        let mut complete = false;
        let mut motion_sent = false;
        let mut last_position = 0;
        let deadline = Instant::now() + Duration::from_secs(8);
        while !complete {
            if Instant::now() > deadline {
                bail!(
                    "{case}: deadline source={source_xid} drop={got_drop} data={}",
                    received.len()
                );
            }
            while let Some(event) = bus.poll_for_event()? {
                match event {
                    Event::ClientMessage(event) if event.type_ == enter => {
                        assert_eq!(
                            event.window, target,
                            "proxy must preserve logical target identity"
                        );
                        source_xid = event.data.as_data32()[0];
                        assert_eq!(event.data.as_data32()[1] >> 24, version);
                        if generation == initial_generation + 1 {
                            assert_ne!(source_xid, old_source);
                            send(
                                &bus,
                                source_xid,
                                old_source,
                                finished,
                                [target, 1, copy, 0, 0],
                            )?;
                        }
                        if destroy {
                            bus.destroy_window(target)?.check()?;
                        }
                    }
                    Event::ClientMessage(event) if event.type_ == position => {
                        let data = event.data.as_data32();
                        assert_eq!(data[0], source_xid);
                        assert_ne!(data[3], 0);
                        last_position = data[2];
                        if case == "coalesced-motion" && !motion_sent {
                            for i in 0..10000 {
                                source.motion(generation, target, (160 + i % 40, 160 + i % 40));
                            }
                            source.motion(generation, target, (199, 199));
                            motion_sent = true;
                            source.drop(generation);
                        }
                        // An unrelated target's status must never authorize a drop.
                        send(
                            &bus,
                            source_xid,
                            source_xid,
                            status,
                            [clipboard_owner, 1, 0, 0, copy],
                        )?;
                        send(
                            &bus,
                            source_xid,
                            source_xid,
                            status,
                            [target, 3, 0, 0, if refuse { move_action } else { copy }],
                        )?;
                    }
                    Event::ClientMessage(event) if event.type_ == drop_atom => {
                        got_drop = true;
                        assert!(!refuse);
                        if case == "coalesced-motion" {
                            assert_eq!(last_position, (199 << 16) | 199);
                        }
                        if case == "stale-generation" && generation == initial_generation {
                            old_source = source_xid;
                            source.cancel(generation);
                            generation += 1;
                            got_drop = false;
                            received.clear();
                            incremental = false;
                            assert!(source.begin(
                                generation,
                                target,
                                (150, 150),
                                Arc::new(BTreeMap::from([(
                                    "text/plain;charset=utf-8".into(),
                                    expected.clone()
                                )]))
                            ));
                            source.drop(generation);
                        } else {
                            bus.convert_selection(
                                target,
                                selection,
                                requested,
                                data_atom,
                                event.data.as_data32()[2],
                            )?;
                            bus.flush()?;
                        }
                    }
                    Event::SelectionNotify(event) if event.selection == selection => {
                        assert_ne!(event.property, NONE);
                        let reply = bus
                            .get_property(true, target, data_atom, AtomEnum::ANY, 0, 2_000_000)?
                            .reply()?;
                        if reply.type_ == incr {
                            incremental = true;
                            assert!(large);
                            assert_eq!(
                                reply.value32().unwrap().next(),
                                Some(expected.len() as u32)
                            );
                        } else {
                            assert_eq!(reply.type_, returned_type);
                            received = reply.value;
                            assert_eq!(received, expected);
                            send(
                                &bus,
                                source_xid,
                                source_xid,
                                finished,
                                [
                                    target,
                                    if version == 5 { 1 } else { 0 },
                                    if version == 5 { copy } else { 0 },
                                    0,
                                    0,
                                ],
                            )?;
                        }
                        bus.flush()?;
                    }
                    Event::PropertyNotify(event)
                        if incremental
                            && event.window == target
                            && event.atom == data_atom
                            && event.state == Property::NEW_VALUE =>
                    {
                        let reply = bus
                            .get_property(
                                case != "cancel-incr",
                                target,
                                data_atom,
                                AtomEnum::ANY,
                                0,
                                2_000_000,
                            )?
                            .reply()?;
                        assert_eq!(reply.type_, returned_type);
                        if reply.value.is_empty() {
                            assert_eq!(received, expected);
                            send(
                                &bus,
                                source_xid,
                                source_xid,
                                finished,
                                [target, 1, copy, 0, 0],
                            )?;
                        } else {
                            received.extend(reply.value);
                            assert!(received.len() <= expected.len());
                            if case == "cancel-incr" {
                                assert_eq!(received.len(), 32 * 1024);
                                source.cancel(generation);
                                bus.delete_property(target, data_atom)?;
                            }
                        }
                        bus.flush()?;
                    }
                    _ => (),
                }
            }
            for outcome in source.poll() {
                if outcome.generation != generation {
                    continue;
                }
                assert_eq!(outcome.target, target);
                match outcome.kind {
                    worker::OutcomeKind::Finished(true) => {
                        assert!(got_drop);
                        assert_eq!(received, expected);
                        complete = true;
                    }
                    worker::OutcomeKind::Error(error) if refuse || destroy => {
                        assert!(!got_drop);
                        assert!(
                            error.contains(if refuse {
                                "rejected COPY"
                            } else {
                                "disappeared"
                            }),
                            "{error}"
                        );
                        complete = true;
                    }
                    worker::OutcomeKind::Finished(false) if case == "cancel-incr" => {
                        assert!(got_drop);
                        assert_eq!(received.len(), 32 * 1024);
                        complete = true;
                    }
                    worker::OutcomeKind::Error(error) if case == "cancel-incr" => {
                        assert!(error.contains("superseded"), "{error}");
                        assert_eq!(received.len(), 32 * 1024);
                        complete = true;
                    }
                    worker::OutcomeKind::Finished(false) => bail!("{case}: unexpected failure"),
                    worker::OutcomeKind::Error(error) => bail!("{case}: {error}"),
                    _ => (),
                }
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        if case == "cancel-incr" {
            // GetProperty is an X-server barrier after cancellation/deletion:
            // the source must not refill the receiver property with another chunk.
            assert_eq!(
                bus.get_property(false, target, data_atom, AtomEnum::ANY, 0, 2_000_000)?
                    .reply()?
                    .type_,
                NONE
            );
        }
        assert_eq!(
            bus.get_selection_owner(clipboard)?.reply()?.owner,
            clipboard_owner
        );
        source.cancel_all();
        if !destroy {
            bus.destroy_window(target)?.check()?;
        }
        if proxy {
            bus.destroy_window(recipient)?.check()?;
        }
        bus.flush()?;
        println!("PASS {case}");
    }
    Ok(())
}
