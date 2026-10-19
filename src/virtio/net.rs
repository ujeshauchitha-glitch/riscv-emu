//! virtio-net: a network card.
//!
//! Two queues: the guest posts empty buffers on the receive queue for incoming frames
//! and full ones on the transmit queue to send. Every frame is prefixed with a
//! 12-byte virtio-net header (offload flags we do not use, and a buffer count).
//!
//! What the card is connected *to* is a `NetBackend`: `LoopbackBackend` returns each
//! sent frame to the guest, and `usernet::UserNet` is a built-in user-mode network
//! stack that gives the guest real internet access without any host setup.

use std::collections::VecDeque;

use super::{Core, VirtioDevice};

const NET_DEVICE: u32 = 1;
const RX_QUEUE: u32 = 0;
const TX_QUEUE: u32 = 1;

const F_MAC: u64 = 1 << 5; // the config space holds the card's MAC address
const F_STATUS: u64 = 1 << 16; // the config space holds a link-up flag
const LINK_UP: u16 = 1;

pub const HEADER_BYTES: usize = 12;
const MAX_PENDING: usize = 512;

/// The far side of the card.
pub trait NetBackend {
    /// The guest sent an Ethernet frame.
    fn send(&mut self, frame: &[u8]);
    /// The next frame for the guest, if one has arrived.
    fn receive(&mut self) -> Option<Vec<u8>>;
    /// True while a reply from the outside world is still expected (so an idle machine
    /// lets real time pass instead of skipping its clock past the guest's timeout).
    fn io_pending(&self) -> bool {
        false
    }
}

/// Every frame the guest sends comes straight back. Useful for testing a driver with
/// no network behind it.
#[derive(Default)]
pub struct LoopbackBackend {
    queue: VecDeque<Vec<u8>>,
}

impl LoopbackBackend {
    pub fn new() -> LoopbackBackend {
        LoopbackBackend::default()
    }
}

impl NetBackend for LoopbackBackend {
    fn send(&mut self, frame: &[u8]) {
        self.queue.push_back(frame.to_vec());
    }
    fn receive(&mut self) -> Option<Vec<u8>> {
        self.queue.pop_front()
    }
}

pub struct VirtioNet {
    backend: Box<dyn NetBackend>,
    mac: [u8; 6],
    /// Frames received from the backend that are waiting for the guest to post a
    /// buffer to put them in.
    pending: VecDeque<Vec<u8>>,
    dropped: u64,
}

impl VirtioNet {
    pub fn new(backend: Box<dyn NetBackend>, mac: [u8; 6]) -> VirtioNet {
        VirtioNet { backend, mac, pending: VecDeque::new(), dropped: 0 }
    }

    pub fn mac(&self) -> [u8; 6] {
        self.mac
    }

    /// Frames dropped because the guest's buffer was too small for them.
    pub fn dropped(&self) -> u64 {
        self.dropped
    }

    fn transmit(&mut self, core: &mut Core) {
        let mut any = false;
        while let Some((head, segs)) = core.pop_chain(TX_QUEUE) {
            any = true;
            // The frame is every driver-written byte in the chain, after the header.
            let total = Core::segments_length(&segs, false) as usize;
            if total > HEADER_BYTES {
                let mut packet = vec![0u8; total];
                if core.read_from_segments(&segs, 0, &mut packet) == total as u64 {
                    self.backend.send(&packet[HEADER_BYTES..]);
                }
            }
            core.push_used(TX_QUEUE, head, 0);
        }
        if any {
            core.signal_used(TX_QUEUE);
        }
    }

    fn deliver(&mut self, core: &mut Core) {
        if !core.queue_ready(RX_QUEUE) {
            return;
        }
        // First anything the backend has produced since we last looked.
        while self.pending.len() < MAX_PENDING {
            match self.backend.receive() {
                Some(f) => self.pending.push_back(f),
                None => break,
            }
        }

        let mut any = false;
        while !self.pending.is_empty() {
            let Some((head, segs)) = core.pop_chain(RX_QUEUE) else {
                break;
            };
            let f = self.pending.pop_front().unwrap();

            // The device-writable space must hold the header and the whole frame; a
            // frame that does not fit is dropped rather than truncated, since a
            // truncated Ethernet frame is worse than a missing one.
            let room = Core::segments_length(&segs, true);
            if room >= (HEADER_BYTES + f.len()) as u64 {
                let mut header = [0u8; HEADER_BYTES];
                header[10] = 1; // num_buffers = 1 (low byte; high is 0)
                core.write_to_segments(&segs, 0, &header);
                core.write_to_segments(&segs, HEADER_BYTES as u64, &f);
                core.push_used(RX_QUEUE, head, (HEADER_BYTES + f.len()) as u32);
                any = true;
            } else {
                self.dropped += 1;
                core.push_used(RX_QUEUE, head, 0);
            }
        }
        if any {
            core.signal_used(RX_QUEUE);
        }
    }
}

impl VirtioDevice for VirtioNet {
    fn name(&self) -> &'static str {
        "virtio-net"
    }
    fn device_id(&self) -> u32 {
        NET_DEVICE
    }
    fn num_queues(&self) -> u32 {
        2
    }
    fn features(&self) -> u64 {
        F_MAC | F_STATUS
    }

    // struct virtio_net_config:  u8 mac[6];  u16 status;
    fn config_read(&self, offset: u64, size: usize) -> u64 {
        let mut config = [0u8; 8];
        config[..6].copy_from_slice(&self.mac);
        config[6..8].copy_from_slice(&LINK_UP.to_le_bytes());
        if offset >= config.len() as u64 || size == 0 || size > 8 {
            return 0;
        }
        let mut value = 0u64;
        for i in 0..size {
            if let Some(b) = config.get(offset as usize + i) {
                value |= (*b as u64) << (8 * i);
            }
        }
        value
    }

    fn notify(&mut self, core: &mut Core, queue: u32) {
        if queue == TX_QUEUE {
            self.transmit(core);
        } else if queue == RX_QUEUE {
            // New receive buffers: frames that were waiting for somewhere to go can go.
            self.deliver(core);
        }
    }

    fn poll(&mut self, core: &mut Core) {
        self.deliver(core);
    }

    fn io_pending(&self) -> bool {
        self.backend.io_pending()
    }
}
