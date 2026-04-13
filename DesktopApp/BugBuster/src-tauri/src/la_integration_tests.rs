//! Integration tests for `run_stream_loop` using `MockLaTransport`.
//!
//! These tests exercise the streaming loop logic without USB hardware.

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
    use std::sync::Mutex;

    use anyhow::Result;

    use crate::la_commands::{run_stream_loop, LaStreamRuntimeStatus};
    use crate::la_store::LaStore;
    use crate::la_transport::{MockLaTransport, StreamStopReason};
    use crate::la_usb::{LaStreamPacket, LaStreamPacketKind};

    // ── helpers ──────────────────────────────────────────────────────────────

    fn make_start() -> Result<LaStreamPacket> {
        Ok(LaStreamPacket { kind: LaStreamPacketKind::Start, seq: 0, info: 0, payload: vec![] })
    }
    fn make_data(seq: u8, payload: &[u8]) -> Result<LaStreamPacket> {
        Ok(LaStreamPacket {
            kind: LaStreamPacketKind::Data,
            seq,
            info: 0,
            payload: payload.to_vec(),
        })
    }
    fn make_stop() -> Result<LaStreamPacket> {
        Ok(LaStreamPacket { kind: LaStreamPacketKind::Stop, seq: 0, info: 0, payload: vec![] })
    }

    fn empty_store() -> Mutex<Option<LaStore>> {
        Mutex::new(None)
    }
    fn empty_status() -> Mutex<LaStreamRuntimeStatus> {
        Mutex::new(LaStreamRuntimeStatus::default())
    }
    fn running() -> AtomicBool {
        AtomicBool::new(true)
    }
    fn seq() -> AtomicU8 {
        AtomicU8::new(0)
    }

    // ── test 1: happy path ────────────────────────────────────────────────────

    #[test]
    fn test_stream_happy_path() {
        let packets = vec![
            make_start(),
            make_data(0, &[1, 2, 3, 4]),
            make_data(1, &[5, 6, 7, 8]),
            make_data(2, &[9, 10, 11, 12]),
            make_data(3, &[13, 14, 15, 16]),
            make_data(4, &[17, 18, 19, 20]),
            make_stop(),
        ];
        let mut transport = MockLaTransport::new(packets);
        let running = running();
        let store = empty_store();
        let status = empty_status();
        let stream_seq = seq();

        let reason = run_stream_loop(&mut transport, &running, &store, &status, &stream_seq);

        assert_eq!(reason, StreamStopReason::Normal, "Expected Normal stop");
        assert!(!running.load(Ordering::SeqCst), "running must be false after loop");
        assert!(!status.lock().unwrap().active, "status.active must be false after loop");
        // Verify START command was sent
        assert!(transport.commands_sent.contains(&0x01), "STREAM_CMD_START must be sent");
    }

    // ── test 2: sequence wrap (255→0 is valid) ───────────────────────────────

    #[test]
    fn test_stream_seq_wrap() {
        // Build a packet list where seq goes 0..=255 then wraps back to 0.
        // After 256 data packets expected_seq wraps to 0, so seq=0 is valid.
        let mut wrap_packets = vec![make_start()];
        for s in 0u8..=255 {
            wrap_packets.push(make_data(s, b"x"));
        }
        wrap_packets.push(make_data(0, b"x")); // valid wrap
        wrap_packets.push(make_stop());

        let mut transport = MockLaTransport::new(wrap_packets);
        let run_flag = running();
        let store = empty_store();
        let status = empty_status();
        let stream_seq = seq();

        let reason = run_stream_loop(&mut transport, &run_flag, &store, &status, &stream_seq);
        assert_eq!(
            reason,
            StreamStopReason::Normal,
            "seq wrap 255->0 must be treated as valid, not a mismatch"
        );
        assert!(!run_flag.load(Ordering::SeqCst));
        assert!(!status.lock().unwrap().active);
    }

    // ── test 3: oneshot header parsing ───────────────────────────────────────

    #[test]
    fn test_oneshot_header_data() {
        // Test the one-shot header format: [u32 LE total_len][raw_data...]
        // This tests the packet format contract, not run_stream_loop
        let n: u32 = 100;
        let header = n.to_le_bytes();
        let payload = vec![0xABu8; n as usize];
        let full = [&header[..], &payload[..]].concat();

        // Simulate what read_capture_blocking does:
        let reported_len =
            u32::from_le_bytes(full[..4].try_into().unwrap()) as usize;
        let data = &full[4..4 + reported_len];

        assert_eq!(reported_len, 100);
        assert_eq!(data.len(), 100);
        assert!(data.iter().all(|&b| b == 0xAB));
    }

    // ── test 4: USB read failure ──────────────────────────────────────────────

    #[test]
    fn test_stream_usb_read_failure() {
        let packets = vec![
            make_start(),
            make_data(0, b"ok"),
            make_data(1, b"ok"),
            Err(anyhow::anyhow!("USB timeout: bulk read failed")),
        ];
        let mut transport = MockLaTransport::new(packets);
        let running = running();
        let store = empty_store();
        let status = empty_status();
        let stream_seq = seq();

        let reason = run_stream_loop(&mut transport, &running, &store, &status, &stream_seq);

        match reason {
            StreamStopReason::UsbError(_) => {} // expected
            other => panic!("Expected UsbError, got {other:?}"),
        }
        assert!(!running.load(Ordering::SeqCst), "running must be false after USB error");
        assert!(!status.lock().unwrap().active, "status.active must be false after USB error");
    }

    // ── test 5: teardown always runs ─────────────────────────────────────────

    #[test]
    fn test_stream_teardown_cleanup() {
        // Immediate stop — START then STOP with no data
        let packets = vec![make_start(), make_stop()];
        let mut transport = MockLaTransport::new(packets);
        let running = running();
        let store = empty_store();
        let status = empty_status();
        let stream_seq = seq();

        run_stream_loop(&mut transport, &running, &store, &status, &stream_seq);

        // Teardown must ALWAYS run regardless of stop reason
        assert!(
            !running.load(Ordering::SeqCst),
            "running must always be set to false on any exit path"
        );
        assert!(
            !status.lock().unwrap().active,
            "status.active must always be set to false on any exit path"
        );
    }

    // ── test 6: stale STOP from previous session is drained ──────────────────
    // When la_stream_usb_stop() sets running=false and the background task exits
    // without consuming the firmware's STOP marker, the next session must drain
    // it before entering the data loop (see drain loop in run_stream_loop).

    #[test]
    fn test_stream_drains_stale_stop_before_start() {
        let packets = vec![
            // Stale STOP left in the FIFO by the previous session's host-stop.
            Ok(LaStreamPacket { kind: LaStreamPacketKind::Stop, seq: 0, info: 1, payload: vec![] }),
            // Firmware's START confirmation for the new STREAM_CMD_START.
            make_start(),
            make_data(0, &[0xDE, 0xAD]),
            make_stop(),
        ];
        let mut transport = MockLaTransport::new(packets);
        let run_flag = running();
        let store = empty_store();
        let status = empty_status();
        let stream_seq = seq();

        let reason = run_stream_loop(&mut transport, &run_flag, &store, &status, &stream_seq);

        assert_eq!(
            reason,
            StreamStopReason::Normal,
            "stale STOP must be drained, not terminate the new session"
        );
        assert!(!run_flag.load(Ordering::SeqCst));
        assert!(!status.lock().unwrap().active);
        assert_eq!(status.lock().unwrap().chunk_count, 1, "one DATA chunk expected");
    }

    // ── test 7: multiple stale packets drained before START ──────────────────

    #[test]
    fn test_stream_drains_multiple_stale_packets() {
        let packets = vec![
            // Stale DATA then stale STOP from a previous session.
            Ok(LaStreamPacket { kind: LaStreamPacketKind::Data, seq: 42, info: 0, payload: b"x".to_vec() }),
            Ok(LaStreamPacket { kind: LaStreamPacketKind::Stop, seq: 0, info: 1, payload: vec![] }),
            // Real session begins here.
            make_start(),
            make_data(0, b"real"),
            make_stop(),
        ];
        let mut transport = MockLaTransport::new(packets);
        let run_flag = running();
        let store = empty_store();
        let status = empty_status();
        let stream_seq = seq();

        let reason = run_stream_loop(&mut transport, &run_flag, &store, &status, &stream_seq);

        assert_eq!(reason, StreamStopReason::Normal, "multiple stale packets must all be drained");
        assert_eq!(status.lock().unwrap().chunk_count, 1, "only real DATA chunk counted");
    }
}
