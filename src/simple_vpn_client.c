/*
 * Simple VPN Client
 *
 * This demonstrates a minimal VPN client that:
 * 1. Creates a TUN device (tun0)
 * 2. Connects to VPN server
 * 3. Reads packets from TUN device (app traffic)
 * 4. Encrypts and sends to server
 * 5. Receives encrypted packets from server
 * 6. Decrypts and injects back into TUN device
 *
 * Applications using this VPN don't know it exists - they just use normal
 * socket programming, and the kernel routes their traffic through tun0.
 *
 * Compile: gcc -o simple_vpn_client simple_vpn_client.c
 * Run: sudo ./simple_vpn_client <server_ip>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define SERVER_PORT 5555
#define TUN_DEVICE "/dev/net/tun"
#define BUFFER_SIZE 2048
#define XOR_KEY 0x42  // Simple XOR "encryption" key (must match server!)

// Create and configure TUN device
int create_tun_device(char *dev_name) {
    struct ifreq ifr;
    int tun_fd, err;

    // Open the TUN device
    tun_fd = open(TUN_DEVICE, O_RDWR);
    if (tun_fd < 0) {
        perror("Failed to open /dev/net/tun");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));

    // IFF_TUN: TUN device (Layer 3, IP packets)
    // IFF_NO_PI: No packet information (just raw IP packets)
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strncpy(ifr.ifr_name, dev_name, IFNAMSIZ);

    // Create the TUN interface
    err = ioctl(tun_fd, TUNSETIFF, (void *)&ifr);
    if (err < 0) {
        perror("Failed to configure TUN device");
        close(tun_fd);
        return -1;
    }

    printf("[TUN] Created TUN device: %s\n", ifr.ifr_name);
    printf("[TUN] Configure it with:\n");
    printf("      sudo ip addr add 10.8.0.2/24 dev %s\n", ifr.ifr_name);
    printf("      sudo ip link set %s up\n", ifr.ifr_name);
    printf("      sudo ip route add 8.8.8.8/32 dev %s\n", ifr.ifr_name);

    return tun_fd;
}

// Simple XOR encryption/decryption (symmetric)
void xor_crypt(unsigned char *data, int len, unsigned char key) {
    for (int i = 0; i < len; i++) {
        data[i] ^= key;
    }
}

// Connect to VPN server
int connect_to_server(const char *server_ip, int port) {
    int sock_fd;
    struct sockaddr_in server_addr;

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Failed to create socket");
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP address: %s\n", server_ip);
        close(sock_fd);
        return -1;
    }

    printf("[CLIENT] Connecting to server %s:%d...\n", server_ip, port);

    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to connect to server");
        close(sock_fd);
        return -1;
    }

    printf("[CLIENT] Connected to VPN server!\n");
    return sock_fd;
}

// Main event loop: multiplex between TUN device and server socket
void vpn_event_loop(int tun_fd, int server_fd) {
    unsigned char buffer[BUFFER_SIZE];
    fd_set read_fds;
    int max_fd;
    int nread;

    printf("[VPN] Starting event loop...\n");
    printf("[VPN] All traffic to 8.8.8.8 will be tunneled through VPN!\n");
    printf("[VPN] Try: ping 8.8.8.8\n");

    max_fd = (tun_fd > server_fd) ? tun_fd : server_fd;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(tun_fd, &read_fds);
        FD_SET(server_fd, &read_fds);

        // Block until data is available on either TUN device or server socket
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ret < 0) {
            perror("select() failed");
            break;
        }

        // Data from TUN device (app → server)
        // This is when an application on this machine sends a packet that
        // matches our routing table (e.g., ping 8.8.8.8)
        if (FD_ISSET(tun_fd, &read_fds)) {
            nread = read(tun_fd, buffer, BUFFER_SIZE);
            if (nread < 0) {
                perror("Failed to read from TUN device");
                break;
            }

            printf("[TUN→SERVER] Read %d bytes from TUN (app sent packet), encrypting and forwarding to server\n", nread);

            // Encrypt the packet
            xor_crypt(buffer, nread, XOR_KEY);

            // Send packet length first (for framing)
            uint16_t packet_len = htons(nread);
            if (write(server_fd, &packet_len, sizeof(packet_len)) < 0) {
                perror("Failed to send packet length to server");
                break;
            }

            // Send encrypted packet to server
            if (write(server_fd, buffer, nread) < 0) {
                perror("Failed to send packet to server");
                break;
            }
        }

        // Data from server (server → app)
        // These are response packets (e.g., ping replies) coming back
        if (FD_ISSET(server_fd, &read_fds)) {
            // Read packet length first
            uint16_t packet_len;
            nread = read(server_fd, &packet_len, sizeof(packet_len));
            if (nread <= 0) {
                printf("[SERVER] Server disconnected\n");
                break;
            }
            packet_len = ntohs(packet_len);

            // Read encrypted packet
            nread = read(server_fd, buffer, packet_len);
            if (nread <= 0) {
                printf("[SERVER] Server disconnected\n");
                break;
            }

            printf("[SERVER→TUN] Received %d bytes from server, decrypting and injecting to TUN\n", nread);

            // Decrypt the packet
            xor_crypt(buffer, nread, XOR_KEY);

            // Write decrypted packet to TUN device
            // This injects the packet into the kernel's network stack
            // The kernel will route it to the appropriate application socket
            if (write(tun_fd, buffer, nread) < 0) {
                perror("Failed to write to TUN device");
                break;
            }
        }
    }
}

void print_usage(const char *prog_name) {
    printf("Usage: %s <server_ip>\n", prog_name);
    printf("Example: %s 192.168.1.100\n", prog_name);
}

int main(int argc, char *argv[]) {
    int tun_fd, server_fd;
    char tun_name[IFNAMSIZ] = "tun0";

    if (argc != 2) {
        print_usage(argv[0]);
        exit(1);
    }

    printf("=== Simple VPN Client ===\n");

    // Step 1: Create TUN device
    tun_fd = create_tun_device(tun_name);
    if (tun_fd < 0) {
        fprintf(stderr, "Failed to create TUN device\n");
        fprintf(stderr, "Make sure:\n");
        fprintf(stderr, "  1. You're running as root (sudo)\n");
        fprintf(stderr, "  2. TUN module is loaded (modprobe tun)\n");
        exit(1);
    }

    printf("\n[SETUP] Please configure the TUN device in another terminal:\n");
    printf("        sudo ip addr add 10.8.0.2/24 dev tun0\n");
    printf("        sudo ip link set tun0 up\n");
    printf("        sudo ip route add 8.8.8.8/32 dev tun0\n");
    printf("\n[SETUP] This routes 8.8.8.8 through the VPN tunnel\n");
    printf("[SETUP] Press Enter when ready...");
    getchar();

    // Step 2: Connect to VPN server
    server_fd = connect_to_server(argv[1], SERVER_PORT);
    if (server_fd < 0) {
        close(tun_fd);
        exit(1);
    }

    // Step 3: Run VPN event loop
    vpn_event_loop(tun_fd, server_fd);

    // Cleanup
    close(server_fd);
    close(tun_fd);

    printf("\n[CLIENT] Shutting down\n");
    return 0;
}
