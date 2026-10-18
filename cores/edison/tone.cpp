/*
  tone.cpp - tone() and noTone().
  Copyright (c) 2026 The open-edison contributors.

  This library is free software; you can redistribute it and/or modify it under the terms
  of the GNU Lesser General Public License as published by the Free Software Foundation;
  either version 2.1 of the License, or (at your option) any later version.

  As on AVR boards: a square wave (50% duty), one tone at a time (a call for another pin
  while one plays does nothing; the same pin changes frequency), the duration doesn't block,
  and noTone() leaves the pin LOW. On ~3, ~5, ~6 and ~9 the hardware PWM makes the tone, at
  the exact frequency. On other pins a thread toggles the pin. Linux wakes a thread up to a
  few hundred microseconds late, so it sleeps until shortly before each edge and spins for
  the rest: edges then land within microseconds (unless something preempts it), at the cost
  of CPU time, up to a whole core at higher frequencies.
*/
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "Arduino.h"
#include "edison_pwm.h"

namespace {

using Clock = std::chrono::steady_clock;

// How long before an edge the thread stops sleeping and spins: more than Linux's usual
// wakeup lateness here.
const auto SPIN = std::chrono::microseconds(300);

std::mutex lock;
std::condition_variable changed;
int playing = -1;           // the pin with a tone, or -1
bool hardware = false;      // made by the PWM rather than the thread
Clock::duration half;       // half a period, for the thread
Clock::time_point until;    // when to stop, if timed
bool timed = false;
std::atomic<unsigned> generation{0};  // changes whenever the tone does

// Stop the tone and leave the pin LOW. Called with the lock held.
void stop_locked() {
    if (playing < 0) return;
    if (hardware) edisonPwmStop(uint8_t(playing));
    pinMode(pin_size_t(playing), OUTPUT);  // back from the PWM function, if it had it
    digitalWrite(pin_size_t(playing), LOW);
    playing = -1;
    generation++;
    changed.notify_all();
}

// Ends timed tones, and toggles the pin for tones the PWM can't make.
void run() {
    std::unique_lock<std::mutex> l(lock);
    for (;;) {
        if (playing < 0) {
            changed.wait(l);
            continue;
        }
        const unsigned gen = generation;
        if (hardware) {
            if (timed) {
                if (!changed.wait_until(l, until, [&] { return generation != gen; })) {
                    stop_locked();
                }
            } else {
                changed.wait(l, [&] { return generation != gen; });
            }
            continue;
        }
        // Toggle on a fixed schedule, so a late wakeup doesn't push later edges back. Without
        // the lock: tone() and noTone() change the generation to stop this.
        const int pin = playing;
        const Clock::duration step = half;
        const bool stops = timed;
        const Clock::time_point end = until;
        l.unlock();
        bool high = false, expired = false;
        Clock::time_point next = Clock::now();
        while (generation == gen) {
            if (stops && Clock::now() >= end) {
                expired = true;
                break;
            }
            high = !high;
            {
                // Briefly, so a noTone() can't come between the check and the write.
                std::lock_guard<std::unique_lock<std::mutex>> g(l);
                if (generation != gen) break;
                digitalWrite(pin_size_t(pin), high ? HIGH : LOW);
            }
            next += step;
            if (Clock::now() > next + step) next = Clock::now();  // fell behind: catch up
            // Sleep until just before the edge, then spin to it.
            if (next - SPIN > Clock::now()) std::this_thread::sleep_until(next - SPIN);
            while (Clock::now() < next && generation == gen) {
            }
        }
        l.lock();
        if (expired && generation == gen) stop_locked();
    }
}

}  // namespace

void tone(uint8_t pin, unsigned int frequency, unsigned long duration) {
    if (pin >= NUM_DIGITAL_PINS) return;
    if (frequency == 0) {
        noTone(pin);
        return;
    }
    static std::once_flag started;
    std::call_once(started, [] { std::thread(run).detach(); });

    std::lock_guard<std::mutex> l(lock);
    if (playing >= 0 && playing != pin) return;
    const uint32_t period_ns = uint32_t(1000000000ull / frequency);
    // A new tone, or a new frequency for a PWM tone: the PWM makes it if it can.
    if (playing < 0 || hardware) {
        hardware = edisonPwmWrite(pin, period_ns, period_ns / 2);
        if (!hardware) pinMode(pin, OUTPUT);  // the thread toggles it instead
    }
    playing = pin;
    half = std::chrono::nanoseconds(period_ns / 2);
    timed = duration > 0;
    until = Clock::now() + std::chrono::milliseconds(duration);
    generation++;
    changed.notify_all();
}

void noTone(uint8_t pin) {
    std::lock_guard<std::mutex> l(lock);
    if (playing == pin) stop_locked();
}
