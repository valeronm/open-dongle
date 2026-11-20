use diagwire::*;

fn fixture(name: &str) -> Vec<u8> {
    let path = format!("{}/../tool/fixtures/{name}", env!("CARGO_MANIFEST_DIR"));
    std::fs::read_to_string(path)
        .unwrap()
        .split_whitespace()
        .map(|b| u8::from_str_radix(b, 16).unwrap())
        .collect()
}

fn numbers<T: Copy + Into<u8>>(all: &[T]) -> Vec<u8> {
    all.iter().map(|&v| v.into()).collect()
}

#[test]
fn numbers_match_fixtures() {
    assert_eq!(numbers(&Command::ALL), fixture("commands.hex"));
    assert_eq!(numbers(&ErrorCode::ALL), fixture("error-codes.hex"));
    assert_eq!(numbers(&EventType::ALL), fixture("event-types.hex"));
    assert_eq!(numbers(&Divert::ALL), fixture("divert-outcomes.hex"));
    assert_eq!(numbers(&Wheel::ALL), fixture("wheel-outcomes.hex"));
    assert_eq!(
        [BUSY_DBM as u8, STRONG_DBM as u8].to_vec(),
        fixture("noise-limits.hex")
    );
}

#[test]
fn a_request_carries_the_slot_after_its_arg() {
    let want = fixture("request.hex");
    let req = parse_request(&request(Command::History, 9, 1440, &[1])).unwrap();
    assert_eq!(
        (req.command, req.seq, req.arg, req.data[0]),
        (Command::History as u8, 9, 1440, 1)
    );
    assert_eq!(request(Command::History, 9, 1440, &[1])[..want.len()], want);
    assert_eq!(parse_request(&want[..REQUEST_LEN - 1]), None);
}

#[test]
fn a_request_carries_the_bytes_after_arg_padded_with_zeros() {
    let mut buf = [0; REPORT_LEN];
    buf[REQUEST_LEN..REQUEST_LEN + 2].copy_from_slice(&[1, 2]);
    let req = parse_request(&buf[..REQUEST_LEN + 2]).unwrap();
    assert_eq!(req.data, [1, 2, 0, 0, 0, 0, 0]);
}

#[test]
fn a_76_byte_payload_goes_out_in_two_parts() {
    let req = Request {
        command: Command::Status as u8,
        seq: 7,
        ..Request::default()
    };
    let payload: Vec<u8> = (1..=76).collect();
    let mut part = [0xaa; REPORT_LEN];
    diagwire::part(&mut part, &req, &payload, 1);
    assert_eq!(part_count(payload.len()), 2);
    assert_eq!(part[..HEADER_LEN], [Command::Status as u8, 7, 1, 2]);
    assert_eq!(part[HEADER_LEN], 61);
    assert_eq!(part[HEADER_LEN + 15], 76);
    assert_eq!(part[HEADER_LEN + 16], 0);
}

#[test]
fn an_empty_payload_is_one_part() {
    let req = Request {
        command: Command::Clear as u8,
        seq: 3,
        ..Request::default()
    };
    assert_eq!(part_count(0), 1);
    let mut report = [0xaa; REPORT_LEN];
    part(&mut report, &req, &[], 0);
    assert_eq!(report[..HEADER_LEN + 1], [Command::Clear as u8, 3, 0, 1, 0]);
}

#[test]
fn an_error_carries_the_flag_code_and_detail() {
    let req = Request {
        command: Command::Pair as u8,
        seq: 2,
        ..Request::default()
    };
    let mut report = [0xaa; REPORT_LEN];
    error_report(
        &mut report,
        &req,
        ErrorCode::WrongProtocol as u8,
        Protocol::Nuphy as u8,
    );
    assert_eq!(
        report[..HEADER_LEN + 2],
        [
            Command::Pair as u8 | ERROR_FLAG,
            2,
            0,
            1,
            ErrorCode::WrongProtocol as u8,
            Protocol::Nuphy as u8
        ]
    );
    assert_eq!(
        parse_part(&report),
        Some(Part::Error {
            command: Command::Pair as u8,
            seq: 2,
            code: ErrorCode::WrongProtocol as u8,
            detail: Protocol::Nuphy as u8
        })
    );
}

#[test]
fn forced_channels_go_in_the_low_bytes_of_the_arg() {
    assert_eq!(channels_arg(Some([62, 52, 44])), 0x002c343e);
    assert_eq!(channels_from_arg(0x002c343e), Some([62, 52, 44]));
    assert_eq!(channels_from_arg(0), None);
}

#[test]
fn a_protocol_answers_only_its_commands() {
    use Protocol::*;
    for c in Command::ALL {
        let answered = Protocol::ALL.map(|p| c.answered(p));
        let want = match c {
            Command::ForceChannels => [true, false, false],
            Command::Noise | Command::Move | Command::Watch => [true, true, false],
            Command::Forget => [false, false, true],
            _ => [true, true, true],
        };
        assert_eq!(answered, want, "{c:?}");
    }
    assert_eq!(Protocol::ALL, [Nuphy, Unifying, Bluetooth]);
}

