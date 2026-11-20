//! Shared MIME selection for the clipboard bridge.
use base64::{engine::general_purpose::STANDARD, Engine as _};
use serde_json::Value;
use std::collections::BTreeMap;

pub const MAX_BYTES: usize = 12 * 1024 * 1024;
const FORMATS: &[&[&str]] = &[
    &[
        "text/plain;charset=utf-8",
        "text/plain; charset=utf-8",
        "text/plain",
    ],
    &["image/png"],
    &[
        "text/html",
        "text/html;charset=utf-8",
        "text/html; charset=utf-8",
    ],
    &["text/rtf", "application/rtf", "application/x-rtf"],
];

pub fn read_types(types: &[String]) -> Vec<String> {
    let mut selected: Vec<_> = FORMATS
        .iter()
        .copied()
        .chain(std::iter::once(&["text/uri-list"][..]))
        .filter_map(|group| {
            group
                .iter()
                .find_map(|mime| types.iter().find(|ty| ty.eq_ignore_ascii_case(mime)))
                .cloned()
        })
        .collect();
    // Preserve text/plain when a producer also advertises a UTF-8 alias.
    if let Some(plain) = types
        .iter()
        .find(|ty| ty.eq_ignore_ascii_case("text/plain"))
    {
        if !selected.contains(plain) {
            selected.push(plain.clone());
        }
    }
    selected
}

pub fn decode(items: &Value) -> BTreeMap<String, Vec<u8>> {
    let Some(items) = items.as_object() else {
        return BTreeMap::new();
    };
    FORMATS
        .iter()
        .filter_map(|group| {
            group.iter().find_map(|mime| {
                let value = items.get(*mime)?.as_str()?;
                if value.len() > MAX_BYTES.div_ceil(3) * 4 {
                    return None;
                }
                let bytes = STANDARD.decode(value).ok()?;
                (bytes.len() <= MAX_BYTES).then(|| ((*mime).to_string(), bytes))
            })
        })
        .collect()
}

pub fn encode(contents: &BTreeMap<String, Vec<u8>>) -> BTreeMap<&'static str, String> {
    // Only one representation from each group is sent. Even when all four
    // are present at their per-format limit, a snapshot fits MAX_PACKET.
    FORMATS
        .iter()
        .filter_map(|group| {
            group.iter().find_map(|mime| {
                let bytes = contents
                    .iter()
                    .find(|(key, _)| key.eq_ignore_ascii_case(mime))?
                    .1;
                let outbound = if group[0] == "text/plain;charset=utf-8" && *mime == "text/plain" {
                    "text/plain"
                } else {
                    group[0]
                };
                Some((outbound, STANDARD.encode(bytes)))
            })
        })
        .collect()
}

pub fn local_types(contents: &BTreeMap<String, Vec<u8>>) -> Vec<String> {
    let mut types: Vec<_> = contents.keys().cloned().collect();
    for group in FORMATS {
        if lookup(contents, group[0]).is_some() {
            types.extend(
                group
                    .iter()
                    .filter(|mime| !contents.contains_key(**mime))
                    .map(|mime| (*mime).into()),
            );
        }
    }
    types
}

pub fn lookup<'a>(contents: &'a BTreeMap<String, Vec<u8>>, mime: &str) -> Option<&'a Vec<u8>> {
    contents.get(mime).or_else(|| {
        let group = FORMATS.iter().find(|group| group.contains(&mime))?;
        group.iter().find_map(|alias| {
            contents
                .iter()
                .find(|(key, _)| key.eq_ignore_ascii_case(alias))
                .map(|(_, data)| data)
        })
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn one_source_per_representation_and_rtf_aliases() {
        let types = [
            "text/plain",
            "TEXT/HTML",
            "text/html; charset=utf-8",
            "application/rtf",
            "text/uri-list",
        ]
        .map(str::to_string);
        assert_eq!(
            read_types(&types),
            [
                "text/plain",
                "TEXT/HTML",
                "application/rtf",
                "text/uri-list"
            ]
        );
        let original = BTreeMap::from([
            ("text/plain".into(), b"plain".to_vec()),
            ("TEXT/HTML".into(), b"<b>bold</b>".to_vec()),
            ("application/rtf".into(), br"{\rtf1 plain}".to_vec()),
            (
                "text/uri-list".into(),
                b"file:///not-a-wire-format".to_vec(),
            ),
        ]);
        let wire = serde_json::to_value(encode(&original)).unwrap();
        assert_eq!(wire.as_object().unwrap().len(), 3);
        let received = decode(&wire);
        assert_eq!(
            lookup(&received, "application/rtf"),
            original.get("application/rtf")
        );
        assert!(local_types(&received).contains(&"application/rtf".into()));
        assert_eq!(received["text/html"], b"<b>bold</b>");
    }
}
