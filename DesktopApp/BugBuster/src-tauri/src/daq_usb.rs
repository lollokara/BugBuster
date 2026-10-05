// =============================================================================
// daq_usb.rs — ESP32-P4 DAQ USB-HS vendor-bulk transport + synthetic source.
//
// The P4 exposes its own dedicated USB-HS port to the PC (independent of the
// S3 CDC/BBP link): VID 0x303A, PID 0x4001, vendor interface 0, bulk IN 0x81 /
// bulk OUT 0x01, 512-byte HS packets. Measurement frames stream device->PC;
// control commands flow PC->device. See daq_proto.rs for the byte layout.
//
// `MockDaqTransport` synthesises the same record stream so the desktop app can
// run as a "Demo / Mock device" with no hardware attached.
// =============================================================================

use crate::daq_proto::{
    self, DaqRecord, EnergyRecord, FftRecord, MarkerRecord, StandbyAckRecord, StatBlock,
    StatsRecord, StatusRecord, WaveIRecord, WaveVRecord, MARK_KIND_FLAG, MARK_KIND_TRIGGER,
    META_SATURATED, META_SETTLING, RANGE_HI, RANGE_LO, RANGE_MID, SRC_BLEND, SRC_COARSE,
    SRC_FINE,
};
use anyhow::{anyhow, Result};
use nusb::transfer::{Queue, RequestBuffer};
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

pub const DAQ_VID: u16 = 0x303A;
pub const DAQ_PID: u16 = 0x4001;
const DAQ_IFACE: u8 = 0;
const DAQ_EP_IN: u8 = 0x81;
const DAQ_EP_OUT: u8 = 0x01;

/// Common interface implemented by the real USB transport and the mock source.
pub trait DaqTransport: Send {
    /// Read and decode whatever frames are currently available. Blocks until at
    /// least one record is produced or a timeout/error occurs.
    fn read_records(&mut self) -> Result<Vec<DaqRecord>>;
    /// Send a control command (type + payload); the transport frames it.
    fn send(&mut self, cmd_type: u8, payload: &[u8]) -> Result<()>;
}

/// True if a P4 DAQ device is present on USB.
pub fn daq_usb_present() -> bool {
    nusb::list_devices()
        .map(|devs| {
            devs.into_iter()
                .any(|d| d.vendor_id() == DAQ_VID && d.product_id() == DAQ_PID)
        })
        .unwrap_or(false)
}

/// Number of RequestBuffers kept in flight on the bulk-IN queue.
const QUEUE_DEPTH: usize = 4;
/// Size of each queued IN transfer buffer.
const QUEUE_BUF_LEN: usize = 65536;
/// Longest single wait inside `read_records`. The ingest thread holds the
/// shared transport mutex for one call, so this bounds control-command latency.
const DAQ_READ_SLICE_MS: u64 = 50;
/// Total silence on bulk IN before `read_records` reports a timeout (DESK-7).
const DAQ_IDLE_TIMEOUT_MS: u64 = 1000;

// -----------------------------------------------------------------------------
// Logical client presence (system standby lease)
// -----------------------------------------------------------------------------
//
// A mounted P4 or an idle IN poll is NOT a host. Only an opened data plane that sends
// USB_CMD_CLIENT_LEASE declares one. Each `connect()` is a new lease epoch with a new
// random non-zero client id; the lease is refreshed every LEASE_REFRESH (device TTL is
// LEASE_TTL_MS) and released best-effort on `close()`/drop.
//
// The ACK (usb_proto.h `bb_standby_reply_t`) does not name the client it answers, so
// the exchanges are strictly serialized: at most one lease frame awaits its ACK, and
// an ACK is applied only to that exchange of the epoch that read it. An ACK with no
// exchange outstanding (late duplicate, or one from a previous epoch whose bytes were
// still in flight) is ignored. Honest limit: a delayed duplicate that lands while a
// later exchange is outstanding is indistinguishable from its ACK.

pub const LEASE_REFRESH: Duration = Duration::from_secs(10);
pub const LEASE_TTL_MS: u32 = 45_000;
/// A lease exchange with no ACK this long is considered lost.
pub const LEASE_ACK_TIMEOUT: Duration = Duration::from_secs(2);
/// Resends of the SAME lease frame after a lost ACK (never an acquisition command).
pub const LEASE_MAX_RESENDS: u8 = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LeasePhase {
    /// Acquire sent (or about to be), nothing answered yet.
    Opening,
    /// Accepted and the system is ACTIVE.
    Held,
    /// Accepted, but the system is not ACTIVE yet (failure 1): a wake is already requested.
    /// Nothing is replayed or auto-started on this.
    Waking,
    /// Table full (failure 7): refused, nobody evicted; retried every period.
    Full,
    /// Malformed frame (failure 3): a bug, not retried.
    Rejected,
    /// Another failure code from a newer firmware; retried every period.
    Failed(u8),
    /// The exchange got no ACK after its bounded resends; retried next period.
    AckLost,
    Released,
}

/// What the transport must put on the wire.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LeaseSend {
    pub op: u8,
    pub client_id: u32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AckOutcome {
    Applied(LeasePhase),
    /// No outstanding exchange for this epoch: the ACK changed nothing.
    Ignored,
}

#[derive(Debug, Clone, Copy)]
struct PendingLease {
    op: u8,
    client_id: u32,
    sent_at: Instant,
    resends: u8,
}

#[derive(Debug)]
pub struct LeaseCore {
    epoch: u64,
    client_id: u32,
    phase: LeasePhase,
    last_ack: Option<StandbyAckRecord>,
    pending: Option<PendingLease>,
    /// Ids of earlier epochs the device may still hold, released before this one acquires.
    owed_release: VecDeque<u32>,
    next_due: Instant,
    /// The device may hold this epoch's lease.
    sent_acquire: bool,
    closed: bool,
}

impl LeaseCore {
    pub fn new(epoch: u64, client_id: u32, now: Instant, owed_release: Vec<u32>) -> Self {
        Self {
            epoch,
            client_id,
            phase: LeasePhase::Opening,
            last_ack: None,
            pending: None,
            owed_release: owed_release.into(),
            next_due: now,
            sent_acquire: false,
            closed: false,
        }
    }

    pub fn epoch(&self) -> u64 {
        self.epoch
    }

    pub fn client_id(&self) -> u32 {
        self.client_id
    }

    pub fn phase(&self) -> LeasePhase {
        self.phase
    }

    pub fn last_ack(&self) -> Option<StandbyAckRecord> {
        self.last_ack
    }

    pub fn is_closed(&self) -> bool {
        self.closed
    }

