/*
  gpio.h - GPIO line handling shared by the core's pin functions.
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.
*/
#pragma once

// Route a header pin to the kit's ADC (once; a later pinMode() takes it back). Returns the
// ADC channel, or -1 (reported once) if the pin can't be an analog input.
int edison_analog_select(unsigned pin);

// Route a header pin to its PWM output (once; a later pinMode() takes it back). Returns
// the PWM channel, or -1 if the pin has none or can't be had (reported once).
int edison_pwm_select(unsigned pin);

// Edge detection on an input pin, for attachInterrupt(): edges is 0 (off), RISING, FALLING
// or CHANGE. Returns a file descriptor to poll for edges, or -1 (reported once).
int edison_gpio_edges(unsigned pin, int edges);

// Read the edges waiting on a pin (after a poll says there are some), oldest first,
// calling cb(rising, arg) for each with no locks held.
void edison_gpio_events(unsigned pin, void (*cb)(bool rising, void *arg), void *arg);

// For pulseIn(): the length in microseconds of the next whole pulse at the given level on
// an input pin, from the kernel's edge timestamps; 0 on timeout. Returns -1 if edges can't
// be used (attachInterrupt() has the pin): the caller should poll instead.
long edison_gpio_pulse(unsigned pin, bool high, unsigned long timeout_us);

// Route IO11-IO13 to SPI5 (MOSI, MISO, SCK) for the SPI library, or (on = false) back to
// inputs. Returns false (reported) if a pin can't be had.
bool edison_spi_pins_enable(bool on);

// SPI5 also carries the kit's ADC (CS0). Between the SPI library's beginTransaction() and
// endTransaction() a device may be selected by its GPIO chip select, and an ADC conversion
// on the bus would reach it as data: analogRead() waits until the transaction ends. Also
// keeps two threads' transactions apart. (In wiring_analog.cpp.)
void edison_spi_transaction_begin();
void edison_spi_transaction_end();

// In wiring_pwm.cpp: switch a PWM channel's SoC pin to its PWM function (edison-image's
// edison-pinmux service), and stop a channel when its pin goes back to GPIO use.
bool edison_pwm_pinmux(int channel);
void edison_pwm_stop(int channel);

// Return the sketch's pins to inputs and release every line it holds, so the next sketch
// can take them. Called once, when the sketch is stopped; the process must exit after it,
// as pin functions called from then on block.
void edison_gpio_release_all();
