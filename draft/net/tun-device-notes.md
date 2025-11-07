# TUN Device Notes

## What is a TUN Device?

A **TUN device** is a virtual network interface that operates at the **network layer (Layer 3)** of the OSI model. It's used to create tunnels for routing IP packets.

### Normal vs VPN Network Flow

**Normal network flow:**
```
Application → Kernel → Physical NIC → Network
```

**VPN with TUN device:**
```
Application → Kernel → TUN device → VPN software → Encryption → Physical NIC → Network
```

### Key Characteristics

- **Layer 3 (IP packets)**: Handles complete IP packets (vs TAP which handles Ethernet frames)
- **Virtual interface**: Shows up like a real network card (e.g., `tun0`, `tun1`)
- **User-space access**: Applications like OpenConnect can read/write raw IP packets
- **Kernel module**: Requires the `tun` kernel module to be loaded

## Application Perspective

From the application's perspective, **VPN is completely transparent**. Apps use normal socket programming:

```c
int sock = socket(AF_INET, SOCK_STREAM, 0);
connect(sock, ...);
send(sock, data, len, 0);
```

The app has **no idea** a VPN exists.

### What Actually Happens

**Without VPN:**
```
App → socket() → Kernel routing → eth0 → Internet → Destination
```

**With VPN (transparent to app):**
```
App → socket() → Kernel routing → tun0 → OpenConnect → Encryption →
    → eth0 → VPN Server → Decryption → Destination
```

### The Routing Trick

1. **App sends packet** to destination IP (e.g., 10.20.30.40)
2. **Kernel checks routing table** (`ip route show`)
3. **Kernel routes to TUN device** (e.g., `10.0.0.0/8 dev tun0`)
4. **OpenConnect reads from TUN device**, encrypts, and forwards
5. **App is completely unaware**

## How OpenConnect Gets Notified

OpenConnect communicates with the TUN device through a **file descriptor**.

### TUN Device Creation

```c
// Simplified version
int tun_fd = open("/dev/net/tun", O_RDWR);
struct ifreq ifr;
ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
strcpy(ifr.ifr_name, "tun0");
ioctl(tun_fd, TUNSETIFF, &ifr);  // Creates tun0 interface
```

### Event Loop with select()

OpenConnect uses `select()`, `poll()`, or `epoll()` to monitor file descriptors:

```c
// Simplified event loop
while (vpn_connected) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(tun_fd, &rfds);        // Monitor TUN device
    FD_SET(ssl_socket, &rfds);    // Monitor VPN server socket

    select(max_fd + 1, &rfds, NULL, NULL, NULL);  // BLOCKS here

    if (FD_ISSET(tun_fd, &rfds)) {
        // App sent packet → kernel → TUN device → ready to read!
        read(tun_fd, buffer, sizeof(buffer));
        encrypt(buffer);
        send_to_vpn_server(buffer);
    }

    if (FD_ISSET(ssl_socket, &rfds)) {
        // VPN server sent encrypted data
        recv_from_vpn_server(buffer);
        decrypt(buffer);
        write(tun_fd, buffer, len);  // Inject into kernel
    }
}
```

### Packet Flow - Sending

```
1. App: send(sock, data, len, 0)
2. Kernel: Routes to tun0 based on routing table
3. Kernel: Buffers packet in TUN device queue
4. select() returns: tun_fd is readable! ← NOTIFICATION
5. OpenConnect: read(tun_fd, buf, size)
6. OpenConnect: Encrypts and forwards to VPN server
```

### Packet Flow - Receiving

```
1. VPN server sends encrypted packet
2. select() returns: ssl_socket is readable!
3. OpenConnect: Decrypts packet
4. OpenConnect: write(tun_fd, decrypted_packet, len)
5. Kernel: Injects packet into network stack
6. Kernel: Routes to correct application socket
7. App: recv(sock, buf, len, 0) returns the data
```

## Common Issue: Missing TUN Module After Kernel Update

### Problem

After `pacman -Syu` updates the kernel, you might see:

```
modprobe: FATAL: Module tun not found in directory /lib/modules/6.17.5-arch1-1
```

Or in OpenConnect logs:

```
Failed to open tun device: No such device
Set up tun device failed
```

### Why This Happens

When kernel updates:
- Old kernel still running in memory (e.g., 6.17.5)
- Old kernel modules deleted from `/lib/modules/6.17.5-arch1-1/`
- New kernel modules installed in `/lib/modules/6.17.7-arch1-1/`
- `modprobe` can't find modules for the running kernel

### Solution

Simply reboot to load the new kernel:

```bash
sudo reboot
```

After reboot, check kernel version:

```bash
uname -r
```

Verify TUN module is loaded:

```bash
lsmod | grep tun
```

### Making TUN Persistent

To auto-load TUN module on boot:

```bash
echo "tun" | sudo tee /etc/modules-load.d/tun.conf
```

## VPN Connection Command

```bash
sudo gpclient --fix-openssl connect --hip --csd-wrapper $(which gohip) ds-connect-me.disney.com
```

### Command Breakdown

- `sudo` - Administrator privileges
- `gpclient` - GlobalProtect VPN client CLI
- `--fix-openssl` - Workaround for OpenSSL compatibility
- `connect` - Initiate VPN connection
- `--hip` - Enable Host Integrity Protection
- `--csd-wrapper $(which gohip)` - Use gohip for security checks
- `ds-connect-me.disney.com` - VPN gateway server

## Useful Commands

Check TUN interfaces:
```bash
ip link show | grep tun
```

View routing table:
```bash
ip route show
```

Check loaded kernel modules:
```bash
lsmod | grep tun
```

List available kernel module directories:
```bash
ls /lib/modules/
```

Current kernel version:
```bash
uname -r
```

## Summary

- **TUN device = virtual network interface** that operates at Layer 3 (IP)
- **Apps are unaware of VPN** - they use normal socket programming
- **Kernel routing** directs traffic through TUN device
- **OpenConnect uses file descriptors** and `select()` for notifications
- **After kernel updates**, reboot to load new kernel and modules
- **VPN is transparent** - no application changes needed