    /// The next frame due at `now`, if any. Marks it outstanding.
    pub fn poll(&mut self, now: Instant) -> Option<LeaseSend> {
        if self.closed {
            return None;
        }
        if let Some(p) = &mut self.pending {
            if now.duration_since(p.sent_at) < LEASE_ACK_TIMEOUT {
                return None;
            }
            if p.resends < LEASE_MAX_RESENDS {
                p.resends += 1;
                p.sent_at = now;
                return Some(LeaseSend { op: p.op, client_id: p.client_id });
            }
            let lost = self.pending.take();
            if lost.is_some_and(|p| p.op == daq_proto::LEASE_OP_ACQUIRE) {
                self.phase = LeasePhase::AckLost;
                self.next_due = now + LEASE_REFRESH;
            }
            return None;
        }
        if let Some(id) = self.owed_release.pop_front() {
            self.pending = Some(PendingLease {
                op: daq_proto::LEASE_OP_RELEASE,
                client_id: id,
                sent_at: now,
                resends: 0,
            });
            return Some(LeaseSend { op: daq_proto::LEASE_OP_RELEASE, client_id: id });
        }
        if self.phase == LeasePhase::Rejected || now < self.next_due {
            return None;
        }
        self.pending = Some(PendingLease {
            op: daq_proto::LEASE_OP_ACQUIRE,
            client_id: self.client_id,
            sent_at: now,
            resends: 0,
        });
        self.sent_acquire = true;
        Some(LeaseSend { op: daq_proto::LEASE_OP_ACQUIRE, client_id: self.client_id })
    }

    /// Apply an ACK read while epoch `read_epoch` was current.
    pub fn on_ack(&mut self, read_epoch: u64, ack: StandbyAckRecord, now: Instant) -> AckOutcome {
        if self.closed || read_epoch != self.epoch {
            return AckOutcome::Ignored;
        }
        let Some(p) = self.pending.take() else {
            return AckOutcome::Ignored;
        };
        self.last_ack = Some(ack);
        if p.op == daq_proto::LEASE_OP_RELEASE {
            // An owed release of an earlier epoch says nothing about this one.
            return AckOutcome::Applied(self.phase);
        }
        self.phase = match ack.failure {
            daq_proto::LEASE_FAIL_NONE if ack.ready => LeasePhase::Held,
            daq_proto::LEASE_FAIL_NONE | daq_proto::LEASE_FAIL_NOT_READY => LeasePhase::Waking,
            daq_proto::LEASE_FAIL_TABLE_FULL => LeasePhase::Full,
            daq_proto::LEASE_FAIL_MALFORMED => LeasePhase::Rejected,
            other => LeasePhase::Failed(other),
        };
        self.next_due = now + LEASE_REFRESH;
        AckOutcome::Applied(self.phase)
    }

    /// End this epoch. Returns the single release frame owed to the device, if it may hold
    /// our lease. Nothing is polled afterwards and no ACK is applied.
    pub fn close(&mut self) -> Option<LeaseSend> {
        if self.closed {
            return None;
        }
        self.closed = true;
        self.pending = None;
        let held = self.sent_acquire
            && !matches!(
                self.phase,
                LeasePhase::Full | LeasePhase::Rejected | LeasePhase::Released
            );
        self.phase = LeasePhase::Released;
        held.then_some(LeaseSend {
            op: daq_proto::LEASE_OP_RELEASE,
            client_id: self.client_id,
        })
    }
}

/// Snapshot of the lease for diagnostics.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LeaseSnapshot {
    pub epoch: u64,
    pub client_id: u32,
    pub phase: LeasePhase,
    pub last_ack: Option<StandbyAckRecord>,
}

static LEASE_EPOCH: AtomicU64 = AtomicU64::new(0);

/// The interface the lease keeper may send on. The connection clears it on close; `users`
/// lets `connect()` wait for a keeper send still in flight before it claims the interface again.
#[derive(Default)]
struct LeaseLink {
    iface: Mutex<Option<nusb::Interface>>,
    users: AtomicUsize,
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// Background task of one lease epoch: sends whatever `poll` says is due. It reads no
/// records (the ingest path owns bulk IN) and exits when the core is closed.
async fn lease_keeper(
    core: Arc<Mutex<LeaseCore>>,
    link: Arc<LeaseLink>,
    out_seq: Arc<AtomicU32>,
) {
    loop {
        let (send, closed) = {
            let mut c = lock(&core);
            let send = c.poll(Instant::now());
            (send, c.is_closed())
        };
        if closed {
            break;
        }
        if let Some(s) = send {
            let ttl = if s.op == daq_proto::LEASE_OP_ACQUIRE { LEASE_TTL_MS } else { 0 };
            let frame = daq_proto::encode_command(
                out_seq.fetch_add(1, Ordering::Relaxed),
                daq_proto::CMD_CLIENT_LEASE,
                &daq_proto::lease_payload(s.op, s.client_id, ttl),
            );
            link.users.fetch_add(1, Ordering::AcqRel);
            {
                let iface = lock(&link.iface).clone();
                if let Some(iface) = iface {
                    let sent = tokio::time::timeout(
                        Duration::from_millis(500),
                        iface.bulk_out(DAQ_EP_OUT, frame),
                    )
                    .await;
                    match sent {
                        Ok(c) => {
                            if let Err(e) = c.into_result() {
                                log::debug!("DAQ lease frame not sent: {e}");
                            }
                        }
                        Err(_) => log::debug!("DAQ lease frame timed out"),
                    }
                }
            }
            link.users.fetch_sub(1, Ordering::AcqRel);
        }
        tokio::time::sleep(Duration::from_millis(250)).await;
    }
}

pub struct DaqUsbConnection {
    interface: Option<nusb::Interface>,
    queue: Option<Queue<RequestBuffer>>,
    connected: bool,
    rx: Vec<u8>,
    out_seq: Arc<AtomicU32>,
    /// Timestamp of the last "update firmware" warning emitted for a run of
    /// BadVersion frames, so we don't spam the log every drained byte.
    last_bad_version_warn: Option<Instant>,
    /// Start of the current run of empty read slices (DESK-24).
    idle_since: Option<Instant>,
    /// Standby lease of the current open (one epoch per `connect()`).
    lease: Option<Arc<Mutex<LeaseCore>>>,
    lease_link: Arc<LeaseLink>,
    /// Runtime the lease keeper runs on (captured at `connect()`).
    lease_rt: Option<tokio::runtime::Handle>,
    /// Lease epoch the bytes in `rx` were read under.
    rx_epoch: u64,
}

impl Default for DaqUsbConnection {
    fn default() -> Self {
        Self::new()
    }
}

impl DaqUsbConnection {
    pub fn new() -> Self {
        Self {
            interface: None,
            queue: None,
            connected: false,
            rx: Vec::new(),
            out_seq: Arc::new(AtomicU32::new(0)),
            last_bad_version_warn: None,
            idle_since: None,
            lease: None,
            lease_link: Arc::new(LeaseLink::default()),
            lease_rt: None,
            rx_epoch: 0,
        }
    }

