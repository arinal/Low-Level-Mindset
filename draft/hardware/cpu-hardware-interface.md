# CPU Hardware Interface: How Software Controls the Physical World

This document explains the fundamental ways a CPU interfaces with hardware, from direct voltage control to complex memory-mapped peripherals.

## Table of Contents
- [The Core Concept: Memory-Mapped vs Port-Mapped I/O](#the-core-concept-memory-mapped-vs-port-mapped-io)
- [Level 1: Direct Voltage Control (GPIO)](#level-1-direct-voltage-control-gpio)
- [Level 2: Timed Protocols (I2C, SPI, UART)](#level-2-timed-protocols-i2c-spi-uart)
- [Level 3: Memory-Mapped Registers (PCI Devices)](#level-3-memory-mapped-registers-pci-devices)
- [Level 4: DMA - Hardware Direct Memory Access](#level-4-dma---hardware-direct-memory-access)
- [The Progression: Simple to Complex](#the-progression-simple-to-complex)

## The Core Concept: Memory-Mapped vs Port-Mapped I/O

At the fundamental level, a CPU only understands two operations:
1. **Read from an address** - `MOV RAX, [address]`
2. **Write to an address** - `MOV [address], RAX`

Hardware interfacing is about mapping physical devices to these read/write operations.

### Two Approaches

#### Port-Mapped I/O (x86 Legacy)

x86 CPUs have separate **I/O port space** (0x0000 to 0xFFFF):

```assembly
; Read from I/O port 0x3F8 (serial port)
IN AL, 0x3F8

; Write to I/O port 0x3F8
OUT 0x3F8, AL
```

These use special `IN`/`OUT` instructions, separate from memory space.

#### Memory-Mapped I/O (Modern Approach)

Hardware registers appear as **memory addresses**:

```assembly
; Read from memory-mapped register at 0xFE000000
MOV RAX, [0xFE000000]

; Write to memory-mapped register
MOV [0xFE000000], 0x42
```

**Same instructions as regular memory access!**

### Address Space Layout

```
0x0000000000000000
    │
    ├─ RAM (0x0000000000000000 - 0x000000007FFFFFFF)  [2GB]
    │  Application memory, kernel memory
    │
    ├─ Memory Gap / Reserved
    │
    ├─ PCI Device 1 (0x00000000FE000000 - 0x00000000FE0FFFFF)  [1MB]
    │  GPU registers, framebuffer
    │
    ├─ PCI Device 2 (0x00000000FE100000 - 0x00000000FE1FFFFF)  [1MB]
    │  Network card registers
    │
    ├─ GPIO Controller (0x00000000FE200000 - 0x00000000FE2000FF)  [256 bytes]
    │  Pin control registers
    │
    └─ More devices...
0xFFFFFFFFFFFFFFFF
```

View on Linux:
```bash
$ sudo cat /proc/iomem
00000000-00000fff : Reserved
00001000-0009ffff : System RAM
000a0000-000bffff : PCI Bus 0000:00
000c0000-000c7fff : Video ROM
fe000000-fe7fffff : PCI Bus 0000:00
  fe000000-fe003fff : 0000:00:02.0  # GPU registers
```

## Level 1: Direct Voltage Control (GPIO)

**GPIO (General Purpose Input/Output)** - the most direct form of CPU control over hardware.

### The Physical Reality

A GPIO pin is literally an electrical connection that the CPU can:
- **Output**: Set to HIGH (3.3V or 5V) or LOW (0V)
- **Input**: Read as HIGH or LOW

```
CPU ──────┬─── GPIO Pin 12 ──── LED ──── Resistor ──── Ground
          │
          └─── GPIO Pin 13 ──── Motor Driver ──── DC Motor
```

### How It Works

#### GPIO Controller Hardware

Inside the SoC (System on Chip), there's a GPIO controller:

```
┌────────────────────────────────────────────────────┐
│           GPIO Controller                          │
│                                                    │
│  ┌──────────────────────────────────────────┐    │
│  │  Control Registers (Memory-Mapped)       │    │
│  │                                           │    │
│  │  GPFSEL0 (0xFE200000) - Pin Function     │    │
│  │  GPSET0  (0xFE20001C) - Set Pin HIGH     │    │
│  │  GPCLR0  (0xFE200028) - Clear Pin LOW    │    │
│  │  GPLEV0  (0xFE200034) - Read Pin Level   │    │
│  └──────────────┬────────────────────────────┘    │
│                 │                                  │
│                 ▼                                  │
│  ┌──────────────────────────────────────────┐    │
│  │  Output Driver Circuits                  │    │
│  │  (Transistors that actually switch       │    │
│  │   voltage on physical pins)              │    │
│  └──────────────┬────────────────────────────┘    │
└─────────────────┼────────────────────────────────┘
                  │
                  ▼
         Physical Pins (exposed on board)
         GPIO 12, 13, 14...
```

### Example: Blinking an LED (Raspberry Pi)

#### Step 1: Configure Pin as Output

```c
// Memory-map the GPIO registers
volatile uint32_t *gpio = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                                MAP_SHARED, mem_fd, GPIO_BASE);

// GPIO Function Select Register 1 (controls pins 10-19)
// Each pin uses 3 bits: 000=Input, 001=Output, 010-111=Alt functions
volatile uint32_t *GPFSEL1 = gpio + 1;  // Offset 0x04

// Set GPIO 12 as output
*GPFSEL1 &= ~(7 << 6);   // Clear bits 6-8
*GPFSEL1 |= (1 << 6);    // Set to 001 (output)
```

**What just happened?**
- CPU wrote to address `GPIO_BASE + 0x04`
- This address is memory-mapped to GPIO controller hardware
- Hardware configured pin 12's transistor as output driver

#### Step 2: Turn LED ON

```c
// GPIO Set Register (set pin HIGH)
volatile uint32_t *GPSET0 = gpio + 7;  // Offset 0x1C

*GPSET0 = (1 << 12);  // Set bit 12 = Set GPIO 12 HIGH
```

**Physical result:**
- Hardware sets pin 12 to 3.3V
- Current flows: Pin 12 → LED → Resistor → Ground
- LED illuminates!

#### Step 3: Turn LED OFF

```c
// GPIO Clear Register (set pin LOW)
volatile uint32_t *GPCLR0 = gpio + 10;  // Offset 0x28

*GPCLR0 = (1 << 12);  // Set bit 12 = Clear GPIO 12 to LOW
```

**Physical result:**
- Hardware sets pin 12 to 0V
- No voltage difference, no current flow
- LED turns off

### Complete Example

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GPIO_BASE 0xFE200000  // Raspberry Pi 4
#define PAGE_SIZE 4096

int main() {
    int mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        perror("Failed to open /dev/mem");
        return 1;
    }

    // Memory-map GPIO registers
    void *gpio_map = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                          MAP_SHARED, mem_fd, GPIO_BASE);

    volatile uint32_t *gpio = (volatile uint32_t *)gpio_map;

    // Configure GPIO 12 as output
    *(gpio + 1) &= ~(7 << 6);
    *(gpio + 1) |= (1 << 6);

    // Blink LED 10 times
    for (int i = 0; i < 10; i++) {
        *(gpio + 7) = (1 << 12);   // LED ON
        usleep(500000);             // Wait 500ms
        *(gpio + 10) = (1 << 12);  // LED OFF
        usleep(500000);
    }

    munmap(gpio_map, PAGE_SIZE);
    close(mem_fd);
    return 0;
}
```

### The Reality Check

When you write `*GPSET0 = (1 << 12)`:

1. **CPU executes**: `MOV [address], value`
2. **Memory controller** sees: "This address is not RAM, it's in I/O space"
3. **Bus routes** write to GPIO controller hardware
4. **GPIO controller** decodes: "Set pin 12 HIGH"
5. **Output transistor** in controller switches ON
6. **Physical pin** goes to 3.3V
7. **Electrons flow** through LED

**From software instruction to physical voltage: microseconds!**

## Level 2: Timed Protocols (I2C, SPI, UART)

More complex devices need **serial communication protocols** with specific timing.

### Why Timing Matters

Unlike GPIO (just HIGH/LOW), serial protocols encode data in **timing patterns**:

```
I2C Data Transmission:

     ┌─────┐     ┌─────┐     ┌─────┐
SDA: │     │     │     │     │     │
     │     └─────┘     └─────┘     └───── (Data: 1 0 1)
     │
SCL: ┌───┐ ┌───┐ ┌───┐ ┌───┐
     │   │ │   │ │   │ │   │
     └───┘ └───┘ └───┘ └───┘  (Clock)
        ^     ^     ^
        │     │     └─── Read bit 3
        │     └───────── Read bit 2
        └─────────────── Read bit 1
```

### I2C: Inter-Integrated Circuit

**Two-wire bus**: SDA (data), SCL (clock)

#### Physical Setup

```
CPU/SoC ─┬─── SDA ───┬─── Temperature Sensor (0x48)
         │           │
         └─── SCL ───┼─── EEPROM (0x50)
                     │
                     └─── Real-Time Clock (0x68)
```

Multiple devices share the same two wires!

#### I2C Transaction Breakdown

**Task**: Read temperature from sensor at address 0x48, register 0x00

```
START condition:
  - SDA goes LOW while SCL is HIGH

Address + Write bit:
  SCL: ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐
       │ │ │ │ │ │ │ │ │ │ │ │ │ │ │ │
       └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘
  SDA: 0   1   0   0   1   0   0   0   (0x48 << 1 | 0 = 0x90)
       └─────────┬─────────┘   └─┘
           Device Address      W=0

ACK from device:
  SDA pulled LOW by sensor (acknowledges)

Register address:
  Send 0x00 (temperature register)

REPEATED START

Address + Read bit:
  Send 0x91 (0x48 << 1 | 1)

Read data bytes:
  Master reads, sensor sends temperature data

STOP condition:
  - SDA goes HIGH while SCL is HIGH
```

#### Software Implementation

**Bit-banging** (manual timing):

```c
// GPIO pins for I2C
#define SDA_PIN 2
#define SCL_PIN 3

void i2c_start() {
    sda_high();
    scl_high();
    usleep(5);      // Setup time
    sda_low();      // SDA goes low while SCL high = START
    usleep(5);
    scl_low();
}

void i2c_write_bit(int bit) {
    if (bit)
        sda_high();
    else
        sda_low();

    usleep(5);      // Data setup time
    scl_high();     // Clock HIGH - slave reads bit now
    usleep(5);      // Clock high time
    scl_low();      // Clock LOW - prepare next bit
    usleep(5);
}

void i2c_write_byte(uint8_t byte) {
    for (int i = 7; i >= 0; i--) {
        i2c_write_bit((byte >> i) & 1);
    }

    // Read ACK bit
    sda_high();     // Release SDA
    scl_high();
    int ack = sda_read();  // Slave pulls low if ACK
    scl_low();
}

uint8_t read_temperature() {
    i2c_start();
    i2c_write_byte(0x90);  // Address + Write
    i2c_write_byte(0x00);  // Register 0x00

    i2c_start();           // Repeated start
    i2c_write_byte(0x91);  // Address + Read

    uint8_t temp = i2c_read_byte();
    i2c_stop();

    return temp;
}
```

**Hardware I2C controller** (better approach):

Modern SoCs have I2C controllers that handle timing automatically:

```c
// Configure I2C controller registers
volatile uint32_t *i2c = mmap(..., I2C_BASE, ...);

// I2C Control Register
#define I2C_C     (i2c + 0x00)
#define I2C_S     (i2c + 0x01)  // Status
#define I2C_DLEN  (i2c + 0x02)  // Data length
#define I2C_A     (i2c + 0x03)  // Slave address
#define I2C_FIFO  (i2c + 0x04)  // Data FIFO

// Read from device 0x48
*I2C_A = 0x48;           // Set slave address
*I2C_DLEN = 1;           // Read 1 byte
*I2C_C = I2C_READ | I2C_START;  // Start transaction

// Hardware handles all timing automatically!
while (!(*I2C_S & I2C_DONE));  // Wait for completion
uint8_t data = *I2C_FIFO;      // Read data
```

### SPI: Serial Peripheral Interface

**Four-wire bus**: MOSI (Master Out), MISO (Master In), SCLK (Clock), CS (Chip Select)

Faster than I2C, but needs more wires.

```
CPU ─┬─── MOSI ───┬─── SD Card
     ├─── MISO ───┤
     ├─── SCLK ───┤
     └─── CS0  ───┘
     └─── CS1  ─────── LCD Display
```

**Timing diagram**:

```
CS:   ────┐                                   ┌────
          └───────────────────────────────────┘

SCLK: ────┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌────
          └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘

MOSI: ────X─7─X─6─X─5─X─4─X─3─X─2─X─1─X─0─X────
          (Master sends byte)

MISO: ────X─7─X─6─X─5─X─4─X─3─X─2─X─1─X─0─X────
          (Slave sends byte simultaneously)
```

**Full-duplex**: Both send and receive at the same time!

### The Key Insight

These protocols require **precise timing**:
- I2C: ~100kHz to 400kHz clock
- SPI: Up to 10s of MHz

**Bit-banging in software** is unreliable because:
- Linux is not real-time
- Interrupt could delay your `usleep()`
- CPU might be doing something else

**Solution**: Hardware controllers that handle timing in silicon.

## Level 3: Memory-Mapped Registers (PCI Devices)

Modern devices (GPUs, NICs, NVMe drives) use **PCI/PCIe** with memory-mapped registers.

### PCI Configuration Space

Every PCI device has a 256-byte configuration space:

```bash
$ sudo lspci -vvv -s 00:02.0  # GPU
00:02.0 VGA compatible controller: Intel Corporation UHD Graphics
    ...
    Region 0: Memory at fc000000 (64-bit, non-prefetchable) [size=16M]
    Region 2: Memory at e0000000 (64-bit, prefetchable) [size=256M]
```

**Translation**:
- Registers at `0xFC000000` - `0xFCFFFFFF` (16MB)
- Framebuffer at `0xE0000000` - `0xEFFFFFFF` (256MB)

### BAR: Base Address Register

PCI devices expose memory regions called **BARs**:

```c
// PCI configuration space (mapped by kernel)
struct pci_dev *pdev;

// BAR 0: Device control registers
void __iomem *registers = pci_iomap(pdev, 0, 0);

// BAR 2: Framebuffer memory
void __iomem *framebuffer = pci_iomap(pdev, 2, 0);
```

### Example: Network Card

An Intel E1000 network card has registers like:

```c
// Memory-mapped register offsets
#define E1000_CTRL     0x00000  // Device Control
#define E1000_STATUS   0x00008  // Device Status
#define E1000_TDBAL    0x03800  // TX Descriptor Base Low
#define E1000_TDBAH    0x03804  // TX Descriptor Base High
#define E1000_TDH      0x03810  // TX Descriptor Head
#define E1000_TDT      0x03818  // TX Descriptor Tail

// Send a packet
void send_packet(struct e1000_hw *hw, void *data, size_t len) {
    // 1. Write packet data to TX buffer
    memcpy(tx_buffer[tx_tail], data, len);

    // 2. Update descriptor
    tx_ring[tx_tail].buffer_addr = tx_buffer_phys[tx_tail];
    tx_ring[tx_tail].length = len;
    tx_ring[tx_tail].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;

    // 3. Update tail pointer - THIS TRIGGERS TRANSMISSION!
    tx_tail = (tx_tail + 1) % TX_RING_SIZE;
    writel(tx_tail, hw->hw_addr + E1000_TDT);

    // Hardware sees tail pointer change and starts DMA transfer
}
```

**What happens when you write to `E1000_TDT`?**

1. **CPU**: `writel(value, address)` → Memory write
2. **PCIe bus**: Routes write to network card
3. **Network card hardware**: Detects register write
4. **DMA engine**: Reads packet from RAM
5. **PHY (Physical layer)**: Transmits on Ethernet wire

### Register Access Helpers

Linux provides macros for different access types:

```c
// Memory-mapped I/O
u32 val = readl(addr);        // Read 32-bit
writeb(0x42, addr);           // Write 8-bit
writeq(0x123456, addr);       // Write 64-bit

// With memory barriers (ensures ordering)
writel(0x1, hw->regs + CTRL);
wmb();  // Write memory barrier
writel(0x2, hw->regs + STATUS);

// Port I/O (x86 legacy)
outb(0x42, 0x3F8);           // Write byte to port
u8 val = inb(0x3F8);         // Read byte from port
```

### Real Example: GPU Framebuffer

Drawing a pixel on screen:

```c
// Map GPU's framebuffer (BAR 2)
int fd = open("/dev/mem", O_RDWR);
void *fb = mmap(NULL, SCREEN_WIDTH * SCREEN_HEIGHT * 4,
                PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, FRAMEBUFFER_BASE);

uint32_t *pixels = (uint32_t *)fb;

// Draw red pixel at (100, 100)
pixels[100 * SCREEN_WIDTH + 100] = 0xFFFF0000;

// That's it! GPU scans framebuffer and displays on monitor
```

**The magic**:
- You write to memory address
- That address is actually on the GPU
- GPU's display controller reads framebuffer 60 times per second
- Monitor shows the pixel

## Level 4: DMA - Hardware Direct Memory Access

**Problem**: CPU copying data is slow and wastes CPU time.

**Solution**: Let hardware access RAM directly!

### DMA Setup

```c
// Allocate DMA buffer
dma_addr_t dma_handle;
void *buffer = dma_alloc_coherent(dev, 4096, &dma_handle, GFP_KERNEL);

// buffer = CPU virtual address (e.g., 0xFFFF8000DEADBEEF)
// dma_handle = Physical address hardware sees (e.g., 0x12345000)

// Tell hardware where to DMA to/from
writel(dma_handle & 0xFFFFFFFF, hw->regs + DMA_ADDR_LOW);
writel(dma_handle >> 32, hw->regs + DMA_ADDR_HIGH);
writel(4096, hw->regs + DMA_LENGTH);
writel(DMA_START, hw->regs + DMA_CTRL);

// Hardware now transfers data directly to/from RAM
// CPU is free to do other work!

// Wait for completion
wait_for_completion(&dma_done);
```

### DMA Process Diagram

```
┌──────────────────────────────────────────────────────┐
│                    System RAM                        │
│                                                      │
│  ┌────────────────────────────────────┐            │
│  │  DMA Buffer @ 0x12345000           │            │
│  │  [packet data...]                  │            │
│  └─────────────┬──────────────────────┘            │
└────────────────┼────────────────────────────────────┘
                 │
                 │ ← DMA transfer (no CPU involved!)
                 │
┌────────────────▼────────────────────────────────────┐
│             Network Card                             │
│                                                      │
│  ┌────────────────────────────────────┐            │
│  │  DMA Engine                        │            │
│  │  - Reads DMA_ADDR_LOW/HIGH        │            │
│  │  - Reads DMA_LENGTH               │            │
│  │  - Transfers directly from RAM    │            │
│  │  - Sends on network wire          │            │
│  └────────────────────────────────────┘            │
│                                                      │
│  [PHY] ──────────────► Network Cable                │
└──────────────────────────────────────────────────────┘
```

### Interrupt When Complete

```c
// In device driver's interrupt handler
static irqreturn_t dma_interrupt(int irq, void *dev_id) {
    u32 status = readl(hw->regs + DMA_STATUS);

    if (status & DMA_COMPLETE) {
        // DMA finished!
        complete(&dma_done);
        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}
```

Hardware sends interrupt → CPU runs handler → Driver knows DMA is done.

## The Progression: Simple to Complex

### Level 1: GPIO (Direct Control)
- **Abstraction**: None
- **CPU involvement**: Every bit
- **Speed**: Slow (microseconds per operation)
- **Use cases**: LEDs, buttons, simple sensors

### Level 2: Serial Protocols (Timed Communication)
- **Abstraction**: Bit timing, clock
- **CPU involvement**: High (bit-bang) or Medium (hardware controller)
- **Speed**: kHz to MHz
- **Use cases**: Sensors, displays, EEPROMs

### Level 3: Memory-Mapped Registers (Complex Devices)
- **Abstraction**: Register interface, command/status
- **CPU involvement**: Command initiation, status checking
- **Speed**: Very fast (PCIe Gen3: 8 GT/s)
- **Use cases**: GPUs, network cards, NVMe SSDs

### Level 4: DMA (Hardware Independence)
- **Abstraction**: Descriptor rings, scatter-gather
- **CPU involvement**: Setup only, hardware does transfer
- **Speed**: Limited by memory bandwidth
- **Use cases**: High-throughput devices (10Gb NICs, GPUs)

## The Unifying Concept: Everything is Memory-Mapped

Whether you're:
- Toggling a GPIO pin
- Sending I2C data
- Programming a PCI device
- Setting up DMA

**You're always writing to memory addresses that represent hardware**.

```c
// All of these are the same operation at CPU level:
*ram_address = 0x42;           // Regular RAM
*gpio_register = 0x42;         // GPIO pin
*i2c_register = 0x42;          // I2C controller
*pci_register = 0x42;          // PCI device
*dma_register = 0x42;          // DMA controller
```

The CPU doesn't care. The **memory controller** and **bus logic** route the write to the right place.

## Summary

| Level | Interface | Timing | CPU Load | Examples |
|-------|-----------|--------|----------|----------|
| **GPIO** | Direct voltage | Software controlled | Very High | LED, button, relay |
| **Serial** | Bit patterns | Critical timing | High | I2C sensor, SPI flash |
| **MMIO** | Register writes | Hardware handled | Medium | PCI devices, GPU |
| **DMA** | Descriptor setup | Autonomous | Very Low | Network cards, disk I/O |

**Key insight**: As we move up levels, we trade direct control for efficiency and performance. Modern systems use all four levels simultaneously.