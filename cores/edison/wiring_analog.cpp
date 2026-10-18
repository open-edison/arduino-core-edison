/*
  wiring_analog.cpp - analogRead() through the kit's ADC (TI ADS7951, Linux IIO).
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.

  The ADC's full scale is twice its reference, which is IOREF halved (with J8 on IOREF), so
  a full reading is IOREF: 5 V or 3.3 V, as set by J9. Each read of the IIO channel's raw
  file is one conversion.
*/
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <mutex>
#include <string>

#include "Arduino.h"
#include "gpio.h"

namespace {

const int ADC_BITS = 12;
const int ADC_CHANNELS = 8;

int resolution = 10;
std::mutex lock;

// Held through an SPI transaction, and by analogRead() (see gpio.h).
std::recursive_timed_mutex spi_bus;
thread_local int transactions = 0;  // this thread's open transactions
int fds[ADC_CHANNELS] = {-1, -1, -1, -1, -1, -1, -1, -1};
bool failed[ADC_CHANNELS];  // couldn't be opened: reported once, then given up

// The kit ADC's IIO directory, found by name: device numbers depend on probe order.
std::string adc_dir() {
    static const std::string dir = [] {
        const char *base = "/sys/bus/iio/devices";
        std::string found;
        if (DIR *d = opendir(base)) {
            while (dirent *e = readdir(d)) {
                if (strncmp(e->d_name, "iio:device", 10) != 0) continue;
                std::string path = std::string(base) + "/" + e->d_name;
                char name[32] = {};
                FILE *f = fopen((path + "/name").c_str(), "r");
                if (!f) continue;
                bool match = fgets(name, sizeof(name), f) && strncmp(name, "ads7951", 7) == 0;
                fclose(f);
                if (match) {
                    found = path;
                    break;
                }
            }
            closedir(d);
        }
        if (found.empty()) fprintf(stderr, "analogRead: the kit's ADC (ads7951) isn't present\n");
        return found;
    }();
    return dir;
}

// The channel's raw value (0-4095), or -1 (reported once per channel).
int read_raw(int ch) {
    std::lock_guard<std::mutex> l(lock);
    if (failed[ch]) return -1;
    if (fds[ch] < 0) {
        std::string dir = adc_dir();
        if (dir.empty()) {
            failed[ch] = true;
            return -1;
        }
        std::string path = dir + "/in_voltage" + std::to_string(ch) + "_raw";
        fds[ch] = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fds[ch] < 0) {
            fprintf(stderr, "analogRead: can't open %s: %s\n", path.c_str(), strerror(errno));
            failed[ch] = true;
            return -1;
        }
    }
    char buf[16];
    ssize_t n = pread(fds[ch], buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        // Report once, but keep trying: a read can fail on its own.
        static bool reported[ADC_CHANNELS];
        if (!reported[ch]) {
            fprintf(stderr, "analogRead: ADC channel %d: %s\n", ch,
                    n < 0 ? strerror(errno) : "no data");
            reported[ch] = true;
        }
        return -1;
    }
    buf[n] = '\0';
    return atoi(buf);
}

}  // namespace

void edison_spi_transaction_begin() {
    spi_bus.lock();
    transactions++;
}

void edison_spi_transaction_end() {
    if (transactions == 0) return;  // an end without a begin, in this thread
    transactions--;
    spi_bus.unlock();
}

int analogRead(pin_size_t pin) {
    // analogRead(0) means A0, as on other boards.
    if (pin < NUM_ANALOG_INPUTS) pin += PIN_A0;
    int ch = edison_analog_select(pin);
    if (ch < 0) return 0;
    // Wait for another thread's SPI transaction to end, but not forever: a sketch might
    // never end one. (Within this thread's own transaction it goes ahead.)
    bool held = spi_bus.try_lock_for(std::chrono::seconds(1));
    if (!held) {
        static bool reported;
        if (!reported) {
            reported = true;
            fprintf(stderr, "analogRead: an SPI transaction has been open for 1 s; reading anyway\n");
        }
    }
    int raw = read_raw(ch);
    if (held) spi_bus.unlock();
    if (raw < 0) return 0;
    return resolution >= ADC_BITS ? raw << (resolution - ADC_BITS) : raw >> (ADC_BITS - resolution);
}

void analogReadResolution(int bits) {
    if (bits >= 1 && bits <= 16) resolution = bits;
}

// The reference is set by jumpers (J8, J9), not software.
void analogReference(uint8_t) {}
