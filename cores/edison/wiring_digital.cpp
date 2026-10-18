/*
  wiring_digital.cpp - pinMode(), digitalWrite() and digitalRead() through libgpiod.
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.
*/
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "Arduino.h"
#include "gpio.h"
#include "variant.h"

namespace {

const char *const CONSUMER = "sketch";

struct Line {
    int chip = -1;
    unsigned offset = 0;
    explicit operator bool() const { return chip >= 0; }
};

std::vector<gpiod_chip *> &chips() {
    static std::vector<gpiod_chip *> list = [] {
        std::vector<gpiod_chip *> v;
        for (int i = 0;; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/gpiochip%d", i);
            gpiod_chip *c = gpiod_chip_open(path);
            if (!c) {
                if (i == 0) fprintf(stderr, "gpio: can't open %s: %s\n", path, strerror(errno));
                break;
            }
            v.push_back(c);
        }
        return v;
    }();
    return list;
}

Line soc_line(int offset) { return chips().empty() ? Line{} : Line{0, unsigned(offset)}; }

Line named_line(const char *name) {
    static std::map<const char *, Line> cache;  // names are string literals from the table
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    Line l;
    auto &cs = chips();
    for (size_t i = 0; i < cs.size(); i++) {
        int off = gpiod_chip_get_line_offset_from_name(cs[i], name);
        if (off >= 0) {
            l = Line{int(i), unsigned(off)};
            break;
        }
    }
    if (!l) fprintf(stderr, "gpio: no line named %s (is the arduino ACPI overlay loaded?)\n", name);
    cache[name] = l;
    return l;
}

std::map<std::pair<int, unsigned>, gpiod_line_request *> held;

enum class Dir { In, Out };

// Request the line (or change it, if the sketch already holds it). Returns false, with errno
// set, if the line can't be had, e.g. because another process holds it (EBUSY).
bool set_line(Line l, Dir dir, int value = 0) {
    if (!l) return false;
    gpiod_line_settings *s = gpiod_line_settings_new();
    gpiod_line_config *lc = gpiod_line_config_new();
    if (!s || !lc) {
        gpiod_line_settings_free(s);
        gpiod_line_config_free(lc);
        return false;
    }
    if (dir == Dir::Out) {
        gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_OUTPUT);
        gpiod_line_settings_set_output_value(
            s, value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
    } else {
        gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_INPUT);
    }
    gpiod_line_config_add_line_settings(lc, &l.offset, 1, s);

    bool ok;
    auto key = std::make_pair(l.chip, l.offset);
    auto it = held.find(key);
    if (it != held.end()) {
        ok = gpiod_line_request_reconfigure_lines(it->second, lc) == 0;
    } else {
        gpiod_request_config *rc = gpiod_request_config_new();
        if (rc) gpiod_request_config_set_consumer(rc, CONSUMER);
        gpiod_line_request *req = gpiod_chip_request_lines(chips()[l.chip], rc, lc);
        gpiod_request_config_free(rc);
        ok = req != nullptr;
        if (ok) held[key] = req;
    }
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(s);
    return ok;
}

gpiod_line_request *request_of(Line l) {
    auto it = held.find(std::make_pair(l.chip, l.offset));
    return it == held.end() ? nullptr : it->second;
}

void release_line(Line l) {
    auto it = held.find(std::make_pair(l.chip, l.offset));
    if (it == held.end()) return;
    gpiod_line_request_release(it->second);
    held.erase(it);
}

enum class State { Unset, Input, Output, Analog, Pwm, Spi, Failed };

bool spi_active = false;  // IO11-IO13 are routed to SPI5 (SPI library)
State state[NUM_DIGITAL_PINS];
// Edge detection left on by pulseIn() (reconfiguring the pin turns it off).
bool pulse_edges[NUM_DIGITAL_PINS];

// Report each pin's problem once: sketches call these functions in loops.
bool reported[NUM_DIGITAL_PINS];
void report(pin_size_t pin, const char *what, const char *why) {
    if (reported[pin]) return;
    reported[pin] = true;
    fprintf(stderr, "pin %u: %s: %s\n", unsigned(pin), what, why);
}

