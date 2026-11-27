use std::{
    io,
    net::{SocketAddr, UdpSocket},
    os::fd::AsRawFd,
};

// At most one millisecond of airtime at the current pacing rate.
pub fn batch_len(rate_kbps: u32, wire_bytes: usize) -> usize {
    (rate_kbps as usize / (8 * wire_bytes.max(1))).clamp(1, 16)
}

pub fn send_batch(socket: &UdpSocket, remote: SocketAddr, datagrams: &[Vec<u8>]) -> io::Result<()> {
    if let [only] = datagrams {
        socket.send_to(only, remote)?;
        return Ok(());
    }
    let mut address: libc::sockaddr_storage = unsafe { std::mem::zeroed() };
    let name = (&mut address as *mut libc::sockaddr_storage).cast::<libc::sockaddr>();
    let length = match remote {
        SocketAddr::V4(ip) => {
            unsafe {
                name.cast::<libc::sockaddr_in>().write(libc::sockaddr_in {
                    sin_family: libc::AF_INET as _,
                    sin_port: ip.port().to_be(),
                    sin_addr: libc::in_addr {
                        s_addr: u32::from_ne_bytes(ip.ip().octets()),
                    },
                    sin_zero: [0; 8],
                });
            }
            size_of::<libc::sockaddr_in>()
        }
        SocketAddr::V6(ip) => {
            unsafe {
                name.cast::<libc::sockaddr_in6>().write(libc::sockaddr_in6 {
                    sin6_family: libc::AF_INET6 as _,
                    sin6_port: ip.port().to_be(),
                    sin6_flowinfo: ip.flowinfo().to_be(),
                    sin6_addr: libc::in6_addr {
                        s6_addr: ip.ip().octets(),
                    },
                    sin6_scope_id: ip.scope_id(),
                });
            }
            size_of::<libc::sockaddr_in6>()
        }
    };
    let mut iovecs = datagrams
        .iter()
        .map(|bytes| libc::iovec {
            iov_base: bytes.as_ptr().cast_mut().cast(),
            iov_len: bytes.len(),
        })
        .collect::<Vec<_>>();
    let mut messages = iovecs
        .iter_mut()
        .map(|iov| libc::mmsghdr {
            msg_hdr: libc::msghdr {
                msg_name: name.cast(),
                msg_namelen: length as _,
                msg_iov: iov,
                msg_iovlen: 1,
                msg_control: std::ptr::null_mut(),
                msg_controllen: 0,
                msg_flags: 0,
            },
            msg_len: 0,
        })
        .collect::<Vec<_>>();
    let mut sent = 0;
    while sent < messages.len() {
        let result = unsafe {
            libc::sendmmsg(
                socket.as_raw_fd(),
                messages[sent..].as_mut_ptr(),
                (messages.len() - sent) as _,
                0,
            )
        };
        if result > 0 {
            sent += result as usize;
        } else if result == 0 {
            return Err(io::ErrorKind::WriteZero.into());
        } else {
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::Interrupted {
                return Err(error);
            }
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn small_batches_preserve_datagram_boundaries_and_order() {
        assert_eq!(batch_len(1_000, 1_100), 1);
        assert_eq!(batch_len(30_000, 1_100), 3);
        assert_eq!(batch_len(1_000_000, 1_100), 16);
        for address in ["127.0.0.1:0", "[::1]:0"] {
            let receiver = match UdpSocket::bind(address) {
                Err(error)
                    if address.starts_with('[')
                        && error.raw_os_error().is_some_and(|e| {
                            e == libc::EADDRNOTAVAIL || e == libc::EAFNOSUPPORT
                        }) =>
                {
                    continue
                }
                result => result.unwrap(),
            };
            receiver
                .set_read_timeout(Some(Duration::from_secs(1)))
                .unwrap();
            let sender = UdpSocket::bind(address).unwrap();
            let datagrams = [vec![1; 23], vec![2; 51], vec![3; 1024]];
            send_batch(&sender, receiver.local_addr().unwrap(), &datagrams).unwrap();
            for datagram in &datagrams {
                let mut received = [0; 1024];
                let size = receiver.recv(&mut received).unwrap();
                assert_eq!(&received[..size], datagram);
            }
        }
    }
}
