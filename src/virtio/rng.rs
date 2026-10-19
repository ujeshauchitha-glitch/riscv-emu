//! virtio-rng: an entropy source.
//!
//! A booting Linux wants randomness early - for address-space randomisation, for TCP
//! sequence numbers, for the first `ssh-keygen` - and an emulated machine has none of
//! the hardware sources a real one does. Without this device, a Debian boot can sit
//! for minutes at "random: crng init" waiting for entropy that never arrives.
//!
//! The guest hands over a buffer; the device fills it with random bytes.

use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hasher};

use super::{Core, VirtioDevice};

const RNG_DEVICE: u32 = 4;
const FALLBACK_STATE: u64 = 0x9e37_79b9_7f4a_7c15;

/// 64 bits from the host. The standard library seeds every `RandomState` from the
/// operating system's entropy source, which makes it a dependency-free way to get some.
pub fn host_entropy() -> u64 {
    RandomState::new().build_hasher().finish()
}

pub struct VirtioRng {
    state: u64,
}

impl Default for VirtioRng {
    fn default() -> Self {
        VirtioRng::new()
    }
}

impl VirtioRng {
    pub fn new() -> VirtioRng {
        let state = host_entropy();
        VirtioRng { state: if state == 0 { FALLBACK_STATE } else { state } }
    }

    /// Make the stream reproducible.
    pub fn seed(&mut self, value: u64) {
        self.state = if value == 0 { FALLBACK_STATE } else { value };
    }

    /// xorshift64*: not cryptographically strong, and it does not need to be - this is
    /// the entropy a guest uses to seed its own generator, from a host that is also
    /// where the guest's every secret already lives.
    pub fn next_u64(&mut self) -> u64 {
        self.state ^= self.state >> 12;
        self.state ^= self.state << 25;
        self.state ^= self.state >> 27;
        self.state.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }
}

impl VirtioDevice for VirtioRng {
    fn name(&self) -> &'static str {
        "virtio-rng"
    }
    fn device_id(&self) -> u32 {
        RNG_DEVICE
    }
    fn num_queues(&self) -> u32 {
        1
    }

    fn notify(&mut self, core: &mut Core, queue: u32) {
        let mut any = false;
        while let Some((head, segs)) = core.pop_chain(queue) {
            any = true;
            let mut written = 0u32;
            for s in segs.iter().filter(|s| s.device_writes) {
                let mut bytes = vec![0u8; s.len as usize];
                for chunk in bytes.chunks_mut(8) {
                    let r = self.next_u64().to_le_bytes();
                    chunk.copy_from_slice(&r[..chunk.len()]);
                }
                if core.write_guest_bytes(s.addr, &bytes) {
                    written += s.len;
                }
            }
            core.push_used(queue, head, written);
        }
        if any {
            core.signal_used(queue);
        }
    }
}
