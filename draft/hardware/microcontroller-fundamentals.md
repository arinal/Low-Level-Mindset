# Microcontroller Fundamentals: Bare-Metal Programming Without an OS

This document explains how microcontrollers run directly on hardware without an operating system, using AVR as a concrete example.

## Table of Contents
- [Microcontroller vs Microprocessor](#microcontroller-vs-microprocessor)
- [AVR Architecture Overview](#avr-architecture-overview)
- [Complete Hardware Setup](#complete-hardware-setup)
- [Programming Model: Registers](#programming-model-registers)
- [Bare-Metal LED Blink: Assembly](#bare-metal-led-blink-assembly)
- [Bare-Metal LED Blink: C](#bare-metal-led-blink-c)
- [Compiling and Building](#compiling-and-building)
- [Programming via SPI (ISP)](#programming-via-spi-isp)
- [What Happens at Power-On](#what-happens-at-power-on)
- [No OS: What Does This Mean?](#no-os-what-does-this-mean)

## Microcontroller vs Microprocessor

### Microprocessor (CPU)
```
┌─────────────────┐
│   Intel Core    │
│                 │
│  - CPU cores    │
│  - Cache        │
│  - NO RAM       │ ──┬──► External RAM (DDR4)
│  - NO Storage   │   ├──► External ROM (BIOS Flash)
│  - NO GPIO      │   ├──► External GPU
│  - NO Timers    │   └──► External Peripherals
└─────────────────┘
     Needs OS
```

**Requires**:
- External RAM
- External storage
- Operating system
- Complex initialization

### Microcontroller (MCU)
```
┌───────────────────────────────────┐
│        ATmega328P (AVR)           │
│                                   │
│  ┌──────────┐  ┌──────────┐       │
│  │   CPU    │  │   Flash  │       │
│  │  8-bit   │  │  32KB    │       │
│  │  20MHz   │  │  (ROM)   │       │
│  └──────────┘  └──────────┘       │
│                                   │
│  ┌──────────┐  ┌──────────┐       │
│  │   RAM    │  │  EEPROM  │       │
│  │   2KB    │  │   1KB    │       │
│  └──────────┘  └──────────┘       │
│                                   │
│  ┌─────────────────────────────┐  │
│  │  GPIO Ports (PORTB, PORTD)  │  │──► LED, Sensors
│  ├─────────────────────────────┤  │
│  │  Timers (Timer0, Timer1)    │  │──► PWM, Delays
│  ├─────────────────────────────┤  │
│  │  ADC (10-bit)               │  │──► Analog Sensors
│  ├─────────────────────────────┤  │
│  │  UART, SPI, I2C             │  │──► Communication
│  └─────────────────────────────┘  │
└───────────────────────────────────┘
        No OS needed!
```

**Self-contained**:
- CPU + RAM + Flash all in one chip
- Built-in peripherals (GPIO, timers, ADC)
- Runs code directly from Flash
- No OS required

## AVR Architecture Overview

### ATmega328P Block Diagram

```
                        ┌──────────────────────────────────────┐
                        │         ATmega328P (28-pin DIP)      │
                        │                                      │
    ┌───────────────────┤  ┌──────────────────────────────┐    │
    │ VCC (5V)          │  │  8-bit AVR CPU Core          │    │
    │ GND               │  │  - 32 General Purpose Regs   │    │
    │ AVCC (Analog)     │  │  - ALU                       │    │
    │ AREF              │  │  - Program Counter (PC)      │    │
    │                   │  │  - Stack Pointer (SP)        │    │
    │                   │  └──────────┬───────────────────┘    │
    │                   │             │                        │
    │  ┌────────────────┤             ▼                        │
    │  │ XTAL1, XTAL2   │  ┌──────────────────────────────┐    │
    │  │ (16MHz Crystal)│  │  32KB Flash Memory           │    │
    │  │                │  │  (Program storage)           │    │
    │  │                │  │  Address: 0x0000 - 0x7FFF    │    │
    │  │                │  └──────────────────────────────┘    │
    │  │                │                                      │
    │  │                │  ┌──────────────────────────────┐    │
    │  │                │  │  2KB SRAM                    │    │
    │  │                │  │  Address: 0x0100 - 0x08FF    │    │
    │  │                │  └──────────────────────────────┘    │
    │  │                │                                      │
    │  │                │  ┌──────────────────────────────┐    │
    │  │ PORTB          │  │  I/O Registers               │    │──► PB0-PB5
    │  │ (PB0-PB7)      │  │  DDRB, PORTB, PINB           │    │
    │  │                │  │  Address: 0x23-0x25          │    │
    │  │                │  └──────────────────────────────┘    │
    │  │                │                                      │
    │  │ PORTC          │  ┌──────────────────────────────┐    │──► PC0-PC5
    │  │ (PC0-PC6)      │  │  DDRC, PORTC, PINC           │    │
    │  │                │  │  Address: 0x26-0x28          │    │
    │  │                │  └──────────────────────────────┘    │
    │  │                │                                      │
    │  │ PORTD          │  ┌──────────────────────────────┐    │──► PD0-PD7
    │  │ (PD0-PD7)      │  │  DDRD, PORTD, PIND           │    │
    │  │                │  │  Address: 0x29-0x2B          │    │
    │  │                │  └──────────────────────────────┘    │
    │  │                │                                      │
    │  │ RESET          │  ┌──────────────────────────────┐    │
    │  │                │  │  Timer0, Timer1, Timer2      │    │
    │  │                │  │  ADC, UART, SPI, I2C         │    │
    │  │                │  └──────────────────────────────┘    │
    └──┴────────────────┴──────────────────────────────────────┘
```

### Memory Map

```
Flash (Program Memory):
0x0000 ┌──────────────────┐
       │ Reset Vector     │  Jump to main program
0x0002 ├──────────────────┤
       │ Interrupt Vectors│  ISR addresses
0x0034 ├──────────────────┤
       │                  │
       │ Your Program     │  Executable code
       │                  │
0x7FFF └──────────────────┘

SRAM (Data Memory):
0x0000 ┌──────────────────┐
       │ 32 CPU Registers │  r0 - r31
0x0020 ├──────────────────┤
       │ 64 I/O Registers │  GPIO, Timer, etc.
0x0060 ├──────────────────┤
       │ 160 Ext I/O Regs │  Extended peripherals
0x0100 ├──────────────────┤
       │                  │
       │ Internal SRAM    │  Variables, stack
       │ (2048 bytes)     │
       │                  │
0x08FF └──────────────────┘ ← Stack grows down from here
```

## Complete Hardware Setup

### Minimal Circuit Schematic

```
                            ATmega328P
                        ┌────────────────┐
    +5V ────────────────┤1  PC6 (Reset)  │
                        │                │
    ┌──[10kΩ]──+5V      │                │
    │                   │                │
    └───[Button]─GND────┤1  PC6 (Reset)  │
                        │                │
                        │                │
    ┌─[16MHz Crystal]─┐ │                │
    │                 │ │                │
    ├─[22pF]─GND      │ │                │
    │                 │ │                │
    └─────────────────┼─┤9  XTAL1        │
                      │ │                │
    ┌─[22pF]─GND      │ │                │
    │                 │ │                │
    └─────────────────┼─┤10 XTAL2        │
                      │ │                │
    +5V ──────────────┤7  VCC            │
    +5V ──────────────┤20 AVCC           │
    GND ──────────────┤8  GND            │
    GND ──────────────┤22 GND            │
                      │ │                │
    +5V ─[10µF]─GND   │ │ (Decoupling)   │
    VCC ─[100nF]─GND  │ │                │
                      │ │                │
                      │ │                │
                      │ │14 PB0          ├──► ISP: MOSI
                      │ │15 PB1          ├──► ISP: MISO
                      │ │16 PB2          ├──► ISP: SCK
                      │ │17 PB3          ├──►
                      │ │18 PB4          ├──►
                      │ │                │
                      │ │19 PB5          ├────[330Ω]────┐
                      │ │                │              │
                      │ │                │           ┌──▼──┐
                      │ └────────────────┘           │ LED │  Red LED
                      │                              │ (↓) │
                      │                              └──┬──┘
                      │                                 │
                      └─────────────────────────────────┴─── GND

ISP Programming Header:
                    ┌─────────────┐
        MISO ───────┤1   2├─────── VCC
         SCK ───────┤3   4├─────── MOSI
       RESET ───────┤5   6├─────── GND
                    └─────────────┘
                    (6-pin header)
```

### Component Explanation

#### 1. Power Supply
```
+5V ─┬─ VCC (Pin 7)    Main power
     ├─ AVCC (Pin 20)  Analog power (cleaner for ADC)
     └─[Decoupling capacitors]
         - 10µF: Bulk capacitance
         - 100nF: High-frequency noise filtering
```

**Why decoupling caps?**
- Digital switching creates voltage spikes
- Capacitors smooth out power supply
- Prevents resets and erratic behavior

#### 2. Clock Source (Crystal Oscillator)
```
     16MHz Crystal
         │
    ┌────┴────┐
XTAL1         XTAL2
    │         │
  [22pF]    [22pF]
    │         │
   GND       GND
```

**Why crystal?**
- More accurate than internal RC oscillator (±1% vs ±10%)
- Required for precise timing (UART, SPI)
- Provides 16MHz clock → Instructions execute at 16 million per second

**How it works**:
- Crystal vibrates at 16MHz when voltage applied
- Load capacitors (22pF) set oscillation frequency
- AVR's internal oscillator circuit amplifies and uses this

#### 3. Reset Circuit
```
+5V ─[10kΩ]─┬─ RESET (Pin 1)
             │
        [Button]
             │
            GND
```

**Why pull-up resistor?**
- RESET is active-low (0V = reset, 5V = run)
- 10kΩ keeps it HIGH normally
- Button press pulls LOW → resets chip

#### 4. LED Circuit
```
PB5 (Pin 19) ─[330Ω]─┬─ LED Anode (+)
                     │
                  LED Cathode (-)
                     │
                    GND
```

**Why 330Ω resistor?**
- LED forward voltage: ~2V
- MCU output: 5V
- Voltage across resistor: 5V - 2V = 3V
- Current: 3V / 330Ω ≈ 9mA (safe for LED and MCU)

**Without resistor**: LED draws too much current → burns out or damages MCU pin!

#### 5. ISP Programming Header
```
   ┌─────┬─────┐
   │ 1   │ 2   │  MISO (Master In, Slave Out)
   ├─────┼─────┤  VCC  (Power)
   │ 3   │ 4   │  SCK  (Serial Clock)
   ├─────┼─────┤  MOSI (Master Out, Slave In)
   │ 5   │ 6   │  RESET
   └─────┴─────┘  GND
```

Used to program the chip via SPI (explained later).

## Programming Model: Registers

### GPIO Control Registers

Each port (PORTB, PORTC, PORTD) has three registers:

```c
// Example for PORTB (controls pins PB0-PB7)

DDRx  - Data Direction Register
       0 = Input, 1 = Output
       Address: 0x24 (DDRB)

PORTx - Port Output Register
       When pin is OUTPUT: 0 = LOW (0V), 1 = HIGH (5V)
       When pin is INPUT: 0 = Hi-Z, 1 = Pull-up enabled
       Address: 0x25 (PORTB)

PINx  - Port Input Register
       Read current state of pins
       Address: 0x23 (PINB)
```

### Example: Configure PB5 as Output

```
Initial state: DDRB = 0b00000000  (all inputs)

Set bit 5:     DDRB |= (1 << 5)
Result:        DDRB = 0b00100000  (PB5 is output)

Turn LED ON:   PORTB |= (1 << 5)
Result:        PORTB = 0b00100000 (PB5 = HIGH = 5V)

Turn LED OFF:  PORTB &= ~(1 << 5)
Result:        PORTB = 0b00000000 (PB5 = LOW = 0V)
```

## Bare-Metal LED Blink: Assembly

### Complete Assembly Program

```asm
; LED blink on PB5 (Arduino pin 13)
; Chip: ATmega328P
; Clock: 16MHz

.include "m328Pdef.inc"  ; Register definitions

.org 0x0000              ; Reset vector at address 0
    rjmp main            ; Jump to main program

.org 0x0034              ; After interrupt vectors
main:
    ; Set up stack pointer (required for function calls)
    ldi r16, HIGH(RAMEND)  ; Load high byte of RAM end
    out SPH, r16           ; Set stack pointer high
    ldi r16, LOW(RAMEND)   ; Load low byte of RAM end
    out SPL, r16           ; Set stack pointer low

    ; Configure PB5 as output
    ldi r16, (1 << PB5)    ; Load value 0b00100000
    out DDRB, r16          ; Write to Data Direction Register B

loop:
    ; Turn LED ON
    sbi PORTB, PB5         ; Set Bit in I/O register (PB5 = HIGH)

    ; Delay ~500ms
    ldi r18, 41            ; Outer loop counter
delay_on:
    ldi r19, 250           ; Middle loop counter
delay_mid1:
    ldi r20, 250           ; Inner loop counter
delay_inner1:
    dec r20                ; Decrement inner counter
    brne delay_inner1      ; Branch if not equal to zero
    dec r19                ; Decrement middle counter
    brne delay_mid1        ; Branch if not equal to zero
    dec r18                ; Decrement outer counter
    brne delay_on          ; Branch if not equal to zero

    ; Turn LED OFF
    cbi PORTB, PB5         ; Clear Bit in I/O register (PB5 = LOW)

    ; Delay ~500ms
    ldi r18, 41            ; Outer loop counter
delay_off:
    ldi r19, 250           ; Middle loop counter
delay_mid2:
    ldi r20, 250           ; Inner loop counter
delay_inner2:
    dec r20                ; Decrement inner counter
    brne delay_inner2      ; Branch if not equal to zero
    dec r19                ; Decrement middle counter
    brne delay_mid2        ; Branch if not equal to zero
    dec r18                ; Decrement outer counter
    brne delay_off         ; Branch if not equal to zero

    rjmp loop              ; Jump back to loop (infinite loop)
```

### Instruction Breakdown

```asm
ldi r16, (1 << PB5)   ; Load Immediate: r16 = 0b00100000
                      ; Machine code: 1110 0000 0001 0000
                      ; Takes 1 clock cycle

out DDRB, r16         ; Output to I/O register
                      ; Writes r16 to DDRB (address 0x04 in I/O space)
                      ; Takes 1 clock cycle

sbi PORTB, PB5        ; Set Bit in I/O register
                      ; Reads PORTB, sets bit 5, writes back
                      ; Takes 2 clock cycles
                      ; Physical effect: Pin goes to 5V!

cbi PORTB, PB5        ; Clear Bit in I/O register
                      ; Reads PORTB, clears bit 5, writes back
                      ; Takes 2 clock cycles
                      ; Physical effect: Pin goes to 0V!
```

### Delay Calculation

At 16MHz, each instruction takes 62.5 nanoseconds:

```
Delay loop:
    ldi r20, 250       ; 1 cycle
inner:
    dec r20            ; 1 cycle × 250 = 250 cycles
    brne inner         ; 2 cycles × 250 = 500 cycles

Total per inner loop: 1 + 250 + 500 = 751 cycles

Three nested loops:
    751 × 250 × 41 ≈ 7,690,250 cycles

Time: 7,690,250 / 16,000,000 = 0.481 seconds ≈ 500ms
```

## Bare-Metal LED Blink: C

### Complete C Program

```c
/*
 * LED Blink - Bare Metal AVR
 * Target: ATmega328P @ 16MHz
 * LED on PB5 (Arduino pin 13)
 */

#include <avr/io.h>       // Register definitions
#include <util/delay.h>   // Delay functions

int main(void)
{
    // Configure PB5 as output
    DDRB |= (1 << PB5);   // Set bit 5 of DDRB to 1

    // Infinite loop
    while (1) {
        // Turn LED ON
        PORTB |= (1 << PB5);   // Set bit 5 of PORTB to 1
        _delay_ms(500);         // Wait 500 milliseconds

        // Turn LED OFF
        PORTB &= ~(1 << PB5);  // Clear bit 5 of PORTB to 0
        _delay_ms(500);         // Wait 500 milliseconds
    }

    return 0;  // Never reached
}
```

### What the Code Expands To

```c
// Register definitions (from <avr/io.h>)
#define DDRB  _SFR_IO8(0x04)   // DDR at I/O address 0x04
#define PORTB _SFR_IO8(0x05)   // PORT at I/O address 0x05
#define PB5   5                 // Bit position

// _SFR_IO8 expands to:
#define _SFR_IO8(addr) (*((volatile uint8_t *)(addr + 0x20)))

// So DDRB is actually:
// *(volatile uint8_t *)(0x04 + 0x20) = *(volatile uint8_t *)0x24

// Operations:
DDRB |= (1 << PB5);
// Expands to:
*(volatile uint8_t *)0x24 |= (1 << 5);
// Compiles to:
sbi 0x04, 5    ; Set bit 5 in I/O register 0x04
```

### Advanced: Reading Input

```c
#include <avr/io.h>

int main(void)
{
    // PB5 = Output (LED)
    DDRB |= (1 << PB5);

    // PD2 = Input (Button)
    DDRD &= ~(1 << PD2);   // Clear bit = input
    PORTD |= (1 << PD2);   // Enable pull-up resistor

    while (1) {
        // Read button state
        if (PIND & (1 << PD2)) {
            // Button NOT pressed (pulled HIGH)
            PORTB &= ~(1 << PB5);  // LED OFF
        } else {
            // Button pressed (pulled LOW)
            PORTB |= (1 << PB5);   // LED ON
        }
    }

    return 0;
}
```

### Using Timers (PWM Example)

```c
#include <avr/io.h>

// Fade LED using PWM
int main(void)
{
    // PB5 = Output
    DDRB |= (1 << PB5);

    // Configure Timer1 for PWM
    // Fast PWM, 8-bit, non-inverting mode
    TCCR1A = (1 << COM1A1) | (1 << WGM10);
    TCCR1B = (1 << WGM12) | (1 << CS11);  // Prescaler = 8

    uint8_t brightness = 0;
    uint8_t direction = 1;

    while (1) {
        OCR1A = brightness;  // Set PWM duty cycle

        // Fade in/out
        brightness += direction;
        if (brightness == 255 || brightness == 0)
            direction = -direction;

        // Small delay
        for (volatile uint32_t i = 0; i < 10000; i++);
    }

    return 0;
}
```

## Compiling and Building

### Toolchain Setup

```bash
# Install AVR toolchain (on Ubuntu/Debian)
sudo apt-get install gcc-avr avr-libc avrdude

# On Arch Linux
sudo pacman -S avr-gcc avr-libc avrdude

# On macOS
brew install avr-gcc avrdude
```

### Compilation Process

#### Step 1: Compile C to Assembly

```bash
avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os \
        -S blink.c -o blink.s
```

**Output `blink.s`**:
```asm
main:
    push r28
    push r29
    ldi r24,lo8(5)
    out 0x4,r24      ; DDRB = 0x05
.L2:
    ldi r24,lo8(5)
    out 0x5,r24      ; PORTB = 0x05
    ldi r24,lo8(250)
    ldi r25,lo8(1000)
    call __delay_ms
    ; ... more code
```

#### Step 2: Compile to Object File

```bash
avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os \
        -c blink.c -o blink.o
```

Generates ELF object file with machine code.

#### Step 3: Link to Create ELF

```bash
avr-gcc -mmcu=atmega328p blink.o -o blink.elf
```

Links with AVR libc, sets up interrupt vectors, etc.

#### Step 4: Generate Intel HEX File

```bash
avr-objcopy -O ihex -R .eeprom blink.elf blink.hex
```

**Intel HEX format** (text representation of binary):
```
:100000000C9434000C943E000C943E000C943E0082
:100010000C943E000C943E000C943E000C943E0068
:100020000C943E000C943E000C943E000C943E0058
:10003000559577511199DD00D500116655FF884466
:020040001DC02E
:00000001FF
```

Each line:
- `:10` - 16 bytes of data
- `0000` - Address
- `00` - Data record type
- `0C9434...` - Actual program bytes
- `82` - Checksum

#### Complete Makefile

```makefile
# Makefile for AVR ATmega328P

MCU = atmega328p
F_CPU = 16000000UL
BAUD = 9600

CC = avr-gcc
OBJCOPY = avr-objcopy
SIZE = avr-size
AVRDUDE = avrdude

CFLAGS = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -DBAUD=$(BAUD)
CFLAGS += -Os -Wall -Wextra -std=gnu99

TARGET = blink
SRC = blink.c

all: $(TARGET).hex size

$(TARGET).elf: $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET).elf

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $(TARGET).elf $(TARGET).hex

size: $(TARGET).elf
	$(SIZE) --format=avr --mcu=$(MCU) $(TARGET).elf

flash: $(TARGET).hex
	$(AVRDUDE) -c usbasp -p $(MCU) -U flash:w:$(TARGET).hex:i

clean:
	rm -f $(TARGET).elf $(TARGET).hex

.PHONY: all clean flash size
```

Usage:
```bash
make          # Compile
make flash    # Program chip
make clean    # Clean up
```

## Programming via SPI (ISP)

### ISP: In-System Programming

AVR chips have built-in SPI bootloader for programming.

### Hardware Setup

```
        PC/Laptop
            │
            │ USB
            ▼
    ┌───────────────┐
    │  USBasp       │  Or Arduino as ISP
    │  Programmer   │
    └───────┬───────┘
            │
            │ 6-wire ISP cable
            ▼
    ┌───────────────┐
    │  ATmega328P   │
    │               │
    │  MOSI ← Pin 17│
    │  MISO → Pin 18│
    │  SCK  ← Pin 19│
    │  RESET← Pin 1 │
    │  VCC  ← Pin 7 │
    │  GND  ← Pin 8 │
    └───────────────┘
```

### SPI Programming Protocol

#### 1. Enter Programming Mode

```
Programmer                        ATmega328P
    │                                  │
    ├─ Pull RESET LOW ─────────────►  │ (Enter programming mode)
    │                                  │
    ├─ Wait 100µs ─────────────────►  │
    │                                  │
    ├─ Send: 0xAC 0x53 0x00 0x00 ─►  │ (Enable programming)
    │                                  │
    │◄─ Receive: 0x00 0x53 0x?? 0x?? ─┤ (Acknowledge)
    │                                  │
```

**SPI timing**:
```
    RESET: ──────┐
                  └────────────────────────────

    SCK:   ───┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─┐ ┌─
              └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘ └─┘

    MOSI:  ─ 1 0 1 0 1 1 0 0  (0xAC)
              A   C

    MISO:  ─ 0 1 0 1 0 0 1 1  (0x53 echo)
              5   3
```

#### 2. Write Flash Memory

```
For each byte at address ADDR:
    Send: 0x40, ADDR_HIGH, ADDR_LOW, DATA
    Wait: 4.5ms (write time)

For each page (64 or 128 bytes):
    Load page buffer with bytes
    Send: 0x4C, PAGE_ADDR_HIGH, PAGE_ADDR_LOW, 0x00
    Wait: 4.5ms (page write)
```

#### 3. Verify Flash

```
For each byte at address ADDR:
    Send: 0x20, ADDR_HIGH, ADDR_LOW, 0x00
    Receive: 0x??, 0x??, 0x??, DATA
```

#### 4. Exit Programming Mode

```
Pull RESET HIGH → Chip runs program
```

### Using avrdude

```bash
# Check connection
avrdude -c usbasp -p m328p

# Program flash
avrdude -c usbasp -p m328p -U flash:w:blink.hex:i

# Read flash (backup)
avrdude -c usbasp -p m328p -U flash:r:backup.hex:i

# Set fuses (clock settings, etc.)
avrdude -c usbasp -p m328p -U lfuse:w:0xFF:m -U hfuse:w:0xDE:m -U efuse:w:0xFD:m

# Verbose output (for debugging)
avrdude -v -c usbasp -p m328p -U flash:w:blink.hex:i
```

**Common programmers**:
- `usbasp` - USBasp programmer (~$3-5 on eBay)
- `arduino` - Arduino as ISP
- `avrisp2` - Official Atmel programmer

### Arduino as ISP

Upload ArduinoISP sketch to Arduino, then:

```bash
avrdude -c arduino -p m328p -P /dev/ttyUSB0 -b 19200 -U flash:w:blink.hex:i
```

## What Happens at Power-On

### Boot Sequence

```
1. Power Applied (VCC reaches 5V)
   ├─ Brown-out detector waits for stable voltage
   └─ Internal circuitry initializes

2. Clock Starts
   ├─ Crystal oscillator begins (if selected)
   └─ Takes ~16,000 cycles to stabilize

3. Reset Vector Executed
   ├─ Program Counter (PC) set to 0x0000
   └─ Jump to reset handler

4. Hardware Initialization (automatic)
   ├─ All GPIO pins = INPUT, no pull-up
   ├─ All peripherals disabled
   └─ Stack pointer = undefined (you must set it!)

5. User Code Begins
   ├─ main() function called (C)
   │  or first instruction (assembly)
   └─ Your code takes control

6. Infinite Loop
   ├─ main() should never return
   └─ Usually ends with infinite loop: while(1){}
```

### Reset Vector in Detail

```asm
; Flash address 0x0000 (reset vector)
.org 0x0000
    rjmp reset_handler   ; Jump to actual code

; Other interrupt vectors at 0x0002, 0x0004, etc.
.org 0x0002
    reti                 ; Return from interrupt (unused)

; Actual program starts here
.org 0x0034
reset_handler:
    ; Set up stack
    ldi r16, HIGH(RAMEND)
    out SPH, r16
    ldi r16, LOW(RAMEND)
    out SPL, r16

    ; Initialize .data section (copy from Flash to RAM)
    ; Initialize .bss section (zero out variables)
    ; (done automatically by avr-libc startup code in C)

    ; Call main
    call main

    ; If main returns (shouldn't happen):
infinite:
    rjmp infinite
```

### What C Runtime Does

When you use `avr-gcc`, it links in startup code:

```asm
; avr-libc startup code (crt1.o)
__vectors:
    rjmp __init          ; Reset vector
    ; ... interrupt vectors

__init:
    clr r1               ; r1 always zero (convention)

    ; Set up stack
    ldi r28, lo8(__stack)
    ldi r29, hi8(__stack)
    out SPH, r29
    out SPL, r28

    ; Copy initialized data from Flash to RAM
__do_copy_data:
    ldi r17, hi8(__data_end)
    ldi r26, lo8(__data_start)
    ; ... copy loop

    ; Zero out .bss section
__do_clear_bss:
    ldi r17, hi8(__bss_end)
    ldi r26, lo8(__bss_start)
    ; ... clear loop

    ; Call main
    call main

    ; Hang if main returns
__stop_program:
    rjmp __stop_program
```

## No OS: What Does This Mean?

### What You DON'T Have

❌ **No memory protection**
- Can access any address
- Pointer bugs crash immediately
- No segmentation faults

❌ **No process scheduler**
- Only one program runs (yours)
- No multitasking (unless you implement it)

❌ **No file system**
- No `fopen()`, `read()`, `write()`
- Direct hardware access only

❌ **No standard input/output**
- `printf()` needs custom implementation
- Must set up UART manually

❌ **No dynamic memory allocation**
- `malloc()` not recommended (limited RAM)
- Usually allocate everything statically

❌ **No system calls**
- No `sleep()`, `fork()`, `exec()`
- Implement everything yourself

### What You DO Have

✓ **Direct hardware control**
- Every register accessible
- Precise timing possible
- Fast interrupt response (<1µs)

✓ **Predictable execution**
- Know exactly what code runs when
- Deterministic timing
- Real-time operation

✓ **Full chip resources**
- All 2KB RAM available
- All CPU power for your code

✓ **Bare-metal libraries**
- AVR libc provides: `<avr/io.h>`, `<avr/interrupt.h>`, `<util/delay.h>`
- Lightweight, optimized for AVR

### Example: No Scheduler

```c
// This runs FOREVER. Nothing else can run!
int main(void) {
    DDRB |= (1 << PB5);

    while (1) {
        PORTB ^= (1 << PB5);  // Toggle LED
        _delay_ms(500);

        // Nothing else happens!
        // No background tasks, no interrupts (unless you enable them)
    }
}
```

### Example: Implementing "Multitasking"

```c
#include <avr/io.h>
#include <avr/interrupt.h>

volatile uint32_t millis_count = 0;

// Timer interrupt every 1ms
ISR(TIMER0_COMPA_vect) {
    millis_count++;
}

void setup_timer() {
    // CTC mode, prescaler 64
    TCCR0A = (1 << WGM01);
    TCCR0B = (1 << CS01) | (1 << CS00);
    OCR0A = 249;  // 16MHz / 64 / 250 = 1000Hz (1ms)
    TIMSK0 = (1 << OCIE0A);  // Enable compare interrupt
    sei();  // Enable global interrupts
}

uint32_t millis() {
    uint32_t m;
    cli();  // Disable interrupts
    m = millis_count;
    sei();  // Re-enable interrupts
    return m;
}

// Simple cooperative "multitasking"
int main(void) {
    setup_timer();

    DDRB |= (1 << PB5) | (1 << PB4);

    uint32_t last_blink1 = 0;
    uint32_t last_blink2 = 0;

    while (1) {
        // "Task 1": Blink LED1 every 500ms
        if (millis() - last_blink1 >= 500) {
            PORTB ^= (1 << PB5);
            last_blink1 = millis();
        }

        // "Task 2": Blink LED2 every 200ms
        if (millis() - last_blink2 >= 200) {
            PORTB ^= (1 << PB4);
            last_blink2 = millis();
        }

        // More "tasks" can be added here
    }
}
```

### Example: printf() over UART

```c
#include <avr/io.h>
#include <stdio.h>

// Initialize UART
void uart_init(void) {
    uint16_t baud = (F_CPU / (16UL * 9600)) - 1;
    UBRR0H = (baud >> 8);
    UBRR0L = baud;
    UCSR0B = (1 << TXEN0);  // Enable transmitter
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);  // 8-bit data
}

// Send one character
int uart_putchar(char c, FILE *stream) {
    if (c == '\n')
        uart_putchar('\r', stream);

    while (!(UCSR0A & (1 << UDRE0)));  // Wait for empty buffer
    UDR0 = c;  // Send character
    return 0;
}

// Setup stdout to use UART
FILE uart_output = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);

int main(void) {
    uart_init();
    stdout = &uart_output;  // Redirect stdout to UART

    uint16_t count = 0;

    while (1) {
        printf("Count: %u\r\n", count++);
        _delay_ms(1000);
    }
}
```

## Summary

### Key Concepts

1. **Microcontrollers are self-contained**
   - CPU + RAM + Flash + peripherals in one chip
   - No external memory needed

2. **Memory-mapped I/O**
   - Write to address → Control hardware
   - Same concept as in full CPUs, but simpler

3. **Direct execution from Flash**
   - No bootloader (well, built-in ISP bootloader)
   - No loading OS
   - Your code runs immediately at power-on

4. **Register-level programming**
   - Control every bit of hardware
   - DDRB, PORTB, etc. are actual hardware registers

5. **No operating system**
   - No abstractions
   - No overhead
   - Complete control and responsibility

### The Mental Model

```
PC with Linux:
    Power → BIOS → Bootloader → Kernel → Init → Your Program

    Your program runs in userspace, OS handles hardware

AVR Microcontroller:
    Power → Your Program

    Your program IS the system, you handle hardware directly
```

### Progression to Larger Systems

```
Simple → Complex

AVR → ARM Cortex-M → ARM Cortex-A → x86
│        │              │            │
│        │              │            └─ Linux, Windows
│        │              └────────────── Linux, FreeRTOS
│        └───────────────────────────── FreeRTOS, bare-metal
└────────────────────────────────────── Almost always bare-metal

No OS   →  Simple RTOS  →  Full RTOS  →  Full OS
```

The concepts you learn on AVR (registers, interrupts, timers) apply directly to ARM and even x86 - just more complex!
