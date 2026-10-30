//! The virtio devices, driven the way a guest driver drives them: by building virtqueues in guest
//! memory, writing the device's registers, and reading back what it left in the used ring.

use std::cell::RefCell;
use std::rc::Rc;

use riscv_emu::bus::{Bus, Shared};
use riscv_emu::csr::{csr, CsrFile};
use riscv_emu::device::{AccessType, Device};
use riscv_emu::plic::Plic;
use riscv_emu::rtc::Rtc;
use riscv_emu::types::*;
use riscv_emu::virtio::blk::{VirtioBlk, SECTOR_SIZE};
use riscv_emu::virtio::net::{LoopbackBackend, VirtioNet};
use riscv_emu::virtio::rng::VirtioRng;
use riscv_emu::virtio::{VirtioDevice, VirtioMmio};

const F_NEXT: u16 = 1;
const F_WRITE: u16 = 2;

/// One buffer of a chain: (address, length, device writes it).
type Seg = (u64, u32, bool);

// Guest memory layout for the rig. Everything lives in the first MiB of DRAM.
const QUEUE_AREA: u64 = DRAM_BASE + 0x10000; // 0x10000 per queue
const BUFFER_AREA: u64 = DRAM_BASE + 0x60000;

#[derive(Clone, Copy)]
struct Queue {
    desc: u64,
    avail: u64,
    used: u64,
    num: u16,
    avail_idx: u16,
    next_desc: u16,
}

struct Rig<D: VirtioDevice + 'static> {
    bus: Bus,
    dev: Rc<RefCell<VirtioMmio<D>>>,
    queues: [Queue; 2],
}

impl<D: VirtioDevice + 'static> Rig<D> {
    fn new(device: D) -> Rig<D> {
        let mut bus = Bus::new(1024 * 1024);
        let (plic_dev, plic) = Shared::new(Plic::new(1));
        let mut dev = VirtioMmio::new(VIRTIO_BASE, bus.ram.clone(), device);
        dev.attach(plic.clone(), VIRTIO0_IRQ);
        let (dev_dev, dev) = Shared::new(dev);
        assert!(bus.attach(Box::new(plic_dev)) && bus.attach(Box::new(dev_dev)));
        let q = Queue { desc: 0, avail: 0, used: 0, num: 16, avail_idx: 0, next_desc: 0 };
        Rig { bus, dev, queues: [q, q] }
    }

    fn w32(&self, off: u64, v: u64) {
        self.dev.borrow_mut().store(off, 4, v).unwrap();
    }
    fn r32(&self, off: u64) -> u32 {
        self.dev.borrow_mut().load(off, 4).unwrap() as u32
    }
    fn load(&self, off: u64, size: usize) -> u64 {
        self.dev.borrow_mut().load(off, size).unwrap()
    }
    fn poll(&self) {
        self.dev.borrow_mut().poll();
    }

    fn poke(&self, addr: u64, size: usize, v: u64) {
        self.bus.store(addr, size, v).unwrap();
    }
    fn peek(&self, addr: u64, size: usize) -> u64 {
        self.bus.load(addr, size, AccessType::Load).unwrap()
    }

    fn setup_queue(&mut self, qi: usize) {
        let q = &mut self.queues[qi];
        q.desc = QUEUE_AREA + qi as u64 * 0x10000;
        q.avail = q.desc + 0x1000;
        q.used = q.desc + 0x2000;
        let q = *q;
        self.w32(0x030, qi as u64);
        self.w32(0x038, q.num as u64);
        self.w32(0x080, q.desc & 0xffff_ffff);
        self.w32(0x084, q.desc >> 32);
        self.w32(0x090, q.avail & 0xffff_ffff);
        self.w32(0x094, q.avail >> 32);
        self.w32(0x0a0, q.used & 0xffff_ffff);
        self.w32(0x0a4, q.used >> 32);
        self.w32(0x044, 1);
    }

    /// Post one descriptor chain and ring the doorbell. Returns the head index.
    fn post(&mut self, qi: usize, segs: &[Seg]) -> u16 {
        self.post_with(qi, segs, true)
    }

    fn post_with(&mut self, qi: usize, segs: &[Seg], notify: bool) -> u16 {
        let q = self.queues[qi];
        let head = q.next_desc;
        for (i, &(addr, len, device_writes)) in segs.iter().enumerate() {
            let index = (head as usize + i) % q.num as usize;
            let last = i + 1 == segs.len();
            let at = q.desc + index as u64 * 16;
            self.poke(at, 8, addr);
            self.poke(at + 8, 4, len as u64);
            self.poke(at + 12, 2, ((if device_writes { F_WRITE } else { 0 }) | if last { 0 } else { F_NEXT }) as u64);
            self.poke(at + 14, 2, ((head as usize + i + 1) % q.num as usize) as u64);
        }
        let q = &mut self.queues[qi];
        q.next_desc = ((q.next_desc as usize + segs.len()) % q.num as usize) as u16;
        let slot = q.avail + 4 + (q.avail_idx % q.num) as u64 * 2;
        q.avail_idx += 1;
        let (idx, avail) = (q.avail_idx, q.avail);
        self.poke(slot, 2, head as u64);
        self.poke(avail + 2, 2, idx as u64);
        if notify {
            self.w32(0x050, qi as u64);
        }
        head
    }

    fn used_idx(&self, qi: usize) -> u64 {
        self.peek(self.queues[qi].used + 2, 2)
    }
    fn used_len(&self, qi: usize, slot: u64) -> u64 {
        self.peek(self.queues[qi].used + 4 + slot * 8 + 4, 4)
    }

    fn read_bytes(&self, addr: u64, n: usize) -> Vec<u8> {
        (0..n as u64).map(|i| self.peek(addr + i, 1) as u8).collect()
    }
    fn write_bytes(&self, addr: u64, v: &[u8]) {
        for (i, b) in v.iter().enumerate() {
            self.poke(addr + i as u64, 1, *b as u64);
        }
    }
}