void set_pull(const EdisonPinDesc &p, PinMode mode) {
    Line l = named_line(p.pull);
    if (mode == INPUT_PULLUP) {
        set_line(l, Dir::Out, 1);
    } else if (mode == INPUT_PULLDOWN) {
        set_line(l, Dir::Out, 0);
    } else {
        set_line(l, Dir::In);
    }
}

bool configure(pin_size_t pin, PinMode mode) {
    const EdisonPinDesc &p = edison_pins[pin];
    if (p.busy) {
        errno = EBUSY;
        report(pin, "in use by", p.busy);
        return false;
    }
    if (mode == OUTPUT_OPENDRAIN) {
        report(pin, "pinMode", "OUTPUT_OPENDRAIN isn't supported (the level shifter drives)");
        return false;
    }
    for (const EdisonMuxSetting &m : p.gpio_mux) {
        if (m.line && !set_line(named_line(m.line), Dir::Out, m.value)) {
            report(pin, m.line, strerror(errno));
            return false;
        }
    }
    Line soc = soc_line(p.soc), dir = named_line(p.dir);
    // Turn the level shifter round only while the SoC side isn't driving into it.
    bool ok;
    if (mode == OUTPUT) {
        set_pull(p, INPUT);
        ok = set_line(dir, Dir::Out, 1) && set_line(soc, Dir::Out, 0);
    } else {
        ok = set_line(soc, Dir::In) && set_line(dir, Dir::Out, 0);
        if (ok) set_pull(p, mode);
    }
    if (!ok) {
        report(pin, "can't claim its GPIO lines", strerror(errno));
        return false;
    }
    return true;
}

void set_mode(pin_size_t pin, PinMode mode) {
    // Requesting the SoC line below returns the pin from PWM to GPIO; stop the PWM first.
    if (state[pin] == State::Pwm) edison_pwm_stop(edison_pins[pin].pwm);
    pulse_edges[pin] = false;
    if (configure(pin, mode)) {
        state[pin] = mode == OUTPUT ? State::Output : State::Input;
    } else {
        state[pin] = State::Failed;
    }
}

// The pin as an analog input: the level shifter and pull resistor out of the way, and the
// mux switched from the GPIO to the ADC.
bool configure_analog(pin_size_t pin) {
    const EdisonPinDesc &p = edison_pins[pin];
    if (p.busy) {
        errno = EBUSY;
        report(pin, "in use by", p.busy);
        return false;
    }
    if (p.adc < 0) {
        report(pin, "analogRead", "not an analog input (use A0-A3)");
        return false;
    }
    // Isolate the ADC's SPI bus from the header (see edison_spi_isolate). Busy when SPI is
    // enabled on the header, which isolates it properly; nothing to do then.
    // With SPI on the header, the ADC shares the bus with it properly: leave it be.
    static bool isolated = false;
    if (!isolated && !spi_active) {
        for (const EdisonMuxSetting &m : edison_spi_isolate) {
            set_line(named_line(m.line), Dir::Out, m.value);
        }
        isolated = true;
    }
    Line soc = soc_line(p.soc), dir = named_line(p.dir);
    bool ok = set_line(soc, Dir::In) && set_line(dir, Dir::Out, 0) &&
              set_line(named_line(p.adc_mux.line), Dir::Out, p.adc_mux.value);
    if (ok) set_pull(p, INPUT);
    if (!ok) {
        report(pin, "can't claim its analog lines", strerror(errno));
        return false;
    }
    return true;
}

// The pin as a PWM output: the level shifter to output, the pull resistor off, and the SoC
// pin switched from its GPIO to its PWM function (which needs the GPIO line let go).
bool configure_pwm(pin_size_t pin) {
    const EdisonPinDesc &p = edison_pins[pin];
    if (p.busy) {
        errno = EBUSY;
        report(pin, "in use by", p.busy);
        return false;
    }
    release_line(soc_line(p.soc));
    set_pull(p, INPUT);
    if (!set_line(named_line(p.dir), Dir::Out, 1)) {
        report(pin, "can't claim its PWM lines", strerror(errno));
        return false;
    }
    if (!edison_pwm_pinmux(p.pwm)) {
        report(pin, "analogWrite",
               "can't switch the pin to PWM (needs edison-image v0.2.5 or later); "
               "writing HIGH/LOW instead");
        return false;
    }
    return true;
}

