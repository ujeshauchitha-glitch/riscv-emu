# Phase 11: devices for a real OS — virtio, a network, a clock

xv6 needs a disk. A Debian needs a disk it can write gigabytes to, a network, a
clock that knows today's date, and a source of entropy. This phase adds them, on
top of a virtio transport written once instead of once per device.

## The virtio transport

[`virtio_mmio.hpp`](../include/virtio_mmio.hpp) holds everything that is the same
for every virtio device: the register file, feature negotiation, the queues, and
interrupts. A device supplies only what makes it itself — an id, its feature bits,
its config space, and what to do when the driver rings the doorbell.

**Every field the guest writes into a descriptor is hostile input.** The device is
a bus master reading memory the guest controls, so:

- a chain is length-limited, so a descriptor that points back at itself cannot
  hang the host;
- indices and addresses are checked against the tables and memory they refer to;
- the used ring entry is written *before* the index that publishes it, so the
  driver can never observe a half-finished completion.

(The first version of the block device had an out-of-bounds host read/write in its
`offset + length > size` check, found in the repository review; the fix and its
regression tests carry over, and are tightened: the sector number is compared
against the capacity *before* it is multiplied.)

## The block device

[`virtio.hpp`](../include/virtio.hpp). A request is a header, **any number of data
buffers**, and a status byte. xv6 sends exactly three descriptors; Linux sends one
data buffer per physically contiguous run of pages, so a large request is a long
chain. The device walks the whole chain.

Also: `FLUSH` and `GET_ID`, the `SEG_MAX`/`BLK_SIZE`/`FLUSH` features, and queues of
256 (the first version had 8, which throttles a real driver).

### Disks that don't fit in RAM

| Option | Behaviour |
|---|---|
| `--disk FILE` | read the whole file into memory; guest writes are discarded at exit (as before) |
| `--drive FILE` | keep the file open on the host; **no RAM cost**, and guest writes persist |
| `--drive-snapshot FILE` | like `--drive`, but writes are held in a sector overlay and **the file is never modified** |

Snapshot mode is copy-on-write at sector granularity — the first partial-sector
write pulls the sector in so the bytes around it survive — and is the safe way to
experiment with a Debian image you don't want to damage.

## Network: a card, and a network behind it

[`virtio_net.hpp`](../include/virtio_net.hpp) is a pure frame mover: queue 0
receives, queue 1 transmits, every frame is preceded by a 12-byte header. No
offload features are negotiated (no checksum or segmentation offload, no merged
buffers), so the guest does that work and the header is all zeros. What the "wire"
is — a loopback, a user-mode network — is a `NetBackend` behind the card.

### The user-mode network

[`usernet.hpp`](../include/usernet.hpp) gives the guest a working internet
connection with **no tap device, bridge, root privileges or host setup** — the same
idea as QEMU's "user" networking. The guest believes it is on a small private
network with a router:

```
10.0.2.0/24
  10.0.2.2    the gateway — and also the host: connecting to it reaches the host's 127.0.0.1
  10.0.2.3    DNS, forwarded to the host's resolver
  10.0.2.15   the guest, which learns all of this over DHCP
```

It speaks **ARP**, **DHCP** (it is the only server on the network), **ICMP echo**
to the gateway, **UDP** (each guest datagram becomes a datagram from a host socket,
and replies are mapped back to look like they came from where the guest sent them),
**DNS**, and **TCP**.

**TCP is a proxy, not a bridge.** A guest's TCP connection *ends in this code*: it is
the guest's peer, with its own sequence numbers and state machine, and it opens a
separate ordinary socket on the host for the other half. Bytes move between the
two. That is what makes it possible without raw sockets or privileges.

It is simpler than it sounds because the link between guest and proxy is a function
call: it never drops, reorders or duplicates a segment, so no retransmission
machinery is needed on this side. (The guest still retransmits if it likes;
duplicates are recognised and re-acknowledged.) Flow control works in both
directions — data toward the guest is only sent into the window the guest
advertises, and the window we advertise shrinks as unsent guest data piles up.

Not supported: pings beyond the gateway (that needs a raw socket on most hosts),
IPv6, IP fragments, and TCP options other than MSS.

Frame handling is defensive — runts, bad headers, fragments and multicast are
counted and dropped — because this is the one component built to face untrusted
input.

### Host sockets, portably

[`hostnet.hpp`](../include/hostnet.hpp) wraps the dozen socket operations the
emulator needs so that nothing else has an `#ifdef`. On Windows that means WinSock
(`WSAStartup`, `closesocket`, `WSAEWOULDBLOCK`); the project builds with MinGW
**statically linked**, because a machine with two MinGW installs otherwise loads
the wrong `libstdc++-6.dll` and a program exits instantly with `0xc0000139`.

## Entropy and the clock

- **virtio-rng** answers the guest's entropy requests. A modern boot stalls
  waiting for its random pool (systemd, ssh key generation, TLS), and a machine
  that executes the same instructions every run has almost nothing of its own to
  gather entropy from.
- **`/chosen/rng-seed`**: a `--linux` boot also hands the kernel 32 random bytes in
  the device tree, which Linux credits to its pool at boot. (`--rng-seed N` makes
  both reproducible.)
- **A goldfish RTC** — the one QEMU's virt machine uses and Linux has a driver for.
  Without a clock a guest boots into 1970, and `apt` refuses a repository whose
  `Release` file is "from the future", and TLS refuses a certificate that is "not
  yet valid" — failures that look nothing like a clock problem. `--rtc-fixed SECONDS`
  pins the date.

## DMA and the decode cache

Devices write guest memory directly, and the CPU caches decoded instructions
(phase 14). A page of code loaded off a disk must not run stale: `Dram::write_bytes`
— the bulk path virtio uses — invalidates the cache exactly as an ordinary store
does.

## Options

```
--drive FILE | --drive-snapshot FILE | --disk FILE
--net user | --net loopback        --net-dns IP
--rng      --rng-seed N            --rtc      --rtc-fixed SECONDS
```

## Tests

- [`tests/test_virtio.cpp`](../tests/test_virtio.cpp) drives the devices the way a
  guest driver does: multi-buffer reads and writes, flush, get-id, chains with no
  status byte, a too-short header, a buffer pointing at nonexistent memory, a
  backwards read, a descriptor loop; file-backed persistence and snapshot
  copy-on-write (including a half-sector write that must not lose the other half);
  the network card holding a frame until the guest posts a buffer, and dropping
  one that doesn't fit; the RTC's latched read and its alarm.
- [`tests/test_usernet.cpp`](../tests/test_usernet.cpp) acts as a guest kernel,
  building raw Ethernet frames with its *own* encoders, against real sockets on the
  host's loopback: ARP, a ping, a DHCP exchange (including a refused lease), a UDP
  round trip, DNS forwarding to a fake resolver, and **a complete TCP connection** —
  handshake, data both ways, close — plus a connection to a closed port (reset) and
  stray segments (reset). Every checksum in every reply is verified.

Both were mutation-checked: a wrong DHCP address, a TCP checksum computed with the
wrong protocol, UDP replies from the wrong source, and bytes not accepted from the
guest each fail a test.