    pub fn connect(&mut self) -> Result<()> {
        // End the previous open's lease first: a keeper send still in flight must not hold
        // the interface when it is claimed again, and the device may still hold that id.
        let owed = self.retire_lease();
        let devices =
            nusb::list_devices().map_err(|e| anyhow!("USB enumeration failed: {}", e))?;
        for info in devices {
            if info.vendor_id() == DAQ_VID && info.product_id() == DAQ_PID {
                let device = info
                    .open()
                    .map_err(|e| anyhow!("Failed to open DAQ USB device: {}", e))?;
                let iface = device
                    .claim_interface(DAQ_IFACE)
                    .map_err(|e| anyhow!("Failed to claim DAQ interface {}: {}", DAQ_IFACE, e))?;
                log::info!("DAQ USB interface claimed (VID={DAQ_VID:04X} PID={DAQ_PID:04X})");
                let mut queue = iface.bulk_in_queue(DAQ_EP_IN);
                for _ in 0..QUEUE_DEPTH {
                    queue.submit(RequestBuffer::new(QUEUE_BUF_LEN));
                }
                self.interface = Some(iface.clone());
                self.queue = Some(queue);
                self.connected = true;
                self.rx.clear();
                self.start_lease(iface, owed);
                return Ok(());
            }
        }
        Err(anyhow!(
            "DAQ HAT not found on USB (VID={:04X} PID={:04X})",
            DAQ_VID,
            DAQ_PID
        ))
    }

    /// A new lease epoch for the open that just succeeded.
    fn start_lease(&mut self, iface: nusb::Interface, owed: Vec<u32>) {
        let epoch = LEASE_EPOCH.fetch_add(1, Ordering::AcqRel) + 1;
        let core = Arc::new(Mutex::new(LeaseCore::new(
            epoch,
            crate::standby_commands::new_client_id(),
            Instant::now(),
            owed,
        )));
        self.rx_epoch = epoch;
        self.lease = Some(core.clone());
        self.lease_rt = tokio::runtime::Handle::try_current().ok();
        let Some(rt) = &self.lease_rt else {
            log::warn!("DAQ lease: no async runtime, presence is not announced");
            return;
        };
        *lock(&self.lease_link.iface) = Some(iface);
        rt.spawn(lease_keeper(core, self.lease_link.clone(), self.out_seq.clone()));
    }

    /// Close the current epoch and wait (bounded) for the keeper to let go of the interface.
    /// Returns the id of that epoch if the device may still hold it.
    fn retire_lease(&mut self) -> Vec<u32> {
        let owed = self
            .lease
            .take()
            .and_then(|core| lock(&core).close())
            .map(|s| s.client_id);
        *lock(&self.lease_link.iface) = None;
        let deadline = Instant::now() + Duration::from_millis(1500);
        while self.lease_link.users.load(Ordering::Acquire) > 0 && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(5));
        }
        owed.into_iter().collect()
    }

    /// Best-effort release of the current lease on the way out (bounded, ACK not awaited).
    fn release_lease(&mut self) {
        let iface = self.interface.clone();
        let rt = self.lease_rt.clone();
        let owed = self.retire_lease();
        let (Some(iface), Some(rt), Some(id)) = (iface, rt, owed.first().copied()) else {
            return; // nothing held, or no way to speak: the device expires the lease
        };
        let frame = daq_proto::encode_command(
            self.out_seq.fetch_add(1, Ordering::Relaxed),
            daq_proto::CMD_CLIENT_LEASE,
            &daq_proto::lease_payload(daq_proto::LEASE_OP_RELEASE, id, 0),
        );
        // A short-lived thread: never `block_on` on the caller's (possibly async) thread, and
        // the interface clone is gone before this returns, so a quick reconnect can claim it.
        let _ = std::thread::spawn(move || {
            rt.block_on(async {
                let _ = tokio::time::timeout(
                    Duration::from_millis(500),
                    iface.bulk_out(DAQ_EP_OUT, frame),
                )
                .await;
            });
        })
        .join();
    }

    /// Current lease state (None before the first open or after close).
    #[allow(dead_code)]
    pub fn lease_snapshot(&self) -> Option<LeaseSnapshot> {
        let core = lock(self.lease.as_ref()?);
        Some(LeaseSnapshot {
            epoch: core.epoch(),
            client_id: core.client_id(),
            phase: core.phase(),
            last_ack: core.last_ack(),
        })
    }

    fn apply_ack(&mut self, ack: Result<StandbyAckRecord, String>) {
        let Some(core) = &self.lease else { return };
        let ack = match ack {
            Ok(a) => a,
            Err(e) => {
                log::warn!("DAQ lease: undecodable standby ACK: {e}");
                return;
            }
        };
        let mut c = lock(core);
        let before = c.phase();
        if let AckOutcome::Applied(now) = c.on_ack(self.rx_epoch, ack, Instant::now()) {
            if now != before {
                log::info!(
                    "DAQ lease {:?} (state {}, ready {}, {} client(s))",
                    now,
                    ack.state,
                    ack.ready,
                    ack.inhibitors
                );
            }
        }
    }

    pub fn is_connected(&self) -> bool {
        self.connected
    }

    pub fn close(&mut self) {
        self.release_lease();
        self.interface = None;
        self.queue = None;
        self.connected = false;
        self.rx.clear();
    }

    /// Parse as many complete frames as the buffer holds, resyncing on garbage.
    fn drain_frames(&mut self) -> Vec<DaqRecord> {
        let mut out = Vec::new();
        loop {
            match daq_proto::split_frame(&self.rx) {
                Ok((rec_type, payload, consumed)) => {
                    if rec_type == daq_proto::REC_STANDBY_ACK {
                        let ack = daq_proto::decode_standby_ack(payload);
                        self.apply_ack(ack);
                    } else {
                        out.push(daq_proto::decode_payload(rec_type, payload));
                    }
                    self.rx.drain(0..consumed);
                }
                Err(daq_proto::FrameError::Truncated { .. })
                | Err(daq_proto::FrameError::ShortHeader(_)) => break,
                Err(daq_proto::FrameError::BadVersion(got)) => {
                    // Rate-limited: repeated BadVersion frames almost always
                    // mean the firmware and app protocol versions have
                    // drifted apart, not transient noise on the wire.
                    let should_warn = match self.last_bad_version_warn {
                        Some(t) => t.elapsed() >= Duration::from_secs(5),
                        None => true,
                    };
                    if should_warn {
                        log::warn!(
                            "DAQ USB: frame version mismatch (got {}, expected {}) — \
                             update firmware or app to matching protocol versions",
                            got,
                            daq_proto::PROTO_VERSION
                        );
                        self.last_bad_version_warn = Some(Instant::now());
                    }
                    if self.rx.is_empty() {
                        break;
                    }
                    self.rx.remove(0);
                }
                Err(daq_proto::FrameError::BadMagic)
                | Err(daq_proto::FrameError::BadCrc { .. })
                | Err(daq_proto::FrameError::PayloadTooLong(_)) => {
                    // Resync: drop one byte and retry.
                    if self.rx.is_empty() {
                        break;
                    }
                    self.rx.remove(0);
                }
            }
        }
        out
    }
}