/// A block-device request header: type, reserved, sector.
fn write_blk_header<D: VirtioDevice + 'static>(rig: &Rig<D>, at: u64, kind: u32, sector: u64) {
    rig.poke(at, 4, kind as u64);
    rig.poke(at + 4, 4, 0);
    rig.poke(at + 8, 8, sector);
}

fn make_blk(sectors: u64) -> Rig<VirtioBlk> {
    let mut blk = VirtioBlk::new();
    // Byte i of the disk is derived from its sector and offset, so any misplaced or misordered copy
    // shows up as the wrong value.
    let data = blk.data_mut();
    data.resize((sectors * SECTOR_SIZE) as usize, 0);
    for (i, b) in data.iter_mut().enumerate() {
        *b = ((i as u64 / SECTOR_SIZE) * 7 + i as u64) as u8;
    }
    let mut rig = Rig::new(blk);
    rig.setup_queue(0);
    rig
}

const HDR: u64 = BUFFER_AREA;
const STATUS: u64 = BUFFER_AREA + 0x100;
const DATA1: u64 = BUFFER_AREA + 0x1000;
const DATA2: u64 = BUFFER_AREA + 0x4000;

fn disk(rig: &Rig<VirtioBlk>, i: usize) -> u8 {
    rig.dev.borrow().dev.data()[i]
}

// --- block: a request whose data is split over several buffers --------------------------

