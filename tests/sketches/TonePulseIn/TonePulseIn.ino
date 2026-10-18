// tone() measured with pulseIn(): jumper each tone pin to its input pin. 11 -> 12 tests a
// tone made by toggling the pin (11 has no PWM); 3 -> 2 a tone made by the hardware PWM
// (any input pin works, A0 included). A pair without a jumper reports "no signal". Results
// on Serial and the sketch's log. pulseIn() needs edison-image v0.2.7 to be exact.
#include <stdio.h>

struct Pair {
    uint8_t tone_pin, in_pin;
};
const Pair pairs[] = {{11, 12}, {3, 2}};
const unsigned frequencies[] = {100, 440, 1000, 2000, 5000};

void say(const char *line) {
    Serial.println(line);
    fprintf(stderr, "%s\n", line);
}

// Average, min and max of n pulse lengths at a level, in microseconds.
bool measure(uint8_t pin, uint8_t level, int n, unsigned long &avg, unsigned long &lo,
             unsigned long &hi) {
    unsigned long sum = 0;
    lo = ~0UL;
    hi = 0;
    for (int i = 0; i < n; i++) {
        unsigned long us = pulseIn(pin, level, 100000);
        if (us == 0) return false;
        sum += us;
        if (us < lo) lo = us;
        if (us > hi) hi = us;
    }
    avg = sum / n;
    return true;
}

void test(const Pair &p) {
    char line[120];
    snprintf(line, sizeof line, "tone on %u, measured on %u:", p.tone_pin, p.in_pin);
    say(line);
    pinMode(p.in_pin, INPUT);
    for (unsigned f : frequencies) {
        tone(p.tone_pin, f);
        delay(50);
        unsigned long h, hlo, hhi, l, llo, lhi;
        if (!measure(p.in_pin, HIGH, 20, h, hlo, hhi) || !measure(p.in_pin, LOW, 20, l, llo, lhi)) {
            snprintf(line, sizeof line, "  %5u Hz: no signal (jumper?)", f);
            say(line);
            noTone(p.tone_pin);
            return;
        }
        snprintf(line, sizeof line,
                 "  %5u Hz: half period %lu us; high %lu (%lu-%lu), low %lu (%lu-%lu); %.1f Hz",
                 f, 500000UL / f, h, hlo, hhi, l, llo, lhi, 1e6 / double(h + l));
        say(line);
    }
    noTone(p.tone_pin);
    delay(10);
    snprintf(line, sizeof line, "  after noTone: input %s", digitalRead(p.in_pin) ? "HIGH (wrong)" : "LOW");
    say(line);
    tone(p.tone_pin, 1000, 200);
    delay(100);
    bool during = pulseIn(p.in_pin, HIGH, 20000) != 0;
    delay(200);
    bool after = pulseIn(p.in_pin, HIGH, 20000) != 0;
    snprintf(line, sizeof line, "  tone(1000 Hz, 200 ms): %s during, %s after",
             during ? "playing" : "SILENT", after ? "STILL PLAYING" : "stopped");
    say(line);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    for (const Pair &p : pairs) test(p);
    say("done");
}

void loop() { delay(1000); }