impl Drop for DaqUsbConnection {
    fn drop(&mut self) {
        self.release_lease();
    }
}

impl DaqTransport for DaqUsbConnection {
    fn read_records(&mut self) -> Result<Vec<DaqRecord>> {
        if self.interface.is_none() {
            return Err(anyhow!("DAQ USB not connected"));
        }
        let rt = tokio::runtime::Handle::current();
        loop {
            // Parse anything already buffered first.
            let ready = self.drain_frames();
            if !ready.is_empty() {
                return Ok(ready);
            }
            let queue = self
                .queue
                .as_mut()
                .ok_or_else(|| anyhow!("DAQ USB not connected"))?;
            // DESK-24: wait at most one short slice so the caller can release
            // the shared transport mutex; only DAQ_IDLE_TIMEOUT_MS of total
            // silence is an error (DESK-7 kept that at 1000 ms).
            let completion = match rt.block_on(tokio::time::timeout(
                std::time::Duration::from_millis(DAQ_READ_SLICE_MS),
                queue.next_complete(),
            )) {
                Ok(c) => c,
                Err(_) => {
                    let since = *self.idle_since.get_or_insert_with(Instant::now);
                    if since.elapsed() >= Duration::from_millis(DAQ_IDLE_TIMEOUT_MS) {
                        self.idle_since = None;
                        return Err(anyhow!("DAQ USB read timed out"));
                    }
                    return Ok(Vec::new());
                }
            };
            self.idle_since = None;
            // Resubmit immediately so the queue stays saturated with
            // QUEUE_DEPTH buffers in flight.
            queue.submit(RequestBuffer::new(QUEUE_BUF_LEN));
            let data = completion
                .into_result()
                .map_err(|e| anyhow!("DAQ bulk IN failed: {}", e))?;
            if data.is_empty() {
                return Err(anyhow!("DAQ bulk IN returned 0 bytes"));
            }
            self.rx.extend_from_slice(&data);
        }
    }

    fn send(&mut self, cmd_type: u8, payload: &[u8]) -> Result<()> {
        let seq = self.out_seq.fetch_add(1, Ordering::Relaxed);
        let frame = daq_proto::encode_command(seq, cmd_type, payload);
        let rt = tokio::runtime::Handle::current();

        // DESK-8 FIX: Escalate recovery instead of jumping straight to re-enumeration.
        // Stages: 1) first attempt, 2) immediate retry, 3) full re-enumerate.
        // Windows WinUsb_WritePipe returns ERROR_BAD_COMMAND (22) when the pipe handle
        // is invalid; clear_halt fails with the same error in that state, so we skip
        // straight to re-enumerate on that specific code. For other transient errors
        // (e.g. BUSY, TIMEOUT), retry once before escalating.

        // Stage 1: first attempt with current interface
        let first_err = {
            let iface = self
                .interface
                .as_ref()
                .ok_or_else(|| anyhow!("DAQ USB not connected"))?
                .clone();
            let c = tokio::task::block_in_place(|| rt.block_on(iface.bulk_out(DAQ_EP_OUT, frame.clone())));
            c.into_result().err()
        };

        if let Some(e) = first_err {
            let err_str = e.to_string();
            log::warn!("DAQ bulk OUT failed: {}", e);

            // Stage 2: Immediate retry for non-fatal errors
            // Skip retry for ERROR_BAD_COMMAND / ERROR_NO_SUCH_DEVICE - those need re-enum
            let needs_re_enum = err_str.contains("ERROR_BAD_COMMAND")
                || err_str.contains("ERROR_NO_SUCH_DEVICE")
                || err_str.contains("LIBUSB_ERROR_NO_DEVICE");

            if !needs_re_enum {
                log::info!("DAQ bulk OUT: retrying before re-enumeration...");
                std::thread::sleep(std::time::Duration::from_millis(100));

                let retry_result = {
                    let iface = self.interface.as_ref()
                        .ok_or_else(|| anyhow!("DAQ USB not connected during retry"))?
                        .clone();
                    let c = tokio::task::block_in_place(|| rt.block_on(iface.bulk_out(DAQ_EP_OUT, frame.clone())));
                    c.into_result()
                };

                if retry_result.is_ok() {
                    log::info!("DAQ bulk OUT: retry succeeded");
                    return Ok(());
                }

                if let Err(retry_err) = retry_result {
                    log::warn!("DAQ bulk OUT retry failed: {}", retry_err);
                }
            }

            // Stage 3: Re-enumerate as last resort
            log::warn!(
                "DAQ bulk OUT: re-enumerating USB (original error: {})...",
                e
            );
            self.interface = None;
            self.queue = None;
            self.connected = false;
            self.rx.clear();
            self.connect().map_err(|re| {
                anyhow!("DAQ bulk OUT failed ({}) and USB re-enumerate failed: {}", e, re)
            })?;

            // Final attempt with fresh interface
            let iface2 = self
                .interface
                .as_ref()
                .ok_or_else(|| anyhow!("DAQ USB not connected after re-enumerate"))?
                .clone();
            let c2 = tokio::task::block_in_place(|| rt.block_on(iface2.bulk_out(DAQ_EP_OUT, frame)));
            c2.into_result()
                .map_err(|e2| anyhow!("DAQ bulk OUT failed after re-enumerate: {}", e2))?;
        }
        Ok(())
    }
}

// =============================================================================
// Mock / synthetic DAQ source
// =============================================================================

