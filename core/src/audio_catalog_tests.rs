use crate::audio_event::{
    CatalogObservation as Event, CatalogReason, events_from_toml, events_from_toml_observed,
};

#[test]
fn catalog_observes_declared_entries_before_filtering_and_preserves_values() {
    let text = "event = [\
        {signature='0x1',asset='first'},\
        {signature='0X01',asset='second'},\
        {signature='oops',asset='bad'},\
        {signature='2'},7,{signature='0',asset=''}]";
    let mut stages = Vec::new();
    let mut declared = Vec::new();
    let observed = events_from_toml_observed(text, |event| match event {
        Event::Begin { entries } => stages.push((1, entries)),
        Event::Declared {
            ordinal,
            signature_text,
            signature,
            asset,
        } => {
            stages.push((2, ordinal));
            declared.push((
                ordinal,
                signature_text.map(str::to_owned),
                signature,
                asset.map(str::to_owned),
            ));
        }
        Event::Accepted { ordinal, .. } => stages.push((3, ordinal)),
        Event::Rejected { ordinal, .. } => stages.push((4, ordinal)),
        Event::End { accepted } => stages.push((5, accepted)),
        Event::Unavailable { .. } => panic!("inventory unexpectedly unavailable"),
    });
    assert_eq!(observed, events_from_toml(text));
    assert_eq!(
        observed
            .iter()
            .map(|s| (s.signature, s.asset.as_str()))
            .collect::<Vec<_>>(),
        vec![(1, "first"), (1, "second"), (0, "")]
    );
    assert_eq!(
        stages,
        vec![
            (1, 6),
            (2, 0),
            (3, 0),
            (2, 1),
            (3, 1),
            (2, 2),
            (4, 2),
            (2, 3),
            (4, 3),
            (2, 4),
            (4, 4),
            (2, 5),
            (3, 5),
            (5, 3)
        ]
    );
    assert_eq!(declared.len(), 6);
    assert_eq!(
        declared[0].2, declared[1].2,
        "aliases preserve the parser's canonical identity"
    );
    assert_eq!(declared[2].1.as_deref(), Some("oops"));
    assert_eq!(declared[2].2, None);
    assert_eq!(declared[3].3, None);
    assert_eq!(declared[4], (4, None, None, None));
}

#[test]
fn catalog_distinguishes_empty_inventory_from_unavailable_inventory() {
    for (text, expected) in [
        (
            "event=[]",
            vec![Event::Begin { entries: 0 }, Event::End { accepted: 0 }],
        ),
        (
            "title='empty'",
            vec![Event::Unavailable {
                reason: CatalogReason::MissingEventArray,
            }],
        ),
        (
            "[[event",
            vec![Event::Unavailable {
                reason: CatalogReason::InvalidSyntax,
            }],
        ),
        (
            "event='wrong type'",
            vec![Event::Unavailable {
                reason: CatalogReason::MissingEventArray,
            }],
        ),
    ] {
        let mut actual = Vec::new();
        let output = events_from_toml_observed(text, |event| {
            // These cases never contain borrowed declaration fields.
            actual.push(match event {
                Event::Begin { entries } => Event::Begin { entries },
                Event::End { accepted } => Event::End { accepted },
                Event::Unavailable { reason } => Event::Unavailable { reason },
                _ => panic!("unexpected entry"),
            });
        });
        assert!(output.is_empty());
        assert_eq!(actual, expected);
    }
}

#[test]
fn catalog_reports_actual_rejection_branches_when_all_entries_are_skipped() {
    let mut reasons = Vec::new();
    let mut end = None;
    let output = events_from_toml_observed(
        "event=[{signature='bad!',asset='x'},{signature='1'},{signature=7,asset='x'}]",
        |event| match event {
            Event::Rejected { ordinal, reason } => reasons.push((ordinal, reason)),
            Event::End { accepted } => end = Some(accepted),
            _ => {}
        },
    );
    assert!(output.is_empty());
    assert_eq!(end, Some(0));
    assert_eq!(
        reasons,
        vec![
            (0, CatalogReason::InvalidSignature),
            (1, CatalogReason::MissingAsset),
            (2, CatalogReason::InvalidSignature)
        ]
    );
}

#[test]
fn catalog_observation_c_layout_is_fixed_on_supported_64_bit_targets() {
    use crate::AytherAudioCatalogObservation as View;
    assert_eq!(std::mem::size_of::<View>(), 72);
    assert_eq!(std::mem::align_of::<View>(), 8);
    assert_eq!(std::mem::offset_of!(View, signature_text), 40);
    assert_eq!(std::mem::offset_of!(View, asset_bytes), 64);
}
