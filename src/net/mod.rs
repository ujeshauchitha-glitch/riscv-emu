//! Networking: a user-mode stack that proxies the guest's traffic through ordinary
//! host sockets (see `usernet`), and the thin layer over the host's sockets it needs
//! (`hostnet`).

pub mod hostnet;
pub mod usernet;