fn minutes() -> [Minute; 2] {
    [
        Minute {
            packets: [7, 300],
            usb_retries: 1,
            gaps: [9, 3],
            link_losses: [9, 0],
            rssi: [
                Rssi {
                    min: -1,
                    avg: -1,
                    max: -1,
                },
                Rssi {
                    min: -80,
                    avg: -70,
                    max: -60,
                },
            ],
            noise: NoiseRecord {
                peak: -41,
                busy: 102,
                strong: 5,
            },
            ..Minute::default()
        },
        Minute {
            link_losses: [0, 1],
            ..Minute::default()
        },
    ]
}

#[test]
fn minutes_match_fixture() {
    let mut out = [0; PAYLOAD_MAX];
    let n = encode_minutes(&mut out, 1440, &minutes(), 1);
    assert_eq!(out[..n], fixture("minutes.hex"));
}

#[test]
fn minutes_decode_to_the_asked_slots_part() {
    let payload = fixture("minutes.hex");
    let (first, items) = decode_minutes(&payload).unwrap();
    let got: Vec<ReplyMinute> = items.collect::<Result<_, _>>().unwrap();
    let want = minutes().map(|m| ReplyMinute {
        invalid: m.invalid,
        dropped: m.dropped,
        usb_retries: m.usb_retries,
        noise: m.noise,
        packets: m.packets[1],
        gaps: m.gaps[1],
        link_losses: m.link_losses[1],
        rssi: m.rssi[1],
    });
    assert_eq!((first, got), (1440, want.to_vec()));
}

fn events() -> [Event; 2] {
    [
        Event {
            time_ms: 5000,
            r#type: EventType::LinkLost as u8,
            slot: 0,
            a: -71i8 as u8,
            value: 1200,
            noise: NoiseRecord {
                peak: -50,
                busy: 2500,
                strong: 300,
            },
            ..Event::default()
        },
        Event {
            time_ms: 7000,
            r#type: EventType::Survey as u8,
            slot: NO_SLOT,
            a: 26,
            b: 30,
            c: 34,
            ..Event::default()
        },
    ]
}

#[test]
fn events_match_fixture() {
    let mut out = [0; PAYLOAD_MAX];
    let n = encode_events(&mut out, 88, &events());
    assert_eq!(out[..n], fixture("events.hex"));
    let payload = fixture("events.hex");
    let (first, items) = decode_events(&payload).unwrap();
    let got: Vec<Event> = items.collect::<Result<_, _>>().unwrap();
    assert_eq!((first, got), (88, events().to_vec()));
}

#[test]
fn a_full_page_fits_the_payload_buffer() {
    let mut out = [0; PAYLOAD_MAX];
    encode_minutes(&mut out, 0, &[Minute::default(); MINUTES_PER_PAGE], 0);
    encode_events(&mut out, 0, &[Event::default(); EVENTS_PER_PAGE]);
}

#[test]
fn noise_matches_fixture() {
    const CHANNELS: usize = 79;
    const SAMPLES: usize = 32;
    let dbm: Vec<i8> = (0..CHANNELS)
        .flat_map(|i| {
            (0..SAMPLES).map(move |k| match (i, k) {
                (0, _) => -100,
                (i, _) if i == CHANNELS - 1 => -30,
                (_, 0) => -50,
                _ => -90,
            })
        })
        .collect();
    let mut out = [0; PAYLOAD_MAX];
    let n = encode_noise(&mut out, 2, SAMPLES, &dbm);
    assert_eq!(out[..n], fixture("noise.hex"));
}

#[test]
fn watch_and_devices_survive_a_round_trip() {
    for (name, encode) in [
        (
            "watch.hex",
            (|payload: &[u8], out: &mut [u8]| {
                let (head, items) = decode_watch(payload).unwrap();
                let channels: Vec<WatchChannel> = items.collect::<Result<_, _>>().unwrap();
                encode_watch(out, &head, &channels)
            }) as fn(&[u8], &mut [u8]) -> usize,
        ),
        ("devices.hex", |payload, out| {
            let devices: Vec<Device> = decode_devices(payload)
                .unwrap()
                .collect::<Result<_, _>>()
                .unwrap();
            encode_devices(out, &devices)
        }),
        ("status.hex", |payload, out| {
            Status::decode(payload).unwrap().encode(out)
        }),
        ("status-slot1.hex", |payload, out| {
            Status::decode(payload).unwrap().encode(out)
        }),
    ] {
        let want = fixture(name);
        let mut out = [0; PAYLOAD_MAX];
        let n = encode(&want, &mut out);
        assert_eq!(out[..n], want, "{name}");
    }
}

#[test]
fn a_charging_byte_of_no_state_goes_out_as_unknown() {
    let mut status = Status::decode(&fixture("status.hex")).unwrap();
    status.battery = Battery {
        level: 40,
        charging: 9,
    };
    let mut out = [0; PAYLOAD_MAX];
    let n = status.encode(&mut out);
    assert_eq!(out[n - 2..n], [40, Charging::Unknown as u8]);
}