// Held by every pin function, and taken for good by edison_gpio_release_all() when the
// sketch is stopped (from another thread), so the two never touch the lines at once.
std::mutex gpio_lock;

}  // namespace

bool edison_spi_pins_enable(bool on) {
    std::lock_guard<std::mutex> lock(gpio_lock);
    if (on == spi_active) return true;
    if (!on) {
        // Back to inputs: configure() also routes the muxes back to the GPIOs.
        for (const EdisonSpiPin &s : edison_spi_pins) set_mode(pin_size_t(s.pin), INPUT);
        spi_active = false;
        return true;
    }
    for (const EdisonSpiPin &s : edison_spi_pins) {
        pin_size_t pin = pin_size_t(s.pin);
        const EdisonPinDesc &p = edison_pins[pin];
        if (p.busy) {
            report(pin, "in use by", p.busy);
            return false;
        }
        if (state[pin] == State::Pwm) edison_pwm_stop(p.pwm);
        // The SoC GPIO is cut off from the pin by the mux; let it go.
        release_line(soc_line(p.soc));
        set_pull(p, INPUT);
        bool ok = set_line(named_line(p.dir), Dir::Out, s.output ? 1 : 0);
        // The pin's other muxes as for its GPIO (e.g. IO11's PWM mux), then SPI5.
        for (const EdisonMuxSetting &m : p.gpio_mux) {
            if (ok && m.line && strcmp(m.line, s.sel) != 0) {
                ok = set_line(named_line(m.line), Dir::Out, m.value);
            }
        }
        ok = ok && set_line(named_line(s.sel), Dir::Out, 1);
        if (!ok) {
            report(pin, "can't claim its SPI lines", strerror(errno));
            return false;
        }
        state[pin] = State::Spi;
    }
    spi_active = true;
    return true;
}

namespace {

// The edges attachInterrupt() asked for on each pin (0: none).
int edges_wanted[NUM_DIGITAL_PINS];

// Set an input line's edge detection. Called with gpio_lock held.
bool set_edges(Line soc, gpiod_line_request *r, gpiod_line_edge edge) {
    gpiod_line_settings *s = gpiod_line_settings_new();
    gpiod_line_config *lc = gpiod_line_config_new();
    bool ok = false;
    if (s && lc) {
        gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_edge_detection(s, edge);
        gpiod_line_config_add_line_settings(lc, &soc.offset, 1, s);
        ok = gpiod_line_request_reconfigure_lines(r, lc) == 0;
    }
    gpiod_line_config_free(lc);
    gpiod_line_settings_free(s);
    return ok;
}

// As on other boards, a pin starts as an input; edge functions don't change one that
// pinMode() made an output or another function. Called with gpio_lock held.
bool input_for(unsigned pin, const char *what) {
    if (state[pin] == State::Unset) set_mode(pin_size_t(pin), INPUT);
    if (state[pin] != State::Input) {
        report(pin_size_t(pin), what, "the pin isn't an input (pinMode INPUT)");
        return false;
    }
    return true;
}

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000u + ts.tv_nsec;
}

// While it exists, asks the kernel to keep the CPUs within 10 us of waking (C1, not the
// deep idle states). Does nothing if the sketch may not (an image before v0.2.7).
class CpuLatency {
  public:
    CpuLatency() {
        fd_ = open("/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
        int32_t us = 10;
        if (fd_ >= 0 && write(fd_, &us, sizeof(us)) != ssize_t(sizeof(us))) {
            close(fd_);
            fd_ = -1;
        }
    }
    ~CpuLatency() {
        if (fd_ >= 0) close(fd_);  // closing withdraws the request
    }
    CpuLatency(const CpuLatency &) = delete;
    CpuLatency &operator=(const CpuLatency &) = delete;

  private:
    int fd_ = -1;
};

}  // namespace

