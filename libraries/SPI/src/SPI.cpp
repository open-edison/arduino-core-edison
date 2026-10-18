/*
  SPI.cpp - SPI through Linux's spidev (SPI5).
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.
*/
#include "SPI.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <vector>

#include "gpio.h"
#include "variant.h"

namespace {

// The fastest clock tested with the kit's level shifters. Faster requests get this, as on
// other boards, where asking for more than the hardware can do gives its fastest clock
// (Adafruit's ST77xx library, for one, asks for 32 MHz).
const uint32_t max_clock = 10000000;

uint32_t limit_clock(uint32_t hz) { return hz > max_clock ? max_clock : hz; }

uint8_t reverse(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

}  // namespace

EdisonSPI::EdisonSPI(const char *device) : device_(device) {}

void EdisonSPI::begin() {
    if (fd_ >= 0) return;
    if (!edison_spi_pins_enable(true)) return;
    fd_ = open(device_, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        if (!warned_) {
            warned_ = true;
            fprintf(stderr, "SPI: can't open %s: %s (needs edison-image v0.2.6 or later)\n",
                    device_, strerror(errno));
        }
        edison_spi_pins_enable(false);
        return;
    }
    // The device's own settings (the chip select polarity, in particular).
    if (ioctl(fd_, SPI_IOC_RD_MODE32, &base_mode_) < 0) {
        uint8_t m8 = 0;
        ioctl(fd_, SPI_IOC_RD_MODE, &m8);
        base_mode_ = m8;
    }
    // The device already has a clock mode; don't set it again unless it changes.
    applied_mode_ = uint8_t(base_mode_ & SPI_MODE_3);
    // The most spidev takes in one message.
    if (FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r")) {
        unsigned long n = 0;
        if (fscanf(f, "%lu", &n) == 1 && n > 0) max_message_ = n;
        fclose(f);
    }
}

void EdisonSPI::end() {
    if (fd_ < 0) return;
    close(fd_);
    fd_ = -1;
    edison_spi_pins_enable(false);
}

void EdisonSPI::beginTransaction(arduino::SPISettings settings) {
    // Until endTransaction(): analogRead() (the kit ADC shares SPI5) and other threads'
    // transactions wait, as the device may be selected by its GPIO chip select.
    edison_spi_transaction_begin();
    clock_ = limit_clock(settings.getClockFreq());
    mode_ = uint8_t(settings.getDataMode());
    lsb_first_ = settings.getBitOrder() == LSBFIRST;
}

void EdisonSPI::endTransaction() { edison_spi_transaction_end(); }

void EdisonSPI::setBitOrder(BitOrder order) { lsb_first_ = order == LSBFIRST; }

void EdisonSPI::setDataMode(uint8_t mode) { mode_ = mode & 3; }

void EdisonSPI::setClockDivider(uint8_t divider) {
    if (divider) clock_ = limit_clock(16000000 / divider);
}

// The SPI mode is a property of the device file; change it only when it changes.
bool EdisonSPI::apply() {
    if (fd_ < 0) begin();
    if (fd_ < 0) return false;
    if (mode_ != applied_mode_) {
        // Keep the device's chip select polarity (SPI_CS_HIGH, from edison-image's overlay):
        // only the clock mode is the sketch's to set.
        uint32_t mode = (base_mode_ & ~uint32_t(SPI_MODE_3)) | mode_;
        if (ioctl(fd_, SPI_IOC_WR_MODE32, &mode) < 0) {
            uint8_t m8 = uint8_t(mode);
            if (ioctl(fd_, SPI_IOC_WR_MODE, &m8) < 0) return false;
        }
        applied_mode_ = mode_;
    }
    return true;
}

void EdisonSPI::transfer(const void *tx, void *rx, size_t count) {
    if (!count || !apply()) {
        if (rx) memset(rx, 0, count);
        return;
    }
    // The caller's buffers go to spidev as they are (it copies them; tx and rx may be the
    // same buffer). Only an LSB-first transfer, or one with nothing to send, needs a copy:
    // the controller sends most significant bit first, so bit order is done here.
    const uint8_t *out = static_cast<const uint8_t *>(tx);
    if (!tx || lsb_first_) {
        if (scratch_.size() < count) scratch_.resize(count);
        for (size_t i = 0; i < count; i++) {
            scratch_[i] = !tx ? 0 : lsb_first_ ? reverse(out[i]) : out[i];
        }
        out = scratch_.data();
    }
    uint8_t *in = static_cast<uint8_t *>(rx);
    // One message per spidev buffer's worth. edison-image (v0.2.6) has the SPI controller
    // use PIO for every transfer: its DMA hangs the board, so this needs that image.
    for (size_t off = 0; off < count; off += max_message_) {
        spi_ioc_transfer t = {};
        t.tx_buf = reinterpret_cast<uintptr_t>(out + off);
        t.rx_buf = in ? reinterpret_cast<uintptr_t>(in + off) : 0;
        t.len = uint32_t(count - off < max_message_ ? count - off : max_message_);
        t.speed_hz = clock_;
        t.bits_per_word = 8;
        if (ioctl(fd_, SPI_IOC_MESSAGE(1), &t) < 0) {
            if (!warned_) {
                warned_ = true;
                fprintf(stderr, "SPI: transfer failed: %s\n", strerror(errno));
            }
            break;
        }
    }
    if (in && lsb_first_) {
        for (size_t i = 0; i < count; i++) in[i] = reverse(in[i]);
    }
}

void EdisonSPI::transfer(void *buf, size_t count) { transfer(buf, buf, count); }

uint8_t EdisonSPI::transfer(uint8_t data) {
    uint8_t in = 0;
    transfer(&data, &in, 1);
    return in;
}

uint16_t EdisonSPI::transfer16(uint16_t data) {
    // As on AVR: most significant byte first, unless the bit order is LSB first.
    uint8_t out[2], in[2];
    if (lsb_first_) {
        out[0] = uint8_t(data);
        out[1] = uint8_t(data >> 8);
    } else {
        out[0] = uint8_t(data >> 8);
        out[1] = uint8_t(data);
    }
    transfer(out, in, 2);
    return lsb_first_ ? uint16_t(in[0] | in[1] << 8) : uint16_t(in[0] << 8 | in[1]);
}

EdisonSPI SPI(edison_spi_device);