#[test]
fn blk_read_scatters_across_several_buffers() {
    // Linux sends one data descriptor per physically contiguous run of pages, so a large request
    // arrives as a chain of several. The device must follow the whole chain, not assume the xv6 shape
    // of header-data-status.
    let mut rig = make_blk(16);

    write_blk_header(&rig, HDR, 0, 2); // read, starting at sector 2
    rig.poke(STATUS, 1, 0xff);
    rig.post(
        0,
        &[
            (HDR, 16, false),
            (DATA1, 512, true),  // sector 2
            (DATA2, 1024, true), // sectors 3 and 4
            (STATUS, 1, true),
        ],
    );

    let a = rig.read_bytes(DATA1, 512);
    let b = rig.read_bytes(DATA2, 1024);
    for i in 0..512 {
        assert_eq!(a[i], disk(&rig, 2 * 512 + i));
    }
    for i in 0..1024 {
        assert_eq!(b[i], disk(&rig, 3 * 512 + i));
    }
    assert_eq!(rig.peek(STATUS, 1), 0);
    assert_eq!(rig.used_idx(0), 1);
    assert_eq!(rig.used_len(0, 0), 512 + 1024 + 1); // data plus the status byte
}

#[test]
fn blk_write_gathers_from_several_buffers() {
    let mut rig = make_blk(16);

    let first: Vec<u8> = (0..512).map(|i| (0x10 + i) as u8).collect();
    let second: Vec<u8> = (0..512).map(|i| (0xc0 - i as i32) as u8).collect();
    rig.write_bytes(DATA1, &first);
    rig.write_bytes(DATA2, &second);

    write_blk_header(&rig, HDR, 1, 5); // write, starting at sector 5
    rig.post(0, &[(HDR, 16, false), (DATA1, 512, false), (DATA2, 512, false), (STATUS, 1, true)]);

    assert_eq!(rig.peek(STATUS, 1), 0);
    for i in 0..512 {
        assert_eq!(disk(&rig, 5 * 512 + i), first[i]);
        assert_eq!(disk(&rig, 6 * 512 + i), second[i]);
    }
    // The neighbours are untouched.
    assert_eq!(disk(&rig, 4 * 512), (4 * 7 + 4 * 512) as u8);
    assert_eq!(disk(&rig, 7 * 512), (7 * 7 + 7 * 512) as u8);
}