int edison_gpio_edges(unsigned pin, int edges) {
    if (pin >= NUM_DIGITAL_PINS) return -1;
    std::lock_guard<std::mutex> lock(gpio_lock);
    if (!input_for(pin, "attachInterrupt")) return -1;
    Line soc = soc_line(edison_pins[pin].soc);
    gpiod_line_request *r = request_of(soc);
    if (!r) return -1;
    gpiod_line_edge edge = edges == RISING    ? GPIOD_LINE_EDGE_RISING
                           : edges == FALLING ? GPIOD_LINE_EDGE_FALLING
                           : edges == CHANGE  ? GPIOD_LINE_EDGE_BOTH
                                              : GPIOD_LINE_EDGE_NONE;
    if (!set_edges(soc, r, edge)) {
        report(pin_size_t(pin), "attachInterrupt", strerror(errno));
        return -1;
    }
    edges_wanted[pin] = edges;
    pulse_edges[pin] = false;
    return gpiod_line_request_get_fd(r);
}

long edison_gpio_pulse(unsigned pin, bool high, unsigned long timeout_us) {
    if (pin >= NUM_DIGITAL_PINS) return 0;
    Line soc = soc_line(edison_pins[pin].soc);
    int fd;
    bool level;
    {
        std::lock_guard<std::mutex> lock(gpio_lock);
        if (!input_for(pin, "pulseIn")) return 0;
        // attachInterrupt()'s thread reads this pin's edges: the caller polls instead.
        if (edges_wanted[pin]) return -1;
        gpiod_line_request *r = request_of(soc);
        if (!r) return -1;
        fd = gpiod_line_request_get_fd(r);
        // Edge detection stays on between calls: turning it on (the kernel requesting an
        // interrupt) is slow. Edges from before this call are stale: drop them.
        if (pulse_edges[pin]) {
            static gpiod_edge_event_buffer *stale = gpiod_edge_event_buffer_new(16);
            pollfd p = {fd, POLLIN, 0};
            while (stale && poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
                if (gpiod_line_request_read_edge_events(r, stale, 16) <= 0) break;
            }
        } else if (set_edges(soc, r, GPIOD_LINE_EDGE_BOTH)) {
            pulse_edges[pin] = true;
        } else {
            return -1;
        }
        // Read after edges are on, so no change can fall between the two.
        level = gpiod_line_request_get_value(r, soc.offset) == GPIOD_LINE_VALUE_ACTIVE;
    }
    // The kernel timestamps each edge when it handles the interrupt, so the pulse is
    // measured where it happened, however late this thread gets to read it, as long as the
    // CPU is awake to handle it: from a deep idle state (C6, C7) that takes 0.1-1.2 ms, and
    // varies. Keep the CPUs out of those while waiting (edison-image v0.2.7 lets sketches;
    // without it, timings are off by up to a few hundred microseconds).
    CpuLatency awake;
    // Wait without the lock: the stop thread needs it.
    const uint64_t deadline = now_ns() + uint64_t(timeout_us) * 1000u;
    bool leaving = level == high;  // in a pulse already: wait for the next whole one
    bool started = false, done = false;
    uint64_t start = 0, end = 0;
    while (!done) {
        uint64_t now = now_ns();
        if (now >= deadline) break;
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, int((deadline - now + 999999) / 1000000)) <= 0) continue;
        std::lock_guard<std::mutex> lock(gpio_lock);
        static gpiod_edge_event_buffer *buf = gpiod_edge_event_buffer_new(16);
        gpiod_line_request *r = request_of(soc);
        if (!r || !buf) break;
        int n = gpiod_line_request_read_edge_events(r, buf, 16);
        for (int i = 0; i < n && !done; i++) {
            gpiod_edge_event *e = gpiod_edge_event_buffer_get_event(buf, i);
            bool now_high = gpiod_edge_event_get_event_type(e) == GPIOD_EDGE_EVENT_RISING_EDGE;
            uint64_t ts = gpiod_edge_event_get_timestamp_ns(e);
            if (leaving) {
                leaving = now_high == high;
            } else if (!started) {
                if (now_high == high) {
                    started = true;
                    start = ts;
                }
            } else if (now_high != high) {
                end = ts;
                done = true;
            }
        }
    }
    return done ? long((end - start) / 1000u) : 0;
}

