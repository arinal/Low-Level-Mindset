# Simple VPN - Quick Start Guide

A minimal educational VPN implementation using TUN devices to demonstrate Linux kernel networking concepts.

## Overview

This simple VPN consists of two programs:
- **simple_vpn_server**: Receives encrypted packets and forwards to the internet
- **simple_vpn_client**: Captures local traffic, encrypts, and tunnels through server

**Note**: This uses XOR "encryption" for simplicity - **NOT SECURE**. For educational purposes only!

## Prerequisites

```bash
# Ensure TUN module is loaded
sudo modprobe tun

# Verify TUN module
lsmod | grep tun
```

## Compilation

```bash
# Compile server
gcc -o simple_vpn_server simple_vpn_server.c

# Compile client
gcc -o simple_vpn_client simple_vpn_client.c
```

## Setup and Usage

### Step 1: Start the Server

On the server machine (e.g., 192.168.1.100):

```bash
# Terminal 1: Run server
sudo ./simple_vpn_server
```

The server will prompt you to configure the TUN device. In a new terminal:

```bash
# Terminal 2: Configure server TUN device
sudo ip addr add 10.8.0.1/24 dev tun0
sudo ip link set tun0 up

# Enable IP forwarding (allow server to route traffic)
sudo sysctl -w net.ipv4.ip_forward=1

# NAT traffic from VPN clients to internet
sudo iptables -t nat -A POSTROUTING -s 10.8.0.0/24 -o eth0 -j MASQUERADE
```

Press Enter in Terminal 1 to continue. Server will wait for client connection.

### Step 2: Start the Client

On the client machine:

```bash
# Terminal 1: Run client (replace IP with your server's IP)
sudo ./simple_vpn_client 192.168.1.100
```

The client will prompt you to configure the TUN device. In a new terminal:

```bash
# Terminal 2: Configure client TUN device
sudo ip addr add 10.8.0.2/24 dev tun0
sudo ip link set tun0 up

# Route specific IP through VPN (Google DNS as example)
sudo ip route add 8.8.8.8/32 dev tun0
```

Press Enter in Terminal 1. Client will connect to server.

### Step 3: Test the VPN

```bash
# All traffic to 8.8.8.8 now goes through VPN!
ping 8.8.8.8

# Watch the terminals - you'll see packet flow:
# Client: [TUN→SERVER] Read 84 bytes from TUN, encrypting...
# Server: [CLIENT→TUN] Received 84 bytes, decrypting...
```

### Step 4: Verify Traffic Flow

**Check routing**:
```bash
ip route show
# You should see: 8.8.8.8 dev tun0 scope link
```

**Check TUN interface**:
```bash
ip link show tun0
# Should show: state UNKNOWN ... UP
```

**Monitor packets**:
```bash
# On client, watch TUN device
sudo tcpdump -i tun0 -n

# In another terminal, ping
ping 8.8.8.8
```

## How It Works

### Packet Flow Diagram

```
CLIENT                                              SERVER
======                                              ======

Application (ping)
    │
    ▼
Kernel: IP routing
    │  (Routes to tun0 based on routing table)
    ▼
TUN Device (tun0)
    │  (Queues packet)
    ▼
VPN Client ─────[Encrypts]────► TCP ────────────► VPN Server
                                                       │
                                                       ▼
                                                  [Decrypts]
                                                       │
                                                       ▼
                                                  TUN Device (tun0)
                                                       │
                                                       ▼
                                                  Kernel: IP routing
                                                       │
                                                       ▼
                                                  eth0 → Internet → 8.8.8.8
```

### Key Concepts

1. **TUN Device**: Virtual network interface that operates at Layer 3 (IP)
2. **Routing Table**: Determines which traffic goes through VPN
3. **select()**: Multiplexes between TUN device and server socket
4. **Transparency**: Applications are completely unaware of VPN

## Routing Examples

### Route Single IP
```bash
# Route only 8.8.8.8 through VPN
sudo ip route add 8.8.8.8/32 dev tun0
```

### Route Entire Subnet
```bash
# Route all 10.x.x.x addresses through VPN
sudo ip route add 10.0.0.0/8 dev tun0
```