#[test]
fn blk_flush_and_get_id() {
    let mut rig = make_blk(4);

    write_blk_header(&rig, HDR, 4, 0); // flush
    rig.poke(STATUS, 1, 0xff);
    rig.post(0, &[(HDR, 16, false), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 0);

    write_blk_header(&rig, HDR, 8, 0); // get id
    rig.post(0, &[(HDR, 16, false), (DATA1, 20, true), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 0);
    assert_eq!(rig.peek(DATA1, 1), b'r' as u64);

    write_blk_header(&rig, HDR, 99, 0); // not a request type
    rig.post(0, &[(HDR, 16, false), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 2); // UNSUPP
}

#[test]
fn blk_survives_hostile_chains() {
    let mut rig = make_blk(4);

    // A chain with no status byte at all: nowhere to report, so it completes empty rather than
    // crashing or writing somewhere guessed.
    write_blk_header(&rig, HDR, 0, 0);
    rig.post(0, &[(HDR, 16, false)]);
    assert_eq!(rig.used_idx(0), 1);
    assert_eq!(rig.used_len(0, 0), 0);

    // A header that is too short.
    rig.post(0, &[(HDR, 4, false), (STATUS, 1, true)]);
    assert_eq!(rig.used_idx(0), 2);

    // A data buffer pointing at memory that does not exist: an I/O error, not a host fault.
    write_blk_header(&rig, HDR, 0, 0);
    rig.poke(STATUS, 1, 0xff);
    rig.post(0, &[(HDR, 16, false), (0xdead_0000, 512, true), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 1);

    // A read whose buffers are not device-writable is backwards.
    rig.poke(STATUS, 1, 0xff);
    rig.post(0, &[(HDR, 16, false), (DATA1, 512, false), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 1);

    // A descriptor chain that loops back on itself must not hang the host. Build it by hand:
    // descriptor 0 chains to descriptor 0 forever.
    let q = rig.queues[0];
    rig.poke(q.desc, 8, HDR);
    rig.poke(q.desc + 8, 4, 16);
    rig.poke(q.desc + 12, 2, F_NEXT as u64);
    rig.poke(q.desc + 14, 2, 0);
    rig.poke(q.avail + 4 + (q.avail_idx % q.num) as u64 * 2, 2, 0);
    rig.poke(q.avail + 2, 2, q.avail_idx as u64 + 1);
    rig.w32(0x050, 0); // returns, which is the test
}

#[test]
fn blk_config_space() {
    let rig = make_blk(10);
    assert_eq!(rig.load(0x100, 8), 10); // capacity
    assert_eq!(rig.load(0x100 + 12, 4), 126); // seg_max
    assert_eq!(rig.load(0x100 + 20, 4), 512); // blk_size
    assert_eq!(rig.r32(0x034), 256); // QueueNumMax
}

// --- block: file-backed -----------------------------------------------------------------------

fn temp_image(name: &str, sectors: u64) -> std::path::PathBuf {
    let path = std::env::temp_dir().join(format!("riscv_emu_test_virtio_{}_{name}.img", std::process::id()));
    let data: Vec<u8> = (0..sectors * 512).map(|i| (i / 512 + 1) as u8).collect();
    std::fs::write(&path, data).unwrap();
    path
}

#[test]
fn blk_file_mode_persists_writes() {
    let path = temp_image("persist", 8);
    {
        let mut blk = VirtioBlk::new();
        blk.open_image(path.to_str().unwrap(), false).unwrap();
        assert_eq!(blk.sectors(), 8);
        let mut rig = Rig::new(blk);
        rig.setup_queue(0);

        rig.write_bytes(DATA1, &[0xab; 512]);
        write_blk_header(&rig, HDR, 1, 3);
        rig.post(0, &[(HDR, 16, false), (DATA1, 512, false), (STATUS, 1, true)]);
        assert_eq!(rig.peek(STATUS, 1), 0);

        // And it reads back what was there before, from the file.
        write_blk_header(&rig, HDR, 0, 6);
        rig.post(0, &[(HDR, 16, false), (DATA2, 512, true), (STATUS, 1, true)]);
        assert_eq!(rig.peek(DATA2, 1), 7); // sector 6 holds the byte 7

        write_blk_header(&rig, HDR, 4, 0);
        rig.post(0, &[(HDR, 16, false), (STATUS, 1, true)]);
    }
    let on_disk = std::fs::read(&path).unwrap();
    assert_eq!(on_disk.len(), 8 * 512);
    assert_eq!(on_disk[3 * 512], 0xab); // the write reached the file
    assert_eq!(on_disk[3 * 512 + 511], 0xab);
    assert_eq!(on_disk[2 * 512], 3); // neighbours untouched
    assert_eq!(on_disk[4 * 512], 5);
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn blk_snapshot_mode_never_modifies_the_file() {
    let path = temp_image("snapshot", 8);
    let before = std::fs::read(&path).unwrap();
    {
        let mut blk = VirtioBlk::new();
        blk.open_image(path.to_str().unwrap(), true).unwrap();
        let mut rig = Rig::new(blk);
        rig.setup_queue(0);

        // Overwrite a whole sector and half of another (via a 256-byte request at a sector boundary),
        // then read both back through the device.
        rig.write_bytes(DATA1, &[0x5a; 512]);
        write_blk_header(&rig, HDR, 1, 2);
        rig.post(0, &[(HDR, 16, false), (DATA1, 512, false), (STATUS, 1, true)]);
        assert_eq!(rig.peek(STATUS, 1), 0);

        rig.write_bytes(DATA1, &[0x77; 256]);
        write_blk_header(&rig, HDR, 1, 4);
        rig.post(0, &[(HDR, 16, false), (DATA1, 256, false), (STATUS, 1, true)]);

        write_blk_header(&rig, HDR, 0, 2);
        rig.post(0, &[(HDR, 16, false), (DATA2, 512, true), (STATUS, 1, true)]);
        assert_eq!(rig.peek(DATA2, 1), 0x5a); // the guest sees its own write

        write_blk_header(&rig, HDR, 0, 4);
        rig.post(0, &[(HDR, 16, false), (DATA2, 512, true), (STATUS, 1, true)]);
        assert_eq!(rig.peek(DATA2, 1), 0x77); // new half
        assert_eq!(rig.peek(DATA2 + 255, 1), 0x77);
        assert_eq!(rig.peek(DATA2 + 256, 1), 5); // old half survives the partial write
        assert_eq!(rig.peek(DATA2 + 511, 1), 5);
    }
    assert_eq!(std::fs::read(&path).unwrap(), before); // ...but the host file never changed
    std::fs::remove_file(&path).unwrap();
}

// --- network card ------------------------------------------------------------------------------

const TX_BUF: u64 = BUFFER_AREA;
const RX_BUF: u64 = BUFFER_AREA + 0x4000;

fn frame_of(n: usize, seed: u8) -> Vec<u8> {
    (0..n).map(|i| seed.wrapping_add((i * 3) as u8)).collect()
}

#[test]
fn net_transmit_reaches_the_backend_and_loops_back_to_receive() {
    let mac = [0x52, 0x54, 0x00, 0x12, 0x34, 0x56];
    let mut rig = Rig::new(VirtioNet::new(Box::new(LoopbackBackend::new()), mac));
    rig.setup_queue(0); // receive
    rig.setup_queue(1); // transmit

    // The card tells the driver its MAC through the config space.
    for (i, m) in mac.iter().enumerate() {
        assert_eq!(rig.load(0x100 + i as u64, 1), *m as u64);
    }

    // Post a receive buffer first, then transmit one frame.
    rig.post(0, &[(RX_BUF, 2048, true)]);

    let payload = frame_of(60, 0x21);
    let mut tx = vec![0u8; 12]; // zeroed virtio_net_hdr
    tx.extend_from_slice(&payload);
    rig.write_bytes(TX_BUF, &tx);
    rig.post(1, &[(TX_BUF, tx.len() as u32, false)]);
    assert_eq!(rig.used_idx(1), 1); // the transmit buffer was returned

    rig.poll(); // the loopback frame comes back
    assert_eq!(rig.used_idx(0), 1);
    assert_eq!(rig.used_len(0, 0), 12 + 60);
    let got = rig.read_bytes(RX_BUF, 12 + 60);
    assert!(got[..10].iter().all(|&b| b == 0)); // header: no offloads
    assert_eq!(&got[12..], &payload[..]);
}

#[test]
fn net_holds_frames_until_the_guest_posts_a_buffer() {
    let mut rig = Rig::new(VirtioNet::new(Box::new(LoopbackBackend::new()), [0; 6]));
    rig.setup_queue(0);
    rig.setup_queue(1);

    let tx = vec![0u8; 12 + 40];
    rig.write_bytes(TX_BUF, &tx);
    rig.post(1, &[(TX_BUF, tx.len() as u32, false)]);

    rig.poll(); // no receive buffer yet
    assert_eq!(rig.used_idx(0), 0); // the frame is waiting, not lost

    rig.post(0, &[(RX_BUF, 2048, true)]); // now the guest supplies room
    assert_eq!(rig.used_idx(0), 1); // the doorbell alone delivers it
    assert_eq!(rig.used_len(0, 0), 12 + 40);
}

#[test]
fn net_drops_a_frame_that_does_not_fit() {
    let mut rig = Rig::new(VirtioNet::new(Box::new(LoopbackBackend::new()), [0; 6]));
    rig.setup_queue(0);
    rig.setup_queue(1);

    let tx = vec![0u8; 12 + 300];
    rig.write_bytes(TX_BUF, &tx);
    rig.post(1, &[(TX_BUF, tx.len() as u32, false)]);
    rig.post(0, &[(RX_BUF, 100, true)]); // far too small
    rig.poll();

    assert_eq!(rig.dev.borrow().dev.dropped(), 1);
    assert_eq!(rig.used_len(0, 0), 0); // returned empty, not truncated
}

// --- entropy -------------------------------------------------------------------------------------

#[test]
fn rng_fills_buffers_and_can_be_seeded() {
    let fill = |seed: u64| {
        let mut rng = VirtioRng::new();
        rng.seed(seed);
        let mut rig = Rig::new(rng);
        rig.setup_queue(0);
        rig.post(0, &[(DATA1, 64, true)]);
        assert_eq!(rig.used_len(0, 0), 64);
        rig.read_bytes(DATA1, 64)
    };
    let (a, b, c) = (fill(1234), fill(1234), fill(99));
    assert_eq!(a, b); // the same seed, the same bytes
    assert_ne!(a, c);
    let nonzero = a.iter().filter(|&&x| x != 0).count();
    assert!(nonzero > 40); // it really is noise, not zeros
}

// --- the clock ---------------------------------------------------------------------------------------

#[test]
fn rtc_reports_wall_clock_time_and_alarms() {
    let (_plic_dev, plic) = Shared::new(Plic::new(1));
    let mut rtc = Rtc::new();
    rtc.attach(plic.clone(), RTC_IRQ);
    rtc.set_fixed_time(0x0000_0001_8000_0000); // a time whose halves both matter

    let lo = rtc.load(0x00, 4).unwrap();
    let hi = rtc.load(0x04, 4).unwrap();
    assert_eq!((hi << 32) | lo, 0x0000_0001_8000_0000);

    // Reading LOW latches HIGH: advancing the clock between the two reads must not tear the value.
    rtc.load(0x00, 4).unwrap();
    rtc.advance(0x1_0000_0000);
    assert_eq!(rtc.load(0x04, 4).unwrap(), 1);

    // An alarm in the future does nothing yet...
    plic.borrow_mut().store(4 * RTC_IRQ as u64, 4, 1).unwrap(); // priority
    plic.borrow_mut().store(0x2000, 4, 1 << RTC_IRQ).unwrap(); // enable for context 0
    let now = rtc.now_ns();
    rtc.store(0x10, 4, 1).unwrap(); // IRQ enabled
    rtc.store(0x0c, 4, (now + 1000) >> 32).unwrap();
    rtc.store(0x08, 4, (now + 1000) & 0xffff_ffff).unwrap();
    rtc.poll();
    let mut csrs = CsrFile::new();
    plic.borrow().update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_MEIP, 0);
    assert_eq!(rtc.load(0x18, 4).unwrap(), 1); // armed

    // ...until the time arrives.
    rtc.advance(2000);
    rtc.poll();
    plic.borrow().update(&mut csrs, 0);
    assert_ne!(csrs.read(csr::MIP) & csr::MIP_MEIP, 0);
    assert_eq!(rtc.load(0x18, 4).unwrap(), 0); // one-shot: disarmed

    rtc.store(0x1c, 4, 1).unwrap(); // clear the interrupt
    plic.borrow().update(&mut csrs, 0);
    assert_eq!(csrs.read(csr::MIP) & csr::MIP_MEIP, 0);
}

#[test]
fn rtc_follows_the_host_clock_by_default() {
    let rtc = Rtc::new();
    assert!(rtc.now_ns() > 1_600_000_000u64 * 1_000_000_000); // after September 2020
}

#[test]
fn blk_image_truncated_while_running_gives_an_io_error_not_garbage() {
    let path = temp_image("truncate", 8);
    let mut blk = VirtioBlk::new();
    blk.open_image(path.to_str().unwrap(), false).unwrap();
    let mut rig = Rig::new(blk);
    rig.setup_queue(0);

    // Someone shrinks the image underneath the emulator: the device still believes it has 8 sectors.
    std::fs::OpenOptions::new().write(true).open(&path).unwrap().set_len(2 * 512).unwrap();

    // A read inside what remains still works...
    write_blk_header(&rig, HDR, 0, 1);
    rig.poke(STATUS, 1, 0xff);
    rig.post(0, &[(HDR, 16, false), (DATA2, 512, true), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 0);

    // ...one past the new end fails with an I/O error and leaves the guest buffer untouched.
    rig.write_bytes(DATA1, &[0x5a; 512]);
    write_blk_header(&rig, HDR, 0, 6);
    rig.poke(STATUS, 1, 0xff);
    rig.post(0, &[(HDR, 16, false), (DATA1, 512, true), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 1);
    assert_eq!(rig.peek(DATA1, 1), 0x5a);
    drop(rig);
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn blk_snapshot_handles_an_image_that_ends_in_a_short_sector() {
    // 8 full sectors plus 100 bytes: the last "sector" is only partly there.
    let path = std::env::temp_dir().join(format!("riscv_emu_test_virtio_{}_short.img", std::process::id()));
    std::fs::write(&path, vec![7u8; 8 * 512 + 100]).unwrap();
    let mut blk = VirtioBlk::new();
    blk.open_image(path.to_str().unwrap(), true).unwrap();
    let mut rig = Rig::new(blk);
    rig.setup_queue(0);

    // Write the 100 tail bytes: a partial-sector write that must seed from the 100 bytes that exist.
    rig.write_bytes(DATA1, &[0x33; 100]);
    write_blk_header(&rig, HDR, 1, 8);
    rig.poke(STATUS, 1, 0xff);
    rig.post(0, &[(HDR, 16, false), (DATA1, 100, false), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 0);

    write_blk_header(&rig, HDR, 0, 8);
    rig.poke(STATUS, 1, 0xff);
    rig.post(0, &[(HDR, 16, false), (DATA2, 100, true), (STATUS, 1, true)]);
    assert_eq!(rig.peek(STATUS, 1), 0);
    assert_eq!(rig.peek(DATA2, 1), 0x33);
    assert_eq!(rig.peek(DATA2 + 99, 1), 0x33);
    drop(rig);
    assert_eq!(std::fs::read(&path).unwrap()[8 * 512], 7); // the host file was never touched
    std::fs::remove_file(&path).unwrap();
}

#[test]
fn blk_does_not_drain_an_enormous_queue_in_one_doorbell() {
    use riscv_emu::virtio::blk::MAX_REQUESTS_PER_PASS;
    let rig = make_blk(4);
    let total = MAX_REQUESTS_PER_PASS as u64 + 5;

    // One flush request (header + status) described once, and listed `total` times on the avail ring.
    write_blk_header(&rig, HDR, 4, 0);
    rig.poke(STATUS, 1, 0xff);
    let q = rig.queues[0];
    rig.poke(q.desc, 8, HDR);
    rig.poke(q.desc + 8, 4, 16);
    rig.poke(q.desc + 12, 2, F_NEXT as u64);
    rig.poke(q.desc + 14, 2, 1);
    rig.poke(q.desc + 16, 8, STATUS);
    rig.poke(q.desc + 24, 4, 1);
    rig.poke(q.desc + 28, 2, F_WRITE as u64);
    for i in 0..total {
        rig.poke(q.avail + 4 + (i % q.num as u64) * 2, 2, 0);
    }
    rig.poke(q.avail + 2, 2, total);
    rig.w32(0x050, 0); // the doorbell

    // One pass handles its share and no more...
    assert_eq!(rig.used_idx(0), MAX_REQUESTS_PER_PASS as u64);
    // ...and the scheduler's next rounds finish the rest.
    rig.poll();
    assert_eq!(rig.used_idx(0), total);
    rig.poll();
    assert_eq!(rig.used_idx(0), total); // nothing left, nothing duplicated
}
