
; AVR Bare-Metal LED Blink - Assembly Version
;
; Target: ATmega328P @ 16MHz
; LED on PB5 (Arduino Uno pin 13)
;
; Assemble:
;   avr-as -mmcu=atmega328p avr_blink.asm -o avr_blink_asm.o
;   avr-ld -m avr5 avr_blink_asm.o -o avr_blink_asm.elf
;   avr-objcopy -O ihex avr_blink_asm.elf avr_blink_asm.hex
;
; Or use avr-gcc:
;   avr-gcc -mmcu=atmega328p -nostartfiles avr_blink.asm -o avr_blink_asm.elf
;   avr-objcopy -O ihex avr_blink_asm.elf avr_blink_asm.hex
;
; Flash:
;   avrdude -c usbasp -p m328p -U flash:w:avr_blink_asm.hex:i
;

.include "/usr/lib/avr/include/avr/iom328pdef.inc"

; Register aliases
.def temp = r16
.def outer = r18
.def mid = r19
.def inner = r20

; Reset vector
.org 0x0000
    rjmp main

; Skip interrupt vectors (26 vectors × 2 bytes = 52 bytes)
.org 0x0034

main:
    ; Initialize stack pointer
    ; RAMEND = 0x08FF for ATmega328P (2KB SRAM)
    ldi temp, HIGH(RAMEND)
    out SPH, temp
    ldi temp, LOW(RAMEND)
    out SPL, temp

    ; Configure PB5 as output
    ; DDRB = 0x04 in I/O space (0x24 in data space)
    ldi temp, (1 << PB5)    ; temp = 0b00100000
    out DDRB, temp          ; Write to DDRB

loop:
    ; Turn LED ON
    ; PORTB = 0x05 in I/O space (0x25 in data space)
    sbi PORTB, PB5          ; Set bit 5 in PORTB

    ; Delay approximately 500ms
    ldi outer, 41           ; Outer loop: 41 iterations
delay_on:
    ldi mid, 250            ; Middle loop: 250 iterations
delay_mid_on:
    ldi inner, 250          ; Inner loop: 250 iterations
delay_inner_on:
    dec inner               ; 1 cycle
    brne delay_inner_on     ; 2 cycles if branch taken, 1 if not
    dec mid                 ; 1 cycle
    brne delay_mid_on       ; 2 cycles
    dec outer               ; 1 cycle
    brne delay_on           ; 2 cycles

    ; Turn LED OFF
    cbi PORTB, PB5          ; Clear bit 5 in PORTB

    ; Delay approximately 500ms
    ldi outer, 41           ; Outer loop
delay_off:
    ldi mid, 250            ; Middle loop
delay_mid_off:
    ldi inner, 250          ; Inner loop
delay_inner_off:
    dec inner               ; 1 cycle
    brne delay_inner_off    ; 2 cycles
    dec mid                 ; 1 cycle
    brne delay_mid_off      ; 2 cycles
    dec outer               ; 1 cycle
    brne delay_off          ; 2 cycles

    ; Repeat forever
    rjmp loop

; Notes on timing:
; Inner loop: 250 * (1 + 2) - 1 = 749 cycles
; Middle loop: 250 * (749 + 1 + 2) - 1 = 188,249 cycles
; Outer loop: 41 * (188,249 + 1 + 2) - 1 = 7,718,311 cycles
; Time: 7,718,311 / 16,000,000 Hz ≈ 0.482 seconds