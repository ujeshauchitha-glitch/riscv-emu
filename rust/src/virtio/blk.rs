//! virtio-blk: a disk.
//!
//! A request is a descriptor chain of three kinds of buffer: a 16-byte header (type
//! and starting sector), one or more data buffers, and a one-byte status the device
//! fills in last. The backing store is either a file read wholly into memory (writes
//! are lost at exit), a file kept on disk (writes persist), or a file kept on disk but
//! read-only with the guest's writes held in a sector overlay (a snapshot - the image
//! is never modified, which is what you want when a Debian rootfs should stay pristine
//! between boots).

use std::collections::HashMap;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};

use super::{Core, Segment, VirtioDevice};

pub const SECTOR_SIZE: u64 = 512;

const BLOCK_DEVICE: u32 = 2;

// Request types.
const BLK_T_IN: u32 = 0; // read from disk
const BLK_T_OUT: u32 = 1; // write to disk
const BLK_T_FLUSH: u32 = 4; // make earlier writes durable
const BLK_T_GET_ID: u32 = 8; // return a device identifier string

// Status values written to the last byte of the chain.
const BLK_S_OK: u8 = 0;
const BLK_S_IOERR: u8 = 1;
const BLK_S_UNSUPP: u8 = 2;

// Feature bits (the low half of the feature space).
const F_SEG_MAX: u64 = 1 << 2; // the config space says how many segments a request may have
const F_BLK_SIZE: u64 = 1 << 6; // the config space says the logical block size
const F_FLUSH: u64 = 1 << 9; // the device understands BLK_T_FLUSH

/// The most data segments one request may have. Linux sizes its scatter-gather lists
/// from this, and then needs the queue to have room for the header and status
/// descriptors as well.
const SEG_MAX: u32 = 126;

const HEADER_BYTES: u64 = 16;

enum Store {
    Memory(Vec<u8>),
    File { file: File, size: u64, snapshot: bool, overlay: HashMap<u64, [u8; SECTOR_SIZE as usize]> },
}

pub struct VirtioBlk {
    store: Store,
}

impl Default for VirtioBlk {
    fn default() -> Self {
        VirtioBlk::new()
    }
}

impl VirtioBlk {
    pub fn new() -> VirtioBlk {
        VirtioBlk { store: Store::Memory(Vec::new()) }
    }

    /// Read a whole image into memory. The guest's writes never reach the file.
    pub fn load_image(&mut self, path: &str) -> std::io::Result<()> {
        self.store = Store::Memory(std::fs::read(path)?);
        Ok(())
    }

    /// Keep the image on disk. In snapshot mode it is opened read-only (which also lets
    /// an image on a read-only medium be used) and the guest's writes live in memory.
    pub fn open_image(&mut self, path: &str, snapshot: bool) -> std::io::Result<()> {
        let file = if snapshot {
            File::open(path)?
        } else {
            OpenOptions::new().read(true).write(true).open(path)?
        };
        let size = file.metadata()?.len();
        self.store = Store::File { file, size, snapshot, overlay: HashMap::new() };
        Ok(())
    }

    /// The disk's contents when it lives in memory (for tests).
    pub fn data(&self) -> &[u8] {
        match &self.store {
            Store::Memory(v) => v,
            Store::File { .. } => &[],
        }
    }

    pub fn data_mut(&mut self) -> &mut Vec<u8> {
        if !matches!(self.store, Store::Memory(_)) {
            self.store = Store::Memory(Vec::new());
        }
        match &mut self.store {
            Store::Memory(v) => v,
            Store::File { .. } => unreachable!(),
        }
    }

    pub fn size_bytes(&self) -> u64 {
        match &self.store {
            Store::Memory(v) => v.len() as u64,
            Store::File { size, .. } => *size,
        }
    }

    pub fn sectors(&self) -> u64 {
        self.size_bytes() / SECTOR_SIZE
    }

    // --- the backing store -----------------------------------------------------

