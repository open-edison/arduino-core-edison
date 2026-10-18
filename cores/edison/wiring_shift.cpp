/*
  wiring_shift.cpp - shiftOut() and shiftIn(), as on other boards (each bit is a GPIO write
  or read, a few microseconds here).
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.
*/
#include "Arduino.h"

void shiftOut(pin_size_t dataPin, pin_size_t clockPin, BitOrder bitOrder, uint8_t val) {
    for (uint8_t i = 0; i < 8; i++) {
        bool bit = bitOrder == LSBFIRST ? (val >> i) & 1 : (val >> (7 - i)) & 1;
        digitalWrite(dataPin, bit ? HIGH : LOW);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, LOW);
    }
}

uint8_t shiftIn(pin_size_t dataPin, pin_size_t clockPin, BitOrder bitOrder) {
    uint8_t value = 0;
    for (uint8_t i = 0; i < 8; i++) {
        digitalWrite(clockPin, HIGH);
        if (digitalRead(dataPin) == HIGH) {
            value |= bitOrder == LSBFIRST ? uint8_t(1 << i) : uint8_t(1 << (7 - i));
        }
        digitalWrite(clockPin, LOW);
    }
    return value;
}
