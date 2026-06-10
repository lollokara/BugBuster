use anyhow::Result;
use std::sync::{Arc, Mutex};

use crate::la_usb::{LaStreamPacket, LaUsbConnection};

pub trait LaTransport: Send + 'static {
    fn send_command(&mut self, cmd: u8) -> Result<()>;
    fn read_packet(&mut self) -> Result<LaStreamPacket>;
}

pub struct LockedLaTransport {
    pub usb: Arc<Mutex<LaUsbConnection>>,
}

impl LaTransport for LockedLaTransport {
    fn send_command(&mut self, _cmd: u8) -> Result<()> {
        Err(anyhow::anyhow!("USB not supported on iOS"))
    }

    fn read_packet(&mut self) -> Result<LaStreamPacket> {
        Err(anyhow::anyhow!("USB not supported on iOS"))
    }
}

#[derive(Debug, PartialEq)]
pub enum StreamStopReason {
    Normal,
    HostStopped,
    UsbError(String),
    SeqMismatch { expected: u8, got: u8 },
    FirmwareError(u8),
}
