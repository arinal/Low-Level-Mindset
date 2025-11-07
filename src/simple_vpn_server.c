/*
 * Simple VPN Server
 *
 * This demonstrates a minimal VPN server that:
 * 1. Creates a TUN device (tun0)
 * 2. Accepts connections from VPN clients
 * 3. Receives encrypted packets from clients
 * 4. Decrypts and injects into TUN device (kernel routes them)
 * 5. Reads responses from TUN device and sends back to client
 *
 * Compile: gcc -o simple_vpn_server simple_vpn_server.c
 * Run: sudo ./simple_vpn_server
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
#define XOR_KEY 0x42  // Simple XOR "encryption" key (NOT SECURE!)

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
    printf("      sudo ip addr add 10.8.0.1/24 dev %s\n", ifr.ifr_name);
    printf("      sudo ip link set %s up\n", ifr.ifr_name);

    return tun_fd;
}

// Simple XOR encryption/decryption (symmetric)
void xor_crypt(unsigned char *data, int len, unsigned char key) {
    for (int i = 0; i < len; i++) {
        data[i] ^= key;
    }
}

// Create TCP server socket
int create_server_socket(int port) {
    int sock_fd;
    struct sockaddr_in server_addr;
    int opt = 1;

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Failed to create socket");
        return -1;
    }

    // Allow address reuse
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to bind");
        close(sock_fd);
        return -1;
    }

    if (listen(sock_fd, 5) < 0) {
        perror("Failed to listen");
        close(sock_fd);
        return -1;
    }

    printf("[SERVER] Listening on port %d\n", port);
    return sock_fd;
}

// Main event loop: multiplex between TUN device and client socket
void vpn_event_loop(int tun_fd, int client_fd) {
    unsigned char buffer[BUFFER_SIZE];
    fd_set read_fds;
    int max_fd;
    int nread;

    printf("[VPN] Starting event loop...\n");
    printf("[VPN] Forwarding packets between client and TUN device\n");

    max_fd = (tun_fd > client_fd) ? tun_fd : client_fd;

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(tun_fd, &read_fds);
        FD_SET(client_fd, &read_fds);

        // Block until data is available on either TUN device or client socket
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ret < 0) {
            perror("select() failed");
            break;
        }

        // Data from TUN device (kernel → client)
        // These are response packets that need to go back to the VPN client
        if (FD_ISSET(tun_fd, &read_fds)) {
            nread = read(tun_fd, buffer, BUFFER_SIZE);
            if (nread < 0) {
                perror("Failed to read from TUN device");
                break;
            }

            printf("[TUN→CLIENT] Read %d bytes from TUN, encrypting and sending to client\n", nread);

            // Encrypt the packet
            xor_crypt(buffer, nread, XOR_KEY);

            // Send packet length first (for framing)
            uint16_t packet_len = htons(nread);
            if (write(client_fd, &packet_len, sizeof(packet_len)) < 0) {
                perror("Failed to send packet length to client");
                break;
            }

            // Send encrypted packet to client
            if (write(client_fd, buffer, nread) < 0) {
                perror("Failed to send packet to client");
                break;
            }
        }

        // Data from client (client → TUN → kernel → internet)
        // These are outgoing packets from the VPN client
        if (FD_ISSET(client_fd, &read_fds)) {
            // Read packet length first
            uint16_t packet_len;
            nread = read(client_fd, &packet_len, sizeof(packet_len));
            if (nread <= 0) {
                printf("[CLIENT] Client disconnected\n");
                break;
            }
            packet_len = ntohs(packet_len);

            // Read encrypted packet
            nread = read(client_fd, buffer, packet_len);
            if (nread <= 0) {
                printf("[CLIENT] Client disconnected\n");
                break;
            }

            printf("[CLIENT→TUN] Received %d bytes from client, decrypting and injecting to TUN\n", nread);

            // Decrypt the packet
            xor_crypt(buffer, nread, XOR_KEY);

            // Write decrypted packet to TUN device
            // The kernel will route this packet based on the IP destination
            if (write(tun_fd, buffer, nread) < 0) {
                perror("Failed to write to TUN device");
                break;
            }
        }
    }
}

int main() {
    int tun_fd, server_fd, client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    char tun_name[IFNAMSIZ] = "tun0";

    printf("=== Simple VPN Server ===\n");

    // Step 1: Create TUN device
    tun_fd = create_tun_device(tun_name);
    if (tun_fd < 0) {
        fprintf(stderr, "Failed to create TUN device\n");
        exit(1);
    }

    printf("\n[SETUP] Please configure the TUN device in another terminal:\n");
    printf("        sudo ip addr add 10.8.0.1/24 dev tun0\n");
    printf("        sudo ip link set tun0 up\n");
    printf("        sudo sysctl -w net.ipv4.ip_forward=1\n");
    printf("\n[SETUP] Press Enter when ready...");
    getchar();

    // Step 2: Create server socket
    server_fd = create_server_socket(SERVER_PORT);
    if (server_fd < 0) {
        close(tun_fd);
        exit(1);
    }

    // Step 3: Accept client connection
    printf("[SERVER] Waiting for client connection...\n");
    client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        perror("Failed to accept client");
        close(server_fd);
        close(tun_fd);
        exit(1);
    }

    printf("[SERVER] Client connected from %s:%d\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // Step 4: Run VPN event loop
    vpn_event_loop(tun_fd, client_fd);

    // Cleanup
    close(client_fd);
    close(server_fd);
    close(tun_fd);

    printf("\n[SERVER] Shutting down\n");
    return 0;
}