//! Integration tests for `run_stream_loop` and `pre_stream_drain` using `MockLaTransport`.
//!
//! These tests exercise the streaming loop logic without USB hardware.

#[cfg(test)]
mod tests {
    use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
    use std::sync::Mutex;

    use anyhow::Result;

    use crate::la_commands::{pre_stream_drain, run_stream_loop, LaStreamRuntimeStatus};
    use crate::la_store::LaStore;
    use crate::la_transport::{MockLaTransport, StreamStopReason};
    use crate::la_usb::{
        LaStreamPacket, LaStreamPacketKind, LA_USB_CMD_START_STREAM, LA_USB_CMD_STOP,
    };

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

    // ── run_stream_loop tests ────────────────────────────────────────────────
    // These tests assume pre_stream_drain() has already been called externally
    // (as done by la_stream_usb() before spawning). The FIFO is clean when
    // run_stream_loop starts — it only sends LA_USB_CMD_START_STREAM.

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
        // Only START command must be sent — drain is done externally now
        assert_eq!(
            transport.commands_sent,
            vec![LA_USB_CMD_START_STREAM],
            "run_stream_loop must only send START, not STOP"
        );
    }

    // ── test 2: sequence wrap (255→0 is valid) ───────────────────────────────

    #[test]
    fn test_stream_seq_wrap() {
        // Build a packet list where seq goes 0..=255 then wraps back to 0.
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
        let n: u32 = 100;
        let header = n.to_le_bytes();
        let payload = vec![0xABu8; n as usize];
        let full = [&header[..], &payload[..]].concat();

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
        // Immediate stop — START confirmation then immediate STOP with no data
        let packets = vec![make_start(), make_stop()];
        let mut transport = MockLaTransport::new(packets);
        let running = running();
        let store = empty_store();
        let status = empty_status();
        let stream_seq = seq();

        run_stream_loop(&mut transport, &running, &store, &status, &stream_seq);

        assert!(
            !running.load(Ordering::SeqCst),
            "running must always be set to false on any exit path"
        );
        assert!(
            !status.lock().unwrap().active,
            "status.active must always be set to false on any exit path"
        );
    }

    // ── pre_stream_drain tests ───────────────────────────────────────────────
    // pre_stream_drain() is called by la_stream_usb() BEFORE CMD_HAT_LA_CONFIG.
    // It sends LA_USB_CMD_STOP (0x00) and drains until PKT_STOP.

    // ── test 6: drain consumes stale STOP from previous host-stop ────────────
    // When a stream was stopped by the host (CMD_HAT_LA_STOP via BBP), the
    // firmware emits PKT_STOP on vendor-bulk. If the background task exited
    // before consuming it, the next session must drain it.

    #[test]
    fn test_pre_drain_consumes_stale_stop() {
        let packets = vec![
            // Stale STOP left from previous host-stop.
            Ok(LaStreamPacket { kind: LaStreamPacketKind::Stop, seq: 0, info: 1, payload: vec![] }),
            // PKT_STOP generated by our LA_USB_CMD_STOP command.
            make_stop(),
        ];
        let mut transport = MockLaTransport::new(packets);

        pre_stream_drain(&mut transport);

        assert_eq!(transport.commands_sent, vec![LA_USB_CMD_STOP], "must send STOP command");
        // The stale STOP was consumed; our LA_USB_CMD_STOP response (the second STOP) remains.
        // run_stream_loop tolerates this one leftover STOP before PKT_START.
        assert_eq!(transport.packets.len(), 1, "one STOP (our drain response) must remain for run_stream_loop");
    }

    // ── test 7: drain consumes mixed stale DATA + STOP + START ───────────────

    #[test]
    fn test_pre_drain_consumes_mixed_stale_packets() {
        let packets = vec![
            // Stale DATA and START left over from a previous aborted session.
            Ok(LaStreamPacket { kind: LaStreamPacketKind::Data, seq: 42, info: 0, payload: b"x".to_vec() }),
            Ok(LaStreamPacket { kind: LaStreamPacketKind::Start, seq: 0, info: 0, payload: vec![] }),
            // PKT_STOP generated by our LA_USB_CMD_STOP command.
            make_stop(),
        ];
        let mut transport = MockLaTransport::new(packets);

        pre_stream_drain(&mut transport);

        assert_eq!(transport.commands_sent, vec![LA_USB_CMD_STOP]);
        assert!(transport.packets.is_empty(), "all stale packets must be consumed");
    }
}