void edison_gpio_events(unsigned pin, void (*cb)(bool rising, void *arg), void *arg) {
    if (pin >= NUM_DIGITAL_PINS) return;
    bool rising[16];
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(gpio_lock);
        static gpiod_edge_event_buffer *buf = gpiod_edge_event_buffer_new(16);
        gpiod_line_request *r = request_of(soc_line(edison_pins[pin].soc));
        if (!r || !buf) return;
        n = gpiod_line_request_read_edge_events(r, buf, 16);
        for (int i = 0; i < n; i++) {
            gpiod_edge_event *e = gpiod_edge_event_buffer_get_event(buf, i);
            rising[i] = gpiod_edge_event_get_event_type(e) == GPIOD_EDGE_EVENT_RISING_EDGE;
        }
    }
    // Outside the lock: the handler will likely use the pins.
    for (int i = 0; i < n; i++) cb(rising[i], arg);
}

int edison_pwm_select(unsigned pin) {
    if (pin >= NUM_DIGITAL_PINS || edison_pins[pin].pwm < 0) return -1;
    std::lock_guard<std::mutex> lock(gpio_lock);
    static bool pwm_failed[NUM_DIGITAL_PINS];  // don't retry on every analogWrite()
    if (state[pin] == State::Pwm) return edison_pins[pin].pwm;
    if (pwm_failed[pin]) return -1;
    if (!configure_pwm(pin_size_t(pin))) {
        // Leave it to analogWrite() to drive the pin as a digital output instead.
        pwm_failed[pin] = true;
        state[pin] = State::Unset;
        return -1;
    }
    state[pin] = State::Pwm;
    return edison_pins[pin].pwm;
}

int edison_analog_select(unsigned pin) {
    if (pin >= NUM_DIGITAL_PINS) return -1;
    std::lock_guard<std::mutex> lock(gpio_lock);
    if (state[pin] != State::Analog) {
        state[pin] = configure_analog(pin_size_t(pin)) ? State::Analog : State::Failed;
    }
    return state[pin] == State::Analog ? edison_pins[pin].adc : -1;
}

void pinMode(pin_size_t pin, PinMode mode) {
    if (pin >= NUM_DIGITAL_PINS) return;
    std::lock_guard<std::mutex> lock(gpio_lock);
    set_mode(pin, mode);
}

void digitalWrite(pin_size_t pin, PinStatus value) {
    if (pin >= NUM_DIGITAL_PINS) return;
    std::lock_guard<std::mutex> lock(gpio_lock);
    if (state[pin] == State::Input) {
        // As on AVR: writing to an input switches its pull-up on or off.
        set_pull(edison_pins[pin], value == HIGH ? INPUT_PULLUP : INPUT);
        return;
    }
    if (state[pin] != State::Output) return;
    gpiod_line_request *r = request_of(soc_line(edison_pins[pin].soc));
    if (r) {
        gpiod_line_request_set_value(
            r, edison_pins[pin].soc,
            value == LOW ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE);
    }
}

PinStatus digitalRead(pin_size_t pin) {
    if (pin >= NUM_DIGITAL_PINS) return LOW;
    std::lock_guard<std::mutex> lock(gpio_lock);
    // Pins start as inputs on other boards; do the same for a pin read before pinMode().
    if (state[pin] == State::Unset) set_mode(pin, INPUT);
    if (state[pin] == State::Failed) return LOW;
    gpiod_line_request *r = request_of(soc_line(edison_pins[pin].soc));
    if (!r) return LOW;
    return gpiod_line_request_get_value(r, edison_pins[pin].soc) == GPIOD_LINE_VALUE_ACTIVE
               ? HIGH
               : LOW;
}

void edison_gpio_release_all() {
    // Never unlocked: the process exits next, and any pin call the sketch makes meanwhile
    // waits rather than undoing this.
    gpio_lock.lock();
    // Leave the pins as inputs, as a reset does on other boards: the lines keep their last
    // levels once released, so an output would otherwise go on driving.
    for (pin_size_t pin = 0; pin < NUM_DIGITAL_PINS; pin++) {
        if (state[pin] == State::Pwm) edison_pwm_stop(edison_pins[pin].pwm);
        if (state[pin] == State::Output || state[pin] == State::Input ||
            state[pin] == State::Analog || state[pin] == State::Pwm ||
            state[pin] == State::Spi) {
            configure(pin, INPUT);
        }
    }
    for (auto &h : held) gpiod_line_request_release(h.second);
    held.clear();
}