    fn disk_read(&mut self, offset: u64, dst: &mut [u8]) -> bool {
        match &mut self.store {
            Store::Memory(v) => {
                let o = offset as usize;
                match v.get(o..o + dst.len()) {
                    Some(src) => {
                        dst.copy_from_slice(src);
                        true
                    }
                    None => false,
                }
            }
            Store::File { file, snapshot, overlay, .. } => {
                if file.seek(SeekFrom::Start(offset)).is_err() || file.read_exact(dst).is_err() {
                    return false;
                }
                // The guest's own writes shadow the file in snapshot mode. Walk the
                // sectors the request touches and patch in any it has overwritten.
                if *snapshot && !overlay.is_empty() && !dst.is_empty() {
                    let len = dst.len() as u64;
                    let (first, last) = (offset / SECTOR_SIZE, (offset + len - 1) / SECTOR_SIZE);
                    for s in first..=last {
                        let Some(sector) = overlay.get(&s) else { continue };
                        let sector_start = s * SECTOR_SIZE;
                        let from = sector_start.max(offset);
                        let to = (sector_start + SECTOR_SIZE).min(offset + len);
                        dst[(from - offset) as usize..(to - offset) as usize]
                            .copy_from_slice(&sector[(from - sector_start) as usize..(to - sector_start) as usize]);
                    }
                }
                true
            }
        }
    }

    fn disk_write(&mut self, offset: u64, src: &[u8]) -> bool {
        let snapshot = match &mut self.store {
            Store::Memory(v) => {
                let o = offset as usize;
                return match v.get_mut(o..o + src.len()) {
                    Some(dst) => {
                        dst.copy_from_slice(src);
                        true
                    }
                    None => false,
                };
            }
            Store::File { file, snapshot, .. } => {
                if !*snapshot {
                    return file.seek(SeekFrom::Start(offset)).is_ok() && file.write_all(src).is_ok();
                }
                true
            }
        };
        debug_assert!(snapshot);

        // Copy-on-write at sector granularity: pull in the sector's current contents
        // the first time it is touched, so a partial-sector write keeps the bytes
        // around it.
        let mut pos = 0u64;
        let len = src.len() as u64;
        while pos < len {
            let s = (offset + pos) / SECTOR_SIZE;
            let within = (offset + pos) % SECTOR_SIZE;
            let n = (SECTOR_SIZE - within).min(len - pos);
            let have = matches!(&self.store, Store::File { overlay, .. } if overlay.contains_key(&s));
            if !have {
                let mut sector = [0u8; SECTOR_SIZE as usize];
                if n != SECTOR_SIZE && !self.disk_read(s * SECTOR_SIZE, &mut sector) {
                    return false;
                }
                if let Store::File { overlay, .. } = &mut self.store {
                    overlay.insert(s, sector);
                }
            }
            if let Store::File { overlay, .. } = &mut self.store {
                let sector = overlay.get_mut(&s).unwrap();
                sector[within as usize..(within + n) as usize].copy_from_slice(&src[pos as usize..(pos + n) as usize]);
            }
            pos += n;
        }
        true
    }

    fn disk_flush(&mut self) -> bool {
        match &mut self.store {
            Store::File { file, snapshot: false, .. } => file.flush().is_ok() && file.sync_data().is_ok(),
            _ => true,
        }
    }

    // --- requests --------------------------------------------------------------