#[test]
fn readings_fall_into_10_db_buckets_from_the_busy_level() {
    let got = [-96, -86, -85, -76, -75, -56, -46, -45, -20].map(bucket);
    assert_eq!(got, [0, 0, 1, 1, 2, 3, 4, 5, 5]);
    assert_eq!(bucket_floor(0), None);
}

#[test]
fn bucket_floors_match_fixture() {
    let floors: Vec<i8> = (1..BUCKETS as u8)
        .map(|b| bucket_floor(b).unwrap())
        .collect();
    assert_eq!(
        floors.iter().map(|&f| f as u8).collect::<Vec<_>>(),
        fixture("bucket-floors.hex")
    );
    for (b, floor) in (1..).zip(floors) {
        assert_eq!((bucket(floor - 1), bucket(floor)), (b - 1, b));
    }
}

#[test]
fn keyboard_channel_tables_match_fixtures() {
    assert_eq!(
        NUPHY_FIXED_CHANNELS.to_vec(),
        fixture("nuphy-fixed-channels.hex")
    );
    assert_eq!(UNIFYING_CHANNELS.to_vec(), fixture("unifying-channels.hex"));
}

fn every_detail() -> Vec<Detail> {
    let addr = [1, 2, 3, 4, 5, 6];
    vec![
        Detail::Boot { reset_cause: 0x81 },
        Detail::Paired {
            own: [26, 30, 34],
            store_err: -5,
        },
        Detail::LinkLost {
            rssi: -71,
            since_input_s: 42,
            gap_ms: 1200,
        },
        Detail::Survey {
            picked: [26, 30, 34],
        },
        Detail::Noise,
        Detail::UsbSuspend,
        Detail::UsbResume,
        Detail::Clear,
        Detail::LinkUp {
            channel: 33,
            rssi: -63,
            up: Up::Again as u8,
            down_ms: 4200,
        },
        Detail::LinkMoved {
            from: 5,
            to: 41,
            reason: Moved::ForNoise as u8,
            home_share: 4600,
            to_share: 210,
        },
        Detail::LinkGap {
            channel: 38,
            rssi_before: -82,
            rssi_after: -79,
            gap_ms: 340,
        },
        Detail::UnifyingPaired {
            wpid: [0x40, 0x8a],
            kind: 1,
            store_err: 0,
        },
        Detail::PairingOpened {
            cause: 1,
            until_paired: true,
        },
        Detail::PairingClosed,
        Detail::PairingTimeout { step: 1 },
        Detail::Reconnect { release_all: true },
        Detail::Attack,
        Detail::BtConnected { addr },
        Detail::BtDisconnected {
            reason: 0x13,
            since_input_s: u16::MAX,
        },
        Detail::BtPaired {
            addr,
            passkey: true,
        },
        Detail::BtRefused {
            refused: Refused::NoPasskey as u8,
        },
        Detail::BtBondLost { addr },
        Detail::BtForgotten {
            addr,
            cause: Forgotten::UnusableMap as u8,
        },
        Detail::KeysDiverted {
            outcome: Divert::Unanswered as u8,
            diverted: 2,
            refused: 1,
            control: 0x00c7,
        },
        Detail::WheelSet {
            outcome: Wheel::Hires as u8,
            steps: 15,
        },
        Detail::BtPasskeyAwaited { addr },
    ]
}

#[test]
fn every_detail_reads_back_from_its_event() {
    let details = every_detail();
    assert_eq!(
        details.iter().map(|d| d.event_type()).collect::<Vec<_>>(),
        EventType::ALL
    );
    for d in details {
        let e = d.event(1);
        assert_eq!((e.r#type, e.slot), (d.event_type() as u8, 1), "{d:?}");
        assert_eq!(Detail::of(&e), Ok(d));
    }
}

#[test]
fn an_unknown_event_type_reads_as_its_byte() {
    let e = Event {
        r#type: 200,
        ..Event::default()
    };
    assert_eq!(Detail::of(&e), Err(200));
}

#[test]
fn a_bluetooth_event_packs_the_address_as_the_fixture_has_it() {
    let f = fixture("event-address.hex");
    let (addr, packed) = f.split_at(ADDR_LEN);
    let e = Detail::BtPaired {
        addr: addr[1..].try_into().unwrap(),
        passkey: true,
    }
    .event(1);
    assert_eq!(
        (e.a, e.b, e.c, e.value),
        (
            packed[0],
            packed[1],
            1,
            u32::from_le_bytes(packed[2..6].try_into().unwrap())
        )
    );
}

#[test]
fn time_since_input_is_whole_seconds_up_to_the_most_a_u16_holds() {
    assert_eq!(since_input_s(42_999), 42);
    assert_eq!(since_input_s(1_000_999), 1000);
    assert_eq!(since_input_s(65_535_000), u16::MAX);
    assert_eq!(since_input_s(100_000_000), u16::MAX);
    assert_eq!(since_input_s(-1), u16::MAX);
}