/// Synthesises a realistic power-profile stream: a low-power sleep baseline with
/// periodic active bursts and the occasional inrush spike that exercises every
/// current range and the FINE/COARSE/BLEND fusion path.
pub struct MockDaqTransport {
    sample_rate: u32,
    voltage_rate: u32,
    decimation: u8,
    running: bool,
    started: Instant,
    /// Current-domain (WAVE_I) sample index.
    sample_idx: u64,
    /// Voltage-domain (WAVE_V) sample index; runs independently at
    /// `voltage_rate`.
    volt_idx: u64,
    /// Mock time (seconds since start) at which the next synthetic index
    /// skip should fire, to exercise the gap-handling path end-to-end.
    next_skip_at: f64,
    seq: u32,
    // SMU / source
    vdut: f32,
    source_enabled: bool,
    range_lock: u8, // 0xFF = auto
    // DSP
    fft_enabled: bool,
    fft_nbins: u16,
    fft_source: u8,
    fft_window: u8,
    // accumulators
    energy_j: f64,
    charge_c: f64,
    last_aux: Instant,
}

impl Default for MockDaqTransport {
    fn default() -> Self {
        Self::new()
    }
}

impl MockDaqTransport {
    pub fn new() -> Self {
        Self {
            sample_rate: 250_000,
            voltage_rate: 64_000,
            decimation: 1,
            running: false,
            started: Instant::now(),
            sample_idx: 0,
            volt_idx: 0,
            next_skip_at: 5.0,
            seq: 0,
            vdut: 3.3,
            source_enabled: true,
            range_lock: 0xFF,
            fft_enabled: true,
            fft_nbins: 256,
            fft_source: 0,
            fft_window: 1,
            energy_j: 0.0,
            charge_c: 0.0,
            last_aux: Instant::now(),
        }
    }

    /// Effective current at absolute time `t` seconds — a duty-cycled load that
    /// exercises autoranging (sleep on the Fine/HI path, active bursts on the
    /// Coarse/LO path) without unrealistic outliers that wreck auto-scaling.
    fn current_at(&self, t: f64) -> f32 {
        if !self.source_enabled {
            return 0.0;
        }
        // Sleep baseline ~120 µA (RANGE_HI / Fine path).
        let base = 120e-6_f64;
        let period = 0.200; // 5 Hz activity
        let ph = t % period;
        let active_w = 0.040; // 40 ms active window
        let mut i = base;
        if ph < active_w {
            // ~42 mA active (RANGE_LO / Coarse) with a gentle inrush at the
            // leading edge and a little switching ripple.
            let active = 0.042;
            let inrush = if ph < 0.0015 {
                1.0 + 0.6 * (1.0 - ph / 0.0015)
            } else {
                1.0
            };
            let ripple = 1.0 + 0.04 * (2.0 * std::f64::consts::PI * 1_500.0 * t).sin();
            i = active * inrush * ripple;
        }
        // Small broadband noise proportional to the sleep floor.
        let noise = base * 0.4 * ((t * 77_003.0).sin() + (t * 41_117.0).cos());
        (i + noise).max(0.0) as f32
    }

    fn range_for(&self, i: f32) -> u8 {
        if self.range_lock != 0xFF {
            return self.range_lock;
        }
        if i < 1.4e-3 {
            RANGE_HI
        } else if i < 37e-3 {
            RANGE_MID
        } else {
            RANGE_LO
        }
    }

    /// Pack a WAVE_I meta byte: bits 0-1 range, bits 2-3 source, bit 4
    /// saturated, bit 5 settling.
    fn pack_meta(range: u8, source: u8, saturated: bool, settling: bool) -> u8 {
        (range & 0x03)
            | ((source & 0x03) << 2)
            | if saturated { META_SATURATED } else { 0 }
            | if settling { META_SETTLING } else { 0 }
    }

    /// Synthesise `count` current-domain samples starting at `self.sample_idx`,
    /// exercising a synthetic ~5 s-periodic index skip so the gap-handling
    /// path has something to render in mock mode too.
    fn synth_wave_i(&mut self, count: usize) -> WaveIRecord {
        let dec = self.decimation.max(1) as f64;
        let eff_rate = (self.sample_rate as f64 / dec).max(1.0);
        let dt = 1.0_f64 / eff_rate;

        // Every ~5 s of mock time, drop 10_000 current indexes without
        // emitting samples for them, simulating a FIFO overrun / drop.
        let now = self.started.elapsed().as_secs_f64();
        if now >= self.next_skip_at {
            self.sample_idx += 10_000;
            self.next_skip_at = now + 5.0;
        }

        let timestamp_us = self.started.elapsed().as_micros() as u64;
        let start_index = self.sample_idx;
        let mut i_vals = Vec::with_capacity(count);
        let mut meta = Vec::with_capacity(count);
        let mut prev_range = if self.sample_idx == 0 {
            RANGE_HI
        } else {
            self.range_for(self.current_at(self.sample_idx as f64 * dt))
        };
        for k in 0..count {
            let n = self.sample_idx + k as u64;
            let t = n as f64 * dt;
            let i = self.current_at(t);
            let range = self.range_for(i);
            // During a range transition the FINE path is settling, so COARSE
            // carries the signal (BLEND at the seams) — gap is filled, never lost.
            let (source, settling) = if range != prev_range {
                (SRC_BLEND, true)
            } else if range == RANGE_LO {
                (SRC_COARSE, false)
            } else {
                (SRC_FINE, false)
            };
            prev_range = range;
            let saturated = i > 2.4;
            i_vals.push(i);
            meta.push(Self::pack_meta(range, source, saturated, settling));

            // Accumulate energy / charge using the same voltage model as
            // synth_wave_v so the aux records stay consistent.
            let v = (self.vdut - i * 0.05).max(0.0);
            self.energy_j += (i * v) as f64 * dt;
            self.charge_c += i as f64 * dt;
        }
        self.sample_idx += count as u64;
        WaveIRecord {
            start_index,
            timestamp_us,
            sample_rate: eff_rate as u32,
            decimation: self.decimation,
            i: i_vals,
            meta,
        }
    }

    /// Synthesise `count` voltage-domain samples at `voltage_rate`, running
    /// independently of the current-domain index.
    fn synth_wave_v(&mut self, count: usize) -> WaveVRecord {
        let v_rate = self.voltage_rate.max(1) as f64;
        let dt = 1.0_f64 / v_rate;
        let timestamp_us = self.started.elapsed().as_micros() as u64;
        let start_index = self.volt_idx;
        let mut v_vals = Vec::with_capacity(count);
        for k in 0..count {
            let n = self.volt_idx + k as u64;
            let t = n as f64 * dt;
            let i_v = self.current_at(t);
            v_vals.push((self.vdut - i_v * 0.05).max(0.0));
        }
        self.volt_idx += count as u64;
        WaveVRecord {
            start_index,
            timestamp_us,
            sample_rate: self.voltage_rate,
            v: v_vals,
        }
    }