    fn process_request(&mut self, core: &mut Core, queue: u32, head: u16, segs: &[Segment]) {
        // A well-formed chain has a header at the front and a status byte at the back.
        // Without both there is nowhere to report anything, so complete it empty
        // rather than guess.
        let (first, last) = match (segs.first(), segs.last()) {
            (Some(f), Some(l)) if segs.len() >= 2 => (*f, *l),
            _ => {
                core.push_used(queue, head, 0);
                return;
            }
        };
        if first.device_writes || (first.len as u64) < HEADER_BYTES || !last.device_writes || last.len < 1 {
            core.push_used(queue, head, 0);
            return;
        }

        let mut header = [0u8; HEADER_BYTES as usize];
        if !core.read_guest_bytes(first.addr, &mut header) {
            core.push_used(queue, head, 0);
            return;
        }
        let kind = u32::from_le_bytes(header[0..4].try_into().unwrap());
        let sector = u64::from_le_bytes(header[8..16].try_into().unwrap());

        let data = &segs[1..segs.len() - 1];
        let mut status = BLK_S_OK;
        let mut written = 0u32; // bytes written into device-writable buffers (data only)

        match kind {
            BLK_T_IN | BLK_T_OUT => {
                // Total bytes the request covers, and whether every data buffer points
                // the right way: the device writes them for a read, the driver filled
                // them for a write. A driver that got this backwards would be asking us
                // to write into a read-only buffer.
                let is_read = kind == BLK_T_IN;
                let total: u64 = data.iter().map(|s| s.len as u64).sum();
                let direction_ok = data.iter().all(|s| s.device_writes == is_read);

                // Bounds-check by comparison before arithmetic. The obvious
                // `sector * 512 + total > size` overflows: a guest that asks for a
                // huge sector number makes the product wrap, the check passes, and the
                // copy loops index far outside the disk. The device is a bus master
                // reading guest-controlled descriptors, so every field here is hostile.
                // Testing the sector against the capacity first means the multiply
                // cannot overflow, and `total` is compared by subtraction.
                let size = self.size_bytes();
                if !direction_ok || sector > size / SECTOR_SIZE || total > size || sector * SECTOR_SIZE > size - total {
                    status = BLK_S_IOERR;
                } else if total != 0 {
                    // Move the data one buffer at a time, bounded by a modest staging
                    // buffer so a 64 MiB request does not allocate 64 MiB.
                    let mut chunk = vec![0u8; 64 * 1024];
                    let mut disk_offset = sector * SECTOR_SIZE;
                    'segments: for s in data {
                        let mut done = 0u64;
                        while done < s.len as u64 {
                            let n = (chunk.len() as u64).min(s.len as u64 - done) as usize;
                            let guest_addr = s.addr.wrapping_add(done);
                            let ok = if is_read {
                                self.disk_read(disk_offset, &mut chunk[..n])
                                    && core.write_guest_bytes(guest_addr, &chunk[..n])
                            } else {
                                core.read_guest_bytes(guest_addr, &mut chunk[..n])
                                    && self.disk_write(disk_offset, &chunk[..n])
                            };
                            if !ok {
                                status = BLK_S_IOERR;
                                break 'segments;
                            }
                            done += n as u64;
                            disk_offset += n as u64;
                        }
                        if is_read {
                            written += s.len;
                        }
                    }
                }
            }
            BLK_T_FLUSH => {
                if !self.disk_flush() {
                    status = BLK_S_IOERR;
                }
            }
            BLK_T_GET_ID => {
                const ID: &[u8; 20] = b"riscv-emu-virtio\0\0\0\0";
                match data.first() {
                    Some(d) if d.device_writes => {
                        let n = (d.len as usize).min(ID.len());
                        if core.write_guest_bytes(d.addr, &ID[..n]) {
                            written = n as u32;
                        } else {
                            status = BLK_S_IOERR;
                        }
                    }
                    _ => status = BLK_S_IOERR,
                }
            }
            _ => status = BLK_S_UNSUPP,
        }

        // [last] the status byte, which the device always writes.
        core.write_guest_bytes(last.addr, &[status]);
        core.push_used(queue, head, written + 1);
    }
}

impl VirtioDevice for VirtioBlk {
    fn name(&self) -> &'static str {
        "virtio-blk"
    }
    fn device_id(&self) -> u32 {
        BLOCK_DEVICE
    }
    fn num_queues(&self) -> u32 {
        1
    }
    fn features(&self) -> u64 {
        F_SEG_MAX | F_BLK_SIZE | F_FLUSH
    }

    // struct virtio_blk_config:
    //    0  u64 capacity (in 512-byte sectors)
    //    8  u32 size_max
    //   12  u32 seg_max
    //   16  geometry: u16 cylinders, u8 heads, u8 sectors
    //   20  u32 blk_size
    fn config_read(&self, offset: u64, size: usize) -> u64 {
        let mut config = [0u8; 24];
        config[0..8].copy_from_slice(&self.sectors().to_le_bytes());
        config[12..16].copy_from_slice(&SEG_MAX.to_le_bytes());
        config[20..24].copy_from_slice(&(SECTOR_SIZE as u32).to_le_bytes());

        // Any read that lies wholly inside the structure is served from it; anything
        // past it reads as zero.
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
        let mut any = false;
        while let Some((head, segs)) = core.pop_chain(queue) {
            self.process_request(core, queue, head, &segs);
            any = true;
        }
        if any {
            core.signal_used(queue);
        }
    }
}
