# Building a Simple VPN: Understanding TUN Devices in Practice

This document explains how to build a minimal VPN from scratch using TUN devices, demonstrating the core concepts of network tunneling in Linux.

## Table of Contents
- [What We're Building](#what-were-building)
- [Architecture Overview](#architecture-overview)
- [Key Components](#key-components)
- [Packet Flow Walkthrough](#packet-flow-walkthrough)
- [Step-by-Step Implementation](#step-by-step-implementation)
- [Running the VPN](#running-the-vpn)
- [How Applications Are Unaware](#how-applications-are-unaware)
- [Kernel Integration](#kernel-integration)

## What We're Building

A **minimal VPN** that tunnels IP packets through an encrypted connection. The VPN consists of:

- **Client**: Creates TUN device, encrypts outgoing packets, decrypts incoming packets
- **Server**: Creates TUN device, forwards packets to the internet, routes responses back

**Key insight**: Applications don't need to change at all - they use normal sockets, and the kernel's routing table transparently redirects their traffic through the TUN device.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         CLIENT MACHINE                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────┐                                                  │
│  │  Application │  (e.g., ping 8.8.8.8)                           │
│  │  ping 8.8.8.8│  Uses normal socket()                           │
│  └──────┬───────┘                                                  │
│         │ send() syscall                                           │
│         ▼                                                           │
│  ┌─────────────────────────────────┐                              │
│  │     Linux Kernel                │                              │
│  │  ┌──────────────────────────┐  │                              │
│  │  │ TCP/IP Stack             │  │                              │
│  │  │ - TCP/UDP layer          │  │                              │
│  │  │ - IP routing             │  │                              │
│  │  │   Looks up: 8.8.8.8      │  │                              │
│  │  │   Route: dev tun0 ←──────┼──┼─ ROUTING TABLE MATCH!        │
│  │  └──────────┬───────────────┘  │                              │
│  │             │                   │                              │
│  │             ▼                   │                              │
│  │  ┌──────────────────────────┐  │                              │
│  │  │  TUN Device (tun0)       │  │ Virtual network interface    │
│  │  │  struct net_device       │  │                              │
│  │  │  Queue: [packet buffer]  │  │                              │
│  │  └──────────┬───────────────┘  │                              │
│  └─────────────┼───────────────────┘                              │
│                │ /dev/net/tun file descriptor                     │
│                ▼                                                   │
│  ┌─────────────────────────────────┐                              │
│  │  VPN Client (userspace)         │                              │
│  │  - read(tun_fd) → gets packet   │                              │
│  │  - XOR encrypt                  │                              │
│  │  - write(server_fd) → TCP       │                              │
│  └─────────────┬───────────────────┘                              │
│                │ TCP connection                                    │
│                ▼                                                   │
│           [ eth0 ] → Internet                                      │
└────────────────┼───────────────────────────────────────────────────┘
                 │
                 │ Encrypted tunnel over Internet
                 │
┌────────────────▼───────────────────────────────────────────────────┐
│                         SERVER MACHINE                              │
├─────────────────────────────────────────────────────────────────────┤
│           [ eth0 ] ← Internet                                       │
│                │                                                    │
│                ▼                                                    │
│  ┌─────────────────────────────────┐                               │
│  │  VPN Server (userspace)         │                               │
│  │  - read(client_fd) from TCP     │                               │
│  │  - XOR decrypt                  │                               │
│  │  - write(tun_fd)                │                               │
│  └─────────────┬───────────────────┘                               │
│                │ /dev/net/tun file descriptor                      │
│                ▼                                                    │
│  ┌─────────────────────────────────┐                               │
│  │     Linux Kernel                │                               │
│  │  ┌──────────────────────────┐  │                               │
│  │  │  TUN Device (tun0)       │  │                               │
│  │  │  Injects packet into     │  │                               │
│  │  │  network stack           │  │                               │
│  │  └──────────┬───────────────┘  │                               │
│  │             │                   │                               │
│  │             ▼                   │                               │
│  │  ┌──────────────────────────┐  │                               │
│  │  │ Routing Decision         │  │                               │
│  │  │ Destination: 8.8.8.8     │  │                               │
│  │  │ Route: default via eth0  │  │                               │
│  │  └──────────┬───────────────┘  │                               │
│  └─────────────┼───────────────────┘                               │
│                ▼                                                    │
│           [ eth0 ] → Internet → 8.8.8.8                            │
└─────────────────────────────────────────────────────────────────────┘
```

## Key Components

### 1. TUN Device Creation (`src/simple_vpn_client.c:33-60`)

```c
int create_tun_device(char *dev_name) {
    struct ifreq ifr;
    int tun_fd, err;

    // Open the TUN device
    tun_fd = open("/dev/net/tun", O_RDWR);

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;  // TUN mode, no packet info
    strncpy(ifr.ifr_name, dev_name, IFNAMSIZ);

    // Create the TUN interface
    err = ioctl(tun_fd, TUNSETIFF, (void *)&ifr);

    return tun_fd;  // File descriptor for reading/writing packets
}
```

**What happens**:
1. Opens `/dev/net/tun` character device
2. Issues `TUNSETIFF` ioctl - kernel creates `struct net_device` for `tun0`
3. Returns file descriptor - used to read/write IP packets
4. Kernel registers `tun0` in network interface list

**Kernel path**: `drivers/net/tun.c:tun_chr_ioctl()` → `drivers/net/tun.c:tun_set_iff()` → creates `struct net_device`

### 2. Event Loop with select() (`src/simple_vpn_client.c:112-186`)

```c
void vpn_event_loop(int tun_fd, int server_fd) {
    unsigned char buffer[2048];
    fd_set read_fds;
    int max_fd = (tun_fd > server_fd) ? tun_fd : server_fd;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(tun_fd, &read_fds);      // Monitor TUN device
        FD_SET(server_fd, &read_fds);   // Monitor server socket

        // BLOCKS until data ready on either FD
        select(max_fd + 1, &read_fds, NULL, NULL, NULL);

        // Data from TUN device (app sent packet)
        if (FD_ISSET(tun_fd, &read_fds)) {
            read(tun_fd, buffer, sizeof(buffer));
            xor_crypt(buffer, nread, XOR_KEY);
            write(server_fd, buffer, nread);  // Send to VPN server
        }

        // Data from server (response packet)
        if (FD_ISSET(server_fd, &read_fds)) {
            read(server_fd, buffer, sizeof(buffer));
            xor_crypt(buffer, nread, XOR_KEY);
            write(tun_fd, buffer, nread);  // Inject to kernel
        }
    }
}
```

**Why `select()` works**:
- `tun_fd` is a file descriptor - integrates with VFS
- When kernel routes packet to `tun0`, it queues the packet
- TUN device's poll operation (`drivers/net/tun.c:tun_chr_poll()`) marks FD as readable
- `select()` returns, VPN client can read the packet

### 3. Routing Configuration

**Client routing**:
```bash
# Configure TUN device
sudo ip addr add 10.8.0.2/24 dev tun0
sudo ip link set tun0 up

# Route Google DNS through VPN
sudo ip route add 8.8.8.8/32 dev tun0
```

**Routing table after configuration**:
```
$ ip route show
8.8.8.8 dev tun0 scope link               ← VPN route (most specific)
10.8.0.0/24 dev tun0 proto kernel         ← TUN network
192.168.1.0/24 dev eth0 proto kernel      ← Local network
default via 192.168.1.1 dev eth0          ← Everything else
```

**How routing works**:
1. App sends packet to 8.8.8.8
2. Kernel calls `ip_route_output_flow()` (`net/ipv4/route.c`)
3. Looks up routing table - finds `8.8.8.8 dev tun0` (most specific match)
4. Calls `tun0->netdev_ops->ndo_start_xmit()` (`drivers/net/tun.c:tun_net_xmit()`)
5. TUN driver queues packet, wakes up VPN client

## Packet Flow Walkthrough

### Sending: App → VPN → Internet

Let's trace what happens when you run `ping 8.8.8.8`:

```
┌─────────────────────────────────────────────────────────────────┐
│ STEP 1: Application (ping)                                      │
└─────────────────────────────────────────────────────────────────┘
ping sends ICMP Echo Request to 8.8.8.8
  → sendto(sock, packet, len, ...)

┌─────────────────────────────────────────────────────────────────┐
│ STEP 2: Kernel - Protocol Layer                                 │
└─────────────────────────────────────────────────────────────────┘
Kernel path: net/ipv4/ping.c:ping_v4_sendmsg()
  → Builds ICMP packet
  → Adds IP header (src: 192.168.1.100, dst: 8.8.8.8)

┌─────────────────────────────────────────────────────────────────┐
│ STEP 3: Kernel - Routing Decision                               │
└─────────────────────────────────────────────────────────────────┘
Kernel path: net/ipv4/route.c:ip_route_output_flow()
  → Looks up routing table for 8.8.8.8
  → Finds: "8.8.8.8 dev tun0"
  → Sets skb->dev = tun0

┌─────────────────────────────────────────────────────────────────┐
│ STEP 4: Kernel - TUN Device Transmit                            │
└─────────────────────────────────────────────────────────────────┘
Kernel path: drivers/net/tun.c:tun_net_xmit()
  → Queues packet in TUN device buffer
  → Marks tun_fd as readable
  → Does NOT send to hardware! (TUN is virtual)

┌─────────────────────────────────────────────────────────────────┐
│ STEP 5: VPN Client Wakes Up                                     │
└─────────────────────────────────────────────────────────────────┘
VPN client's select() returns:
  → FD_ISSET(tun_fd, &read_fds) is TRUE
  → Userspace can now read the packet

┌─────────────────────────────────────────────────────────────────┐
│ STEP 6: VPN Client Reads and Encrypts                           │
└─────────────────────────────────────────────────────────────────┘
VPN client:
  → nread = read(tun_fd, buffer, 2048)
  → Gets raw IP packet (ICMP inside IP)
  → xor_crypt(buffer, nread, 0x42)
  → write(server_fd, buffer, nread)  // Send via TCP

┌─────────────────────────────────────────────────────────────────┐
│ STEP 7: Encrypted Packet Goes Over Internet                     │
└─────────────────────────────────────────────────────────────────┘
Packet travels over normal TCP connection:
  Client (eth0) → Internet → Server (eth0)

┌─────────────────────────────────────────────────────────────────┐
│ STEP 8: VPN Server Receives and Decrypts                        │
└─────────────────────────────────────────────────────────────────┘
VPN server's select() returns (server_fd readable):
  → read(client_fd, buffer, 2048)
  → xor_crypt(buffer, nread, 0x42)  // Decrypt
  → write(tun_fd, buffer, nread)    // Inject to kernel

┌─────────────────────────────────────────────────────────────────┐
│ STEP 9: Server Kernel Injects Packet                            │
└─────────────────────────────────────────────────────────────────┘
Kernel path: drivers/net/tun.c:tun_chr_write_iter()
  → Receives IP packet from userspace
  → Injects into network stack: netif_rx_ni()
  → Kernel routes based on destination (8.8.8.8)
  → Routes out eth0 to real internet

┌─────────────────────────────────────────────────────────────────┐
│ STEP 10: Packet Reaches Destination                             │
└─────────────────────────────────────────────────────────────────┘
8.8.8.8 receives ICMP Echo Request
  → Sends ICMP Echo Reply back
  → Reverse path: Internet → Server eth0
```

### Receiving: Internet → VPN → App

The reply follows the reverse path:

```
1. Server eth0 receives ICMP reply (dst: 10.8.0.2 - client's TUN IP)
2. Server kernel routes to tun0 (because 10.8.0.0/24 is on tun0)
3. Server TUN device queues packet → VPN server reads it
4. VPN server encrypts and sends to client over TCP
5. Client VPN reads from server_fd
6. Client VPN decrypts and writes to tun_fd
7. Client kernel injects packet into network stack
8. Kernel delivers to ping socket (matches ICMP ID)
9. ping receives reply → displays "64 bytes from 8.8.8.8..."
```

## Step-by-Step Implementation

### Creating TUN Device (Kernel Perspective)

**File**: `drivers/net/tun.c`

```c
// When userspace calls: ioctl(fd, TUNSETIFF, &ifr)
static int tun_set_iff(struct net *net, struct file *file, struct ifreq *ifr)
{
    struct tun_struct *tun;
    struct net_device *dev;

    // Allocate network device structure
    dev = alloc_netdev(sizeof(struct tun_struct), ifr->ifr_name,
                       NET_NAME_UNKNOWN, tun_setup);

    // Set up device operations
    dev->netdev_ops = &tun_netdev_ops;  // Includes ndo_start_xmit

    // Register with kernel network stack
    err = register_netdevice(dev);

    // Link file descriptor to TUN device
    tun_attach(tun, file, ifr->ifr_flags & IFF_NOFILTER);

    return 0;
}
```

**Result**: `tun0` appears in `ip link show`, behaves like a real NIC

### TUN Device Operations

```c
static const struct net_device_ops tun_netdev_ops = {
    .ndo_start_xmit     = tun_net_xmit,    // Transmit packet
    .ndo_open           = tun_net_open,    // ifconfig up
    .ndo_stop           = tun_net_close,   // ifconfig down
    .ndo_change_mtu     = tun_net_change_mtu,
    // ... other operations
};
```

**ndo_start_xmit implementation**:

```c
static netdev_tx_t tun_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct tun_struct *tun = netdev_priv(dev);

    // Queue packet for userspace to read
    skb_queue_tail(&tun->readq, skb);

    // Wake up VPN process waiting in select()
    wake_up_interruptible_poll(&tun->wq.wait, ...);

    return NETDEV_TX_OK;  // No actual hardware transmission!
}
```

### Reading from TUN Device

**File**: `drivers/net/tun.c:tun_chr_read_iter()`

```c
static ssize_t tun_chr_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct tun_file *tfile = file->private_data;
    struct sk_buff *skb;

    // Dequeue packet that was queued by tun_net_xmit()
    skb = skb_array_consume(&tfile->tx_array);

    // Copy packet data to userspace buffer
    ret = skb_copy_datagram_iter(skb, 0, to, skb->len);

    return ret;
}
```

**Userspace sees**: Raw IP packet bytes

### Writing to TUN Device

**File**: `drivers/net/tun.c:tun_chr_write_iter()`

```c
static ssize_t tun_chr_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct tun_file *tfile = file->private_data;
    struct sk_buff *skb;

    // Allocate sk_buff
    skb = tun_alloc_skb(tfile, from->count);

    // Copy from userspace
    skb_copy_datagram_from_iter(skb, 0, from, len);

    // Inject into network stack
    netif_rx_ni(skb);  // Routes packet as if received from network

    return len;
}
```

**Result**: Kernel routes packet based on IP destination

## Running the VPN

### Terminal 1: VPN Server

```bash
# Compile
gcc -o simple_vpn_server src/simple_vpn_server.c

# Run server
sudo ./simple_vpn_server
```

### Terminal 2: Configure Server TUN

```bash
sudo ip addr add 10.8.0.1/24 dev tun0
sudo ip link set tun0 up
sudo sysctl -w net.ipv4.ip_forward=1  # Enable forwarding

# Add iptables rule to NAT VPN traffic
sudo iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -o eth0 -j MASQUERADE
```

### Terminal 3: VPN Client

```bash
# Compile
gcc -o simple_vpn_client src/simple_vpn_client.c

# Run client (replace with actual server IP)
sudo ./simple_vpn_client 192.168.1.100
```

### Terminal 4: Configure Client TUN

```bash
sudo ip addr add 10.8.0.2/24 dev tun0
sudo ip link set tun0 up
sudo ip route add 8.8.8.8/32 dev tun0  # Route Google DNS through VPN
```

### Terminal 5: Test!

```bash
# This traffic will go through the VPN!
ping 8.8.8.8

# Watch the VPN client/server terminals - you'll see:
# [TUN→SERVER] Read 84 bytes from TUN, encrypting...
# [SERVER→TUN] Received 84 bytes, decrypting...
```

## How Applications Are Unaware

The magic is that **applications don't know the VPN exists**:

```c
// Application code (same with or without VPN!)
int sock = socket(AF_INET, SOCK_DGRAM, 0);
struct sockaddr_in dest = {
    .sin_family = AF_INET,
    .sin_addr.s_addr = inet_addr("8.8.8.8"),  // Google DNS
    .sin_port = htons(53)
};
sendto(sock, dns_query, len, 0, &dest, sizeof(dest));
```

**Without VPN**:
```
App → Kernel → Routing table → eth0 → Internet
```

**With VPN** (after adding route):
```
App → Kernel → Routing table → tun0 → VPN client → Encryption → eth0 → Internet
```

**The app sends the exact same bytes** - the kernel's routing decision determines where the packet goes.

## Kernel Integration

### Routing Table Lookup

**File**: `net/ipv4/route.c:ip_route_output_flow()`

```c
struct rtable *ip_route_output_flow(struct net *net,
                                     struct flowi4 *flp4,
                                     const struct sock *sk)
{
    struct rtable *rt;

    // Look up routing table based on destination IP
    rt = __ip_route_output_key(net, flp4);

    // rt->dst.dev points to the output device (eth0, tun0, etc.)
    return rt;
}
```

### Longest Prefix Match

The kernel uses **longest prefix match** for routing:

```
Routes:
  8.8.8.8/32 dev tun0        ← /32 = most specific
  8.0.0.0/8 dev eth1         ← /8 = less specific
  default via gw dev eth0    ← /0 = least specific

Lookup for 8.8.8.8:
  → Matches /32 route → uses tun0
```

This is how you can route specific IPs through the VPN while leaving others direct.

### select() Integration

**File**: `drivers/net/tun.c:tun_chr_poll()`

```c
static __poll_t tun_chr_poll(struct file *file, poll_table *wait)
{
    struct tun_file *tfile = file->private_data;
    struct tun_struct *tun = tun_get(tfile);
    __poll_t mask = 0;

    // Add to wait queue
    poll_wait(file, &tfile->wq.wait, wait);

    // Check if data available
    if (!ptr_ring_empty(&tfile->tx_ring))
        mask |= EPOLLIN | EPOLLRDNORM;  // Readable!

    return mask;
}
```

When `select()` is called:
1. Kernel calls `tun_chr_poll()` for `tun_fd`
2. If no packets queued, adds VPN process to wait queue
3. When packet arrives, `tun_net_xmit()` wakes up wait queue
4. `select()` returns, indicating `tun_fd` is readable

## Key Takeaways

1. **TUN devices are virtual `struct net_device` structures** that redirect packets to userspace instead of hardware

2. **Routing table is the key** - it determines which interface handles which traffic

3. **Applications are completely unaware** - they use normal socket programming

4. **VPN uses select()** to multiplex between TUN device and server socket

5. **Symmetric paths**:
   - Outgoing: App → Kernel → TUN → VPN client → Encrypt → Server
   - Incoming: Server → Decrypt → VPN client → TUN → Kernel → App

6. **File descriptor integration** - TUN devices work with `select()`, `poll()`, `epoll()` like any other FD

7. **No special privileges needed for apps** - only the VPN client needs root (to create TUN device)

## Further Improvements

This simple VPN is educational but missing:
- **Real encryption** (use TLS/OpenSSL instead of XOR)
- **UDP transport** (better for VPN than TCP-over-TCP)
- **MTU handling** (fragmentation, PMTU discovery)
- **IPv6 support** (requires dual stack)
- **Persistent tunnels** (automatic reconnection)
- **Security** (authentication, key exchange)

For production VPNs, use established solutions like WireGuard, OpenVPN, or IPsec.

## Reference Files

- Client implementation: `src/simple_vpn_client.c`
- Server implementation: `src/simple_vpn_server.c`
- TUN device notes: `draft/tun-device-notes.md`
- Kernel TUN driver: `refs/linux/drivers/net/tun.c`
- Routing code: `refs/linux/net/ipv4/route.c`