    fn synth_stats(&self) -> StatsRecord {
        // Cheap representative block; the front-end mostly shows last values.
        let blk = |mean: f32, max: f32| StatBlock {
            min: 0.0,
            max,
            mean,
            rms: mean * 1.1,
            std: mean * 0.3,
            count: self.sample_rate,
        };
        StatsRecord {
            i: blk(0.010, 1.6),
            v: blk(self.vdut, self.vdut),
            p: blk(0.033, self.vdut * 1.6),
        }
    }

    fn synth_energy(&self) -> EnergyRecord {
        let elapsed = self.started.elapsed().as_secs_f64();
        let last_i = self.current_at(elapsed);
        let last_v = (self.vdut - last_i * 0.05).max(0.0);
        EnergyRecord {
            energy_mwh: self.energy_j / 3.6,
            energy_j: self.energy_j,
            charge_mah: self.charge_c / 3.6,
            charge_c: self.charge_c,
            elapsed_s: elapsed,
            last_i,
            last_v,
            last_p: last_i * last_v,
        }
    }

    fn synth_fft(&self) -> FftRecord {
        let n = self.fft_nbins.max(2) as usize;
        let mut bins = Vec::with_capacity(n);
        for k in 0..n {
            let f = k as f32 / n as f32;
            // DC-heavy with a peak near the 5 Hz burst rate and 2 kHz ripple.
            let burst = 0.6 * (-((f - 0.001).abs() * 400.0)).exp();
            let ripple = 0.3 * (-((f - 0.016).abs() * 120.0)).exp();
            let floor = 0.02 / (1.0 + 50.0 * f);
            bins.push(burst + ripple + floor);
        }
        FftRecord {
            sample_rate: self.sample_rate,
            source: self.fft_source,
            window: self.fft_window,
            bins,
        }
    }

    fn synth_status(&self) -> StatusRecord {
        let i_out = self.current_at(self.started.elapsed().as_secs_f64());
        let v_out = (self.vdut - i_out * 0.05).max(0.0);
        // Synthesised input-rail sense: a ~5 V supply feeding the SMU at ~88 %
        // efficiency plus a small quiescent draw.
        let v_in = 5.0_f32;
        let i_in = ((v_out * i_out) / (v_in * 0.88) + 0.004).max(0.0);
        StatusRecord {
            sample_rate: self.sample_rate,
            overflow_count: 0,
            range: self.range_for(i_out),
            streaming: self.running,
            range_locked: self.range_lock != 0xFF,
            source_enabled: self.source_enabled,
            vdut_set: self.vdut,
            ilimit_set: 2.5,
            in_voltage: if self.source_enabled { v_in } else { 0.0 },
            in_current: if self.source_enabled { i_in } else { 0.0 },
            // Simulator: all ADAQs healthy, no errors.
            adaq_ok_bits: 0b111,
            fine_err_pct: 0,
            drop_fine: 0,
            drop_coarse: 0,
            fine_diag_sticky: 0,
            frames_tx: 0,
            bytes_per_sec: 0,
            fifo_drop_frames: 0,
            ring_high_water: 0,
            wave_i_index_lo: (self.sample_idx & 0xFFFF_FFFF) as u32,
            // Extension v7+v8: board temps and calibration status (mock values)
            board_temp_analog_c: Some(25.0),
            board_temp_power_c: Some(28.0),
            cal_have_hi: true,
            cal_have_mid: true,
            cal_have_lo: true,
            missed_conversions: Some(0),
        }
    }
    /// Emit synthetic event markers for any 5 Hz burst edge that falls inside
    /// the sample window `[start, start+count)`. Each burst start raises a FLAG
    /// on IO1; its end raises a FLAG on IO2; every 5th burst also fires a
    /// TRIGGER on IO3. Aligned to `current_at`'s 0.200 s period / 0.040 s active
    /// window so the lines land on the rising/falling edges of the load.
    fn synth_markers(&self, start: u64, count: usize, eff_rate: f64) -> Vec<DaqRecord> {
        if count == 0 {
            return Vec::new();
        }
        let dt = 1.0_f64 / eff_rate;
        let period = 0.200_f64;
        let active_w = 0.040_f64;
        let t0 = start as f64 * dt;
        let t1 = (start + count as u64) as f64 * dt;
        let mk = |idx: u64, ch: u8, edge: u8, kind: u8| {
            DaqRecord::Marker(MarkerRecord {
                sample_index: idx,
                timestamp_us: (idx as f64 * dt * 1e6) as u64,
                channel: ch,
                edge,
                kind,
            })
        };
        let mut out = Vec::new();
        let first = (t0 / period).floor() as i64;
        let last = (t1 / period).ceil() as i64;
        for b in first..=last {
            if b < 0 {
                continue;
            }
            let t_rise = b as f64 * period;
            let t_fall = t_rise + active_w;
            if t_rise >= t0 && t_rise < t1 {
                let idx = (t_rise / dt).round() as u64;
                out.push(mk(idx, 1, 1, MARK_KIND_FLAG));
                if b % 5 == 0 {
                    out.push(mk(idx, 3, 1, MARK_KIND_TRIGGER));
                }
            }
            if t_fall >= t0 && t_fall < t1 {
                let idx = (t_fall / dt).round() as u64;
                out.push(mk(idx, 2, 0, MARK_KIND_FLAG));
            }
        }
        out
    }
}

impl DaqTransport for MockDaqTransport {
    fn read_records(&mut self) -> Result<Vec<DaqRecord>> {
        // Pace at ~30 ms cadence to mimic a live stream without pegging a core.
        std::thread::sleep(std::time::Duration::from_millis(30));
        if !self.running {
            // Idle: still emit a heartbeat so the UI shows "connected, stopped".
            return Ok(vec![DaqRecord::Status(self.synth_status())]);
        }
        let mut out = Vec::new();
        // ~30 ms of samples at the effective (decimated) rate, capped per frame.
        let dec = self.decimation.max(1) as f64;
        let eff_rate = (self.sample_rate as f64 / dec).max(1.0);
        let count = ((eff_rate * 0.030) as usize).clamp(1, 4096);
        let win_start = self.sample_idx;
        out.push(DaqRecord::WaveI(self.synth_wave_i(count)));

        let v_count = ((self.voltage_rate as f64 * 0.030) as usize).clamp(1, 4096);
        out.push(DaqRecord::WaveV(self.synth_wave_v(v_count)));

        // Synthetic event markers aligned to the 5 Hz activity bursts so the
        // flag/trigger overlay has something to render in mock mode.
        out.extend(self.synth_markers(win_start, count, eff_rate));

        // Aux records ~5 Hz.
        if self.last_aux.elapsed().as_millis() >= 200 {
            self.last_aux = Instant::now();
            out.push(DaqRecord::Stats(self.synth_stats()));
            out.push(DaqRecord::Energy(self.synth_energy()));
            out.push(DaqRecord::Status(self.synth_status()));
            if self.fft_enabled {
                out.push(DaqRecord::Fft(self.synth_fft()));
            }
        }
        Ok(out)
    }