### Route All Traffic (Full Tunnel)
```bash
# Save default gateway first
DEFAULT_GW=$(ip route | grep default | awk '{print $3}')

# Add route to VPN server directly (avoid routing loop!)
sudo ip route add 192.168.1.100/32 via $DEFAULT_GW

# Route everything through VPN
sudo ip route add 0.0.0.0/1 dev tun0
sudo ip route add 128.0.0.0/1 dev tun0
```

## Cleanup

### Remove Routes
```bash
sudo ip route del 8.8.8.8/32 dev tun0
```

### Remove NAT Rule (on server)
```bash
sudo iptables -t nat -D POSTROUTING -s 10.8.0.0/24 -o eth0 -j MASQUERADE
```

### Stop VPN
- Press Ctrl+C in both client and server terminals
- TUN devices will be automatically removed

## Troubleshooting

### "Failed to open /dev/net/tun"
```bash
# Load TUN module
sudo modprobe tun

# Create device if missing
sudo mkdir -p /dev/net
sudo mknod /dev/net/tun c 10 200
sudo chmod 0666 /dev/net/tun
```

### "Failed to connect to server"
- Check server is running and listening
- Verify server IP address
- Check firewall: `sudo iptables -L -n | grep 5555`
- Allow port: `sudo iptables -A INPUT -p tcp --dport 5555 -j ACCEPT`

### "No route to host"
- Ensure TUN device is UP: `ip link set tun0 up`
- Check routing table: `ip route show`
- Verify IP addresses match (client: 10.8.0.2, server: 10.8.0.1)

### Packets Not Flowing
```bash
# Check IP forwarding (on server)
cat /proc/sys/net/ipv4/ip_forward
# Should be 1

# Check NAT rules (on server)
sudo iptables -t nat -L -n -v
# Should show POSTROUTING MASQUERADE rule

# Debug with tcpdump
sudo tcpdump -i tun0 -n icmp
```

## Architecture Overview

### Client Components
- **TUN Device**: Captures outgoing packets from applications
- **Event Loop**: Uses `select()` to monitor TUN device and server socket
- **Encryption**: XOR with key 0x42 (educational only!)
- **TCP Client**: Connects to server on port 5555

### Server Components
- **TCP Server**: Accepts client connections on port 5555
- **TUN Device**: Injects decrypted packets into kernel
- **Event Loop**: Multiplexes between client socket and TUN device
- **Forwarding**: Kernel routes packets to internet via NAT

## Security Warning

**This implementation uses simple XOR "encryption" which provides NO SECURITY**.

For real-world use:
- Use proper encryption: TLS, WireGuard, IPsec
- Implement authentication
- Use secure key exchange
- Consider UDP instead of TCP (avoid TCP-over-TCP performance issues)
- Implement proper error handling and reconnection logic

## Further Reading

- **Detailed explanation**: See `draft/simple-vpn-implementation.md`
- **TUN device concepts**: See `draft/tun-device-notes.md`
- **Linux kernel TUN driver**: `refs/linux/drivers/net/tun.c`

## Example Session

```bash
# Server Terminal 1
$ sudo ./simple_vpn_server
=== Simple VPN Server ===
[TUN] Created TUN device: tun0
[SETUP] Please configure the TUN device...
<press Enter>
[SERVER] Listening on port 5555
[SERVER] Client connected from 192.168.1.50:54321
[VPN] Starting event loop...

# Client Terminal 1
$ sudo ./simple_vpn_client 192.168.1.100
=== Simple VPN Client ===
[TUN] Created TUN device: tun0
[SETUP] Please configure the TUN device...
<press Enter>
[CLIENT] Connecting to server 192.168.1.100:5555...
[CLIENT] Connected to VPN server!
[VPN] Starting event loop...

# Client Terminal 2
$ ping -c 2 8.8.8.8
PING 8.8.8.8 (8.8.8.8) 56(84) bytes of data.
64 bytes from 8.8.8.8: icmp_seq=1 ttl=116 time=42.3 ms
64 bytes from 8.8.8.8: icmp_seq=2 ttl=116 time=41.8 ms

# You'll see packet flow in VPN terminals!
```

## License

Educational code - use freely for learning purposes.
