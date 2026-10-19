//! Guest DRAM.
//!
//! A flat byte array mapped at `DRAM_BASE` (0x8000_0000). Guest address
//! 0x8000_0000 is element 0, not guest address 0: every kernel we want to boot is
//! linked expecting RAM to start there. Every access is bounds-checked, including
//! the case where it starts in range but its width runs off the end.
//!
//! **Interior mutability.** RAM is shared: the bus, every hart, and any device
//! that does DMA (virtio) all reach it through an `Rc<Dram>`. Everything runs on
//! one host thread, and no method hands out a reference into the storage that
//! outlives the call, so the single `UnsafeCell` below is sound. (`UnsafeCell` is
//! not `Sync`, so the compiler also refuses to let a `Dram` cross threads.)
//!
//! **Decode-cache support.** A CPU that caches decoded instructions must learn when
//! the bytes behind them change. The guest is *supposed* to execute FENCE.I after
//! modifying code, but a device DMAing a page of code into RAM never does - so the
//! CPU tells DRAM which pages it cached code from, and any store that lands on one
//! (from the guest or from a device) bumps an epoch every cache compares against
//! before trusting an entry.

use std::cell::{Cell, UnsafeCell};

pub struct Dram {
    mem: UnsafeCell<Vec<u8>>,
    len: usize,
    epoch: Cell<u64>,
    code_pages: UnsafeCell<Vec<u8>>,
    marked: UnsafeCell<Vec<u32>>,
    any_marked: Cell<bool>,
}

impl Dram {
    pub fn new(size_bytes: u64) -> Dram {
        let len = size_bytes as usize;
        Dram {
            mem: UnsafeCell::new(vec![0u8; len]),
            len,
            epoch: Cell::new(1),
            code_pages: UnsafeCell::new(vec![0u8; (len + 4095) >> 12]),
            marked: UnsafeCell::new(Vec::new()),
            any_marked: Cell::new(false),
        }
    }

    #[inline(always)]
    pub fn size(&self) -> u64 {
        self.len as u64
    }

    #[inline(always)]
    fn ptr(&self) -> *mut u8 {
        // SAFETY: only the pointer is taken here; callers bounds-check before use.
        unsafe { (*self.mem.get()).as_mut_ptr() }
    }

    /// True if `[offset, offset + size)` lies entirely inside the array.
    ///
    /// Written as a subtraction: `offset + size > len` can overflow for a hostile
    /// offset near 2^64, and this form cannot.
    #[inline(always)]
    pub fn in_range(&self, offset: u64, size: usize) -> bool {
        offset < self.len as u64 && size <= self.len - offset as usize
    }

    /// Little-endian load of 1, 2, 4 or 8 bytes. `None` if out of range.
    #[inline(always)]
    pub fn load(&self, offset: u64, size: usize) -> Option<u64> {
        if !self.in_range(offset, size) {
            return None;
        }
        // SAFETY: bounds checked above; unaligned reads are allowed by the ISA and
        // read_unaligned makes no alignment assumption. The host is little-endian
        // (asserted in lib.rs), matching guest byte order.
        unsafe {
            let p = self.ptr().add(offset as usize);
            Some(match size {
                1 => *p as u64,
                2 => (p as *const u16).read_unaligned() as u64,
                4 => (p as *const u32).read_unaligned() as u64,
                8 => (p as *const u64).read_unaligned(),
                _ => {
                    let mut v = 0u64;
                    for i in 0..size.min(8) {
                        v |= (*p.add(i) as u64) << (8 * i);
                    }
                    v
                }
            })
        }
    }

    /// Little-endian store of 1, 2, 4 or 8 bytes. `false` if out of range.
    #[inline(always)]
    pub fn store(&self, offset: u64, size: usize, value: u64) -> bool {
        if !self.in_range(offset, size) {
            return false;
        }
        self.note_write(offset, size as u64);
        // SAFETY: as in `load`.
        unsafe {
            let p = self.ptr().add(offset as usize);
            match size {
                1 => *p = value as u8,
                2 => (p as *mut u16).write_unaligned(value as u16),
                4 => (p as *mut u32).write_unaligned(value as u32),
                8 => (p as *mut u64).write_unaligned(value),
                _ => {
                    for i in 0..size.min(8) {
                        *p.add(i) = (value >> (8 * i)) as u8;
                    }
                }
            }
        }
        true
    }