    fn send(&mut self, cmd_type: u8, payload: &[u8]) -> Result<()> {
        match cmd_type {
            daq_proto::CMD_START => {
                self.running = true;
                self.started = Instant::now();
                self.sample_idx = 0;
                self.volt_idx = 0;
                self.next_skip_at = 5.0;
                self.energy_j = 0.0;
                self.charge_c = 0.0;
            }
            daq_proto::CMD_STOP => self.running = false,
            daq_proto::CMD_SET_RATE if payload.len() >= 9 => {
                let sps = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
                if sps > 0 {
                    self.sample_rate = sps;
                }
                let vsps = u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]);
                if vsps > 0 {
                    self.voltage_rate = vsps;
                }
                self.decimation = payload[8].max(1);
            }
            daq_proto::CMD_RANGE_LOCK if !payload.is_empty() => {
                self.range_lock = payload[0];
            }
            daq_proto::CMD_RESET_ENERGY => {
                self.energy_j = 0.0;
                self.charge_c = 0.0;
            }
            daq_proto::CMD_FFT_CONFIG if payload.len() >= 5 => {
                self.fft_nbins = u16::from_le_bytes([payload[0], payload[1]]);
                self.fft_source = payload[2];
                self.fft_window = payload[3];
                self.fft_enabled = payload[4] != 0;
            }
            daq_proto::CMD_SET_SOURCE if payload.len() >= 9 => {
                self.vdut = f32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
                self.source_enabled = payload[8] != 0;
            }
            _ => {}
        }
        let _ = self.seq.wrapping_add(1);
        Ok(())
    }
}

#[cfg(test)]
mod lock_hold_tests {
    /// DESK-24: the ingest thread holds the transport mutex for a whole
    /// `read_records` call, so a 1 s blocking read starved control commands
    /// (range lock, rate, stop) for up to 1 s while the stream was idle.
    #[test]
    fn usb_read_slice_bounds_lock_hold() {
        const { assert!(super::DAQ_IDLE_TIMEOUT_MS >= 1000) };
        assert!(super::DAQ_READ_SLICE_MS <= 100, "slice {} ms", super::DAQ_READ_SLICE_MS);
    }
}

#[cfg(test)]
mod mock_tests {
    use super::*;

    /// After CMD_START, read_records should yield both WaveI and WaveV
    /// records whose start_index is contiguous with the previous block's end
    /// index, except across the deliberate synthetic skip.
    #[test]
    fn mock_start_yields_contiguous_wave_i_and_wave_v() {
        let mut mock = MockDaqTransport::new();
        mock.send(daq_proto::CMD_START, &[]).unwrap();

        let mut prev_i_end: Option<u64> = None;
        let mut prev_v_end: Option<u64> = None;
        let mut saw_wave_i = false;
        let mut saw_wave_v = false;
        let mut saw_skip = false;

        for _ in 0..20 {
            let recs = mock.read_records().unwrap();
            for rec in recs {
                match rec {
                    DaqRecord::WaveI(w) => {
                        saw_wave_i = true;
                        assert!(!w.i.is_empty());
                        assert_eq!(w.i.len(), w.meta.len());
                        if let Some(end) = prev_i_end {
                            if w.start_index != end {
                                // Only the deliberate 10_000-index skip
                                // should break continuity.
                                assert_eq!(w.start_index, end + 10_000);
                                saw_skip = true;
                            }
                        }
                        prev_i_end = Some(w.start_index + w.i.len() as u64);
                    }
                    DaqRecord::WaveV(w) => {
                        saw_wave_v = true;
                        assert!(!w.v.is_empty());
                        if let Some(end) = prev_v_end {
                            assert_eq!(w.start_index, end);
                        }
                        prev_v_end = Some(w.start_index + w.v.len() as u64);
                    }
                    _ => {}
                }
            }
        }

        assert!(saw_wave_i, "expected at least one WaveI record");
        assert!(saw_wave_v, "expected at least one WaveV record");
        // The skip is time-gated (~5s of mock time) and this test only runs
        // a handful of ~30ms iterations, so it isn't expected to fire here;
        // this just documents the invariant checked above when it does.
        let _ = saw_skip;
    }
}

#[cfg(test)]
mod lease_tests {
    use super::*;
    use crate::daq_proto::{
        LEASE_FAIL_MALFORMED, LEASE_FAIL_NONE, LEASE_FAIL_NOT_READY, LEASE_FAIL_TABLE_FULL,
        LEASE_OP_ACQUIRE as ACQ, LEASE_OP_RELEASE as REL,
    };

    fn ack(failure: u8, ready: bool) -> StandbyAckRecord {
        StandbyAckRecord {
            schema: 1,
            state: if ready { 0 } else { 3 },
            ready,
            failure,
            generation: 4,
            inhibitors: 2,
            activity: 0,
        }
    }

    fn s(op: u8, client_id: u32) -> Option<LeaseSend> {
        Some(LeaseSend { op, client_id })
    }

    fn at(t0: Instant, secs: u64) -> Instant {
        t0 + Duration::from_secs(secs)
    }

