/*
  SPI.h - SPI on the Intel Edison Kit for Arduino's header, through Linux's spidev.
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.

  SPI5 on the header: SCK on 13 (the DS2 LED flickers with the clock), MISO on 12, MOSI
  on 11, and the ICSP header. SPI.begin() takes those three pins from GPIO; SPI.end() or
  pinMode() gives them back. SS (10), or any other pin, is the device's chip select, which
  the sketch or library drives with digitalWrite(), as usual. Controller mode only.
*/
#pragma once

#include <Arduino.h>

#include <vector>

#include "api/HardwareSPI.h"

// AVR-era clock dividers (of 16 MHz), for libraries that still use setClockDivider().
#define SPI_CLOCK_DIV2 2
#define SPI_CLOCK_DIV4 4
#define SPI_CLOCK_DIV8 8
#define SPI_CLOCK_DIV16 16
#define SPI_CLOCK_DIV32 32
#define SPI_CLOCK_DIV64 64
#define SPI_CLOCK_DIV128 128

class EdisonSPI : public arduino::HardwareSPI {
  public:
    explicit EdisonSPI(const char *device);

    void begin() override;
    void end() override;

    void beginTransaction(arduino::SPISettings settings) override;
    void endTransaction(void) override {}

    uint8_t transfer(uint8_t data) override;
    uint16_t transfer16(uint16_t data) override;
    void transfer(void *buf, size_t count) override;
    // Separate send and receive buffers (either may be null).
    void transfer(const void *tx, void *rx, size_t count);

    // Handlers run in a thread of their own here: nothing to mask.
    void usingInterrupt(int) override {}
    void notUsingInterrupt(int) override {}
    void attachInterrupt() override {}
    void detachInterrupt() override {}

    // AVR-style settings, kept until the next beginTransaction().
    void setBitOrder(BitOrder order);
    void setDataMode(uint8_t mode);
    void setClockDivider(uint8_t divider);

  private:
    bool apply();

    const char *device_;
    int fd_ = -1;
    uint32_t clock_ = 4000000;
    uint8_t mode_ = 0;
    bool lsb_first_ = false;
    uint8_t applied_mode_ = 0xFF;
    uint32_t base_mode_ = 0;  // the device's mode bits at open (SPI_CS_HIGH)
    size_t max_message_ = 4096;  // spidev's limit on one message (its bufsiz)
    std::vector<uint8_t> scratch_;  // reused for LSB-first transfers and ones sending zeros
    bool warned_ = false;
};

extern EdisonSPI SPI;
