/*
  wiring_pulse.cpp - pulseIn(), timed from the kernel's GPIO edge timestamps.
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.

  As on other boards: if the pin is already at the level, the pulse in progress is skipped
  and the next whole one measured; the timeout (microseconds) covers the whole wait, and
  0 means none came in time.
*/
#include "Arduino.h"
#include "gpio.h"

namespace {

// By reading the pin, for a pin whose edges attachInterrupt() already takes. Less exact:
// the result is off by however long this thread wasn't running.
unsigned long poll_pulse(pin_size_t pin, PinStatus level, unsigned long timeout) {
    const unsigned long begin = micros();
    auto expired = [&] { return micros() - begin >= timeout; };
    while (digitalRead(pin) == level) {
        if (expired()) return 0;
    }
    while (digitalRead(pin) != level) {
        if (expired()) return 0;
    }
    const unsigned long start = micros();
    while (digitalRead(pin) == level) {
        if (expired()) return 0;
    }
    return micros() - start;
}

}  // namespace

unsigned long pulseIn(pin_size_t pin, uint8_t state, unsigned long timeout) {
    long us = edison_gpio_pulse(pin, state != LOW, timeout);
    if (us >= 0) return (unsigned long)us;
    return poll_pulse(pin, state != LOW ? HIGH : LOW, timeout);
}

// The same here: pulseIn() doesn't depend on interrupts being enabled, as on AVR.
unsigned long pulseInLong(pin_size_t pin, uint8_t state, unsigned long timeout) {
    return pulseIn(pin, state, timeout);
}