    #[test]
    fn acquire_goes_out_once_and_waits_for_its_ack() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        assert_eq!(c.poll(t0), s(ACQ, 77));
        assert_eq!(c.poll(at(t0, 1)), None, "one exchange at a time");
        assert_eq!(c.phase(), LeasePhase::Opening);
    }

    #[test]
    fn a_held_lease_is_refreshed_every_ten_seconds_with_the_same_id() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        c.poll(t0);
        assert_eq!(c.on_ack(1, ack(LEASE_FAIL_NONE, true), t0), AckOutcome::Applied(LeasePhase::Held));
        assert_eq!(c.poll(at(t0, 9)), None);
        assert_eq!(c.poll(at(t0, 10)), s(ACQ, 77));
        c.on_ack(1, ack(LEASE_FAIL_NONE, true), at(t0, 10));
        assert_eq!(c.poll(at(t0, 19)), None);
        assert_eq!(c.poll(at(t0, 20)), s(ACQ, 77));
        assert_eq!(c.last_ack().map(|a| a.inhibitors), Some(2));
    }

    #[test]
    fn accepted_but_waking_is_a_lease_not_a_retry_trigger() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        c.poll(t0);
        let out = c.on_ack(1, ack(LEASE_FAIL_NOT_READY, false), t0);
        assert_eq!(out, AckOutcome::Applied(LeasePhase::Waking));
        // Nothing is re-sent early because the system is not ready yet.
        assert_eq!(c.poll(at(t0, 3)), None);
        assert_eq!(c.poll(at(t0, 10)), s(ACQ, 77));
        // failure 0 with ready 0 is also "accepted, not ready", never "held".
        c.on_ack(1, ack(LEASE_FAIL_NONE, false), at(t0, 10));
        assert_eq!(c.phase(), LeasePhase::Waking);
        assert!(!c.last_ack().unwrap().ready);
    }

    #[test]
    fn a_full_table_refuses_without_evicting_and_holds_nothing() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        c.poll(t0);
        let out = c.on_ack(1, ack(LEASE_FAIL_TABLE_FULL, false), t0);
        assert_eq!(out, AckOutcome::Applied(LeasePhase::Full));
        assert_eq!(c.poll(at(t0, 10)), s(ACQ, 77), "retried per period, never forced");
        assert_eq!(c.close(), None, "no lease was granted, so nothing to release");
    }

    #[test]
    fn a_malformed_rejection_is_an_error_and_is_not_retried() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        c.poll(t0);
        assert_eq!(
            c.on_ack(1, ack(LEASE_FAIL_MALFORMED, false), t0),
            AckOutcome::Applied(LeasePhase::Rejected)
        );
        assert_eq!(c.poll(at(t0, 100)), None);
    }

    #[test]
    fn a_lost_ack_resends_the_lease_frame_a_bounded_number_of_times() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        assert_eq!(c.poll(t0), s(ACQ, 77));
        assert_eq!(c.poll(at(t0, 1)), None);
        assert_eq!(c.poll(at(t0, 2)), s(ACQ, 77), "resend 1");
        assert_eq!(c.poll(at(t0, 4)), s(ACQ, 77), "resend 2");
        assert_eq!(c.poll(at(t0, 6)), None, "gives up; no third resend");
        assert_eq!(c.phase(), LeasePhase::AckLost);
        assert_eq!(c.poll(at(t0, 15)), None, "next attempt waits a full period");
        assert_eq!(c.poll(at(t0, 16)), s(ACQ, 77));
    }

    #[test]
    fn an_ack_after_the_exchange_gave_up_changes_nothing() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        c.poll(t0);
        c.poll(at(t0, 2));
        c.poll(at(t0, 4));
        c.poll(at(t0, 6));
        assert_eq!(c.on_ack(1, ack(LEASE_FAIL_NONE, true), at(t0, 7)), AckOutcome::Ignored);
        assert_eq!(c.phase(), LeasePhase::AckLost);
    }

    #[test]
    fn an_ack_read_under_another_epoch_cannot_update_this_one() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(5, 77, t0, vec![]);
        c.poll(t0);
        assert_eq!(c.on_ack(4, ack(LEASE_FAIL_NONE, true), t0), AckOutcome::Ignored);
        assert_eq!(c.phase(), LeasePhase::Opening, "still waiting for its own ACK");
        assert_eq!(c.on_ack(5, ack(LEASE_FAIL_NONE, true), t0), AckOutcome::Applied(LeasePhase::Held));
    }

    #[test]
    fn an_unsolicited_ack_is_ignored() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(1, 77, t0, vec![]);
        assert_eq!(c.on_ack(1, ack(LEASE_FAIL_NONE, true), t0), AckOutcome::Ignored);
        c.poll(t0);
        c.on_ack(1, ack(LEASE_FAIL_NONE, true), t0);
        assert_eq!(c.on_ack(1, ack(LEASE_FAIL_TABLE_FULL, false), t0), AckOutcome::Ignored);
        assert_eq!(c.phase(), LeasePhase::Held);
    }

    #[test]
    fn earlier_epoch_ids_are_released_one_at_a_time_before_acquiring() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(2, 77, t0, vec![5, 6]);
        assert_eq!(c.poll(t0), s(REL, 5));
        assert_eq!(c.poll(t0), None, "serialized behind the first exchange");
        c.on_ack(2, ack(LEASE_FAIL_NONE, true), t0);
        assert_eq!(c.phase(), LeasePhase::Opening, "an owed release says nothing about this lease");
        assert_eq!(c.poll(t0), s(REL, 6));
        c.on_ack(2, ack(LEASE_FAIL_NONE, true), t0);
        assert_eq!(c.poll(t0), s(ACQ, 77));
    }

    #[test]
    fn a_lost_owed_release_is_abandoned_and_the_new_lease_still_starts() {
        let t0 = Instant::now();
        let mut c = LeaseCore::new(2, 77, t0, vec![5]);
        assert_eq!(c.poll(t0), s(REL, 5));
        assert_eq!(c.poll(at(t0, 2)), s(REL, 5));
        assert_eq!(c.poll(at(t0, 4)), s(REL, 5));
        assert_eq!(c.poll(at(t0, 6)), None, "gives up (the device TTL covers it)");
        assert_eq!(c.poll(at(t0, 6)), s(ACQ, 77));
    }

    #[test]
    fn closing_releases_only_a_lease_the_device_may_hold() {
        let t0 = Instant::now();
        let mut never = LeaseCore::new(1, 77, t0, vec![]);
        assert_eq!(never.close(), None, "nothing was ever sent");
        assert!(never.is_closed());

        let mut c = LeaseCore::new(1, 78, t0, vec![]);
        c.poll(t0);
        c.on_ack(1, ack(LEASE_FAIL_NONE, true), t0);
        assert_eq!(c.close(), s(REL, 78));
        assert_eq!(c.close(), None, "once");
        assert_eq!(c.poll(at(t0, 100)), None);
        assert_eq!(c.on_ack(1, ack(LEASE_FAIL_NONE, true), t0), AckOutcome::Ignored);
        assert_eq!(c.phase(), LeasePhase::Released);

        // Acquire in flight (ACK not seen yet) may already be granted: release it too.
        let mut inflight = LeaseCore::new(1, 79, t0, vec![]);
        inflight.poll(t0);
        assert_eq!(inflight.close(), s(REL, 79));
    }

    #[test]
    fn the_new_lease_epochs_never_reuse_the_counter() {
        let a = LEASE_EPOCH.fetch_add(1, Ordering::AcqRel);
        let b = LEASE_EPOCH.fetch_add(1, Ordering::AcqRel);
        assert!(b > a);
    }
}
