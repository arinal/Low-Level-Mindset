/*
 * AVR Bare-Metal LED Blink
 *
 * Target: ATmega328P @ 16MHz
 * LED on PB5 (Arduino Uno pin 13)
 *
 * Compile:
 *   avr-gcc -mmcu=atmega328p -DF_CPU=16000000UL -Os avr_blink.c -o avr_blink.elf
 *   avr-objcopy -O ihex -R .eeprom avr_blink.elf avr_blink.hex
 *
 * Flash:
 *   avrdude -c usbasp -p m328p -U flash:w:avr_blink.hex:i
 *
 * Circuit:
 *   PB5 (Pin 19) ──[330Ω]── LED ── GND
 */

#include <avr/io.h>
#include <util/delay.h>

int main(void)
{
    // Configure PB5 as output
    // DDRB: Data Direction Register for Port B
    // Setting bit 5 = 1 makes PB5 an output
    DDRB |= (1 << PB5);

    // Infinite loop - microcontrollers never exit!
    while (1) {
        // Turn LED ON
        // PORTB: Port B Data Register
        // Setting bit 5 = 1 outputs HIGH (5V) on PB5
        PORTB |= (1 << PB5);

        // Wait 500 milliseconds
        _delay_ms(500);

        // Turn LED OFF
        // Clearing bit 5 = 0 outputs LOW (0V) on PB5
        PORTB &= ~(1 << PB5);

        // Wait 500 milliseconds
        _delay_ms(500);
    }

    return 0;  // Never reached
}