    /// Bulk read for devices that DMA. `guest_addr` is an absolute physical
    /// address; returns false, touching nothing, if any part is outside DRAM.
    pub fn read_bytes(&self, guest_addr: u64, dst: &mut [u8]) -> bool {
        let Some(offset) = guest_addr.checked_sub(crate::types::DRAM_BASE) else {
            return false;
        };
        if dst.is_empty() {
            return offset <= self.len as u64;
        }
        if !self.in_range(offset, dst.len()) {
            return false;
        }
        // SAFETY: range checked.
        unsafe {
            std::ptr::copy_nonoverlapping(self.ptr().add(offset as usize), dst.as_mut_ptr(), dst.len());
        }
        true
    }

    /// Bulk write for devices that DMA. Invalidates cached code exactly as an
    /// ordinary store does.
    pub fn write_bytes(&self, guest_addr: u64, src: &[u8]) -> bool {
        let Some(offset) = guest_addr.checked_sub(crate::types::DRAM_BASE) else {
            return false;
        };
        if src.is_empty() {
            return offset <= self.len as u64;
        }
        if !self.in_range(offset, src.len()) {
            return false;
        }
        self.note_write(offset, src.len() as u64);
        // SAFETY: range checked.
        unsafe {
            std::ptr::copy_nonoverlapping(src.as_ptr(), self.ptr().add(offset as usize), src.len());
        }
        true
    }

    /// Copy a blob into DRAM at an absolute guest address (image loaders).
    pub fn load_image(&self, guest_addr: u64, bytes: &[u8]) -> bool {
        if guest_addr < crate::types::DRAM_BASE {
            return false;
        }
        let offset = guest_addr - crate::types::DRAM_BASE;
        if offset > self.len as u64 || bytes.len() as u64 > self.len as u64 - offset {
            return false;
        }
        self.invalidate_code();
        if !bytes.is_empty() {
            // SAFETY: range checked.
            unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), self.ptr().add(offset as usize), bytes.len());
            }
        }
        true
    }

    /// Read one byte (tests and debuggers). Returns 0 out of range.
    pub fn peek8(&self, offset: u64) -> u8 {
        self.load(offset, 1).unwrap_or(0) as u8
    }

    /// Mutable access to the whole array, for tests and loaders. Counts as a write:
    /// whoever holds the slice can change code behind the decode cache's back, so
    /// it conservatively invalidates everything the CPUs have cached.
    pub fn with_bytes_mut<R>(&self, f: impl FnOnce(&mut [u8]) -> R) -> R {
        self.invalidate_code();
        // SAFETY: single-threaded, and the slice does not escape the closure.
        unsafe { f(&mut *self.mem.get()) }
    }

    // --- decoded-instruction cache support ---------------------------------------

    pub fn code_epoch(&self) -> u64 {
        self.epoch.get()
    }

    /// The CPU has cached decoded code from the 4 KiB page containing `offset`.
    pub fn mark_code_page(&self, offset: u64) {
        let page = (offset >> 12) as usize;
        // SAFETY: single-threaded; no reference outlives this block.
        unsafe {
            let pages = &mut *self.code_pages.get();
            if page >= pages.len() || pages[page] != 0 {
                return;
            }
            pages[page] = 1;
            (*self.marked.get()).push(page as u32);
        }
        self.any_marked.set(true);
    }

    fn invalidate_code(&self) {
        self.epoch.set(self.epoch.get() + 1);
        // SAFETY: single-threaded.
        unsafe {
            let pages = &mut *self.code_pages.get();
            let marked = &mut *self.marked.get();
            for p in marked.drain(..) {
                pages[p as usize] = 0;
            }
        }
        self.any_marked.set(false);
    }

    #[inline(always)]
    fn note_write(&self, offset: u64, size: u64) {
        if !self.any_marked.get() || size == 0 {
            return;
        }
        let first = (offset >> 12) as usize;
        let last = ((offset + size - 1) >> 12) as usize;
        // SAFETY: single-threaded read of the flag array.
        let pages = unsafe { &*self.code_pages.get() };
        for page in first..=last {
            if pages.get(page).copied().unwrap_or(0) != 0 {
                self.invalidate_code();
                return;
            }
        }
    }
}
