# arduino-core-edison

An Arduino core for the Intel Edison on the Arduino expansion board, for boards running
[edison-image](https://github.com/open-edison/edison-image) (Debian 13). Part of
[open-edison](https://github.com/open-edison).

**Status: early.** Works: `pinMode`/`digitalWrite`/`digitalRead` on the header pins,
`Serial` (USB) and `Serial1` (IO0/IO1), `millis`/`micros`/delays, upload over USB or the
network, `analogRead` on A0-A3, `analogWrite` (PWM) on ~3/~5/~6/~9, `attachInterrupt`,
`Wire` (I2C), `SPI` and `Servo`. Not yet: `tone`, `pulseIn`.

## Installing

**Arduino IDE 2.x:** add this URL under File → Preferences → Additional boards manager URLs,
then install "Intel Edison" by open-edison in the Boards Manager:

```
https://github.com/open-edison/arduino-core-edison/releases/latest/download/package_open-edison_index.json
```

**PlatformIO:** use [platform-edison](https://github.com/open-edison/platform-edison),
which installs the same release archives.

The board needs [edison-image](https://github.com/open-edison/edison-image) v0.2.0 or
later (it runs the sketch). Its
[hardware guide](https://github.com/open-edison/edison-image/blob/main/docs/hardware.md)
covers the board's jumpers (IOREF, AREF, PWM pins), buttons and quirks.

## How it works

A sketch is compiled on your computer into a Linux program for the Edison (by Zig's clang,
so the same toolchain works on Windows, macOS and Linux) and uploaded to
`/var/lib/edison-sketch/sketch.elf`. edison-image runs it as the `edison-sketch` service,
as the `edison` user, and restarts it when a new one arrives.

- **Pins** are driven through the Linux GPIO character devices (libgpiod). For each pin the
  core also sets the kit's level shifter direction, pull resistor and muxes, which are
  described in [variants/edison/variant.cpp](variants/edison/variant.cpp).
- Pins owned by a peripheral are refused, with a message in the sketch's log
  (`journalctl -u edison-sketch`): IO0/IO1 (UART1, `Serial1`) and A4/A5 (I2C-6, `Wire`).
- **Serial** is the USB serial port on J16 (`/dev/ttyGS0` on the board). Output is dropped
  while nothing on the computer reads the port, as on other USB Arduinos; the last few
  KiB from before can still arrive when the port is opened.
- **analogRead** uses the kit's 12-bit ADC (TI ADS7951) on A0-A3, returning 0-1023 by
  default (`analogReadResolution(12)` for 0-4095). Full scale is IOREF, the 5 V or 3.3 V
  set by jumper J9 (with J8 on IOREF); on USB power the 5 V rail, and so IOREF, is nearer
  4.5 V. `analogReference()` does nothing, as the reference is set by the jumpers. A4/A5
  are the I2C pins, so they can't be analog inputs. Needs edison-image v0.2.4 or later:
  on earlier images the ADC is left in its half range, and readings saturate above about
  half of IOREF.
- **analogWrite** drives hardware PWM on ~3, ~5, ~6 and ~9 (the SoC's four PWM channels, with
  the kit's default PWM jumpers J11/J12) at 490 Hz, 0-255 by default (`analogWriteResolution()`,
  up to 16 bits). Other pins are set HIGH or LOW, as on AVR boards. `pinMode()` takes a pin
  back from PWM. Needs edison-image v0.2.5 or later, which switches the pins to PWM for
  sketches (`edison-pinmux`); on earlier images those pins are set HIGH or LOW too.
- **attachInterrupt** works on any input pin: the kernel queues each edge, and a thread
  calls the handler, so handlers run alongside `loop()` rather than pausing it.
  `noInterrupts()`/`interrupts()` hold them off, so wrap reads of what a handler changes
  in them, as on other boards. `RISING`, `FALLING` and `CHANGE` are exact (tested at 490
  edges/s); `LOW`/`HIGH` are checked every millisecond while the level lasts.
- **SPI** is SPI5 on the header: SCK on 13, MISO on 12, MOSI on 11 (and the ICSP header).
  `SPI.begin()` takes those pins from GPIO (the DS2 LED on 13 flickers with the clock); `SPI.end()`
  or `pinMode()` gives them back. The device's chip select (10, or any pin) is driven by the
  sketch or library with `digitalWrite()`, as usual. All four modes and MSB/LSB first; up to
  10 MHz, the tested limit (a library asking for more gets 10 MHz, as other boards give their
  fastest clock). Each call costs about 100 us in the kernel whatever its size, and long
  transfers run at about 420 KB/s, so send blocks (`SPI.transfer(buffer, length)`) where you
  can. Libraries that send a byte at a time are slow: Adafruit's ST7735 driver takes about 4 s
  to fill a 128x128 display, which one 32 KB transfer does in 77 ms. Needs edison-image v0.2.6
  or later.
- **Servo** drives hobby servos on ~3, ~5, ~6 and ~9 (one each) from the hardware PWM at
  50 Hz. The hardware sets the pulse in 255ths of the period: steps of 78 us at 50 Hz,
  about 7.5 degrees over the default range. `servo.setRefreshInterval(10000)` (100 Hz)
  halves them, for servos that accept it (most digital ones).
- **Wire** is I2C-6 on SDA/SCL (A4/A5), through Linux's i2c-dev: controller mode only.
  `endTransmission(false)` followed by `requestFrom()` (a register read) is sent as one
  transfer with a repeated start. The bus speed is set by the kernel; `setClock()` does
  nothing.
- When the sketch stops, its pins go back to inputs.

## Uploading

Pick either port in the IDE (or `arduino-cli upload -p`):

- **The board's USB serial port** (J16): the upload goes over the USB network link on the
  same cable (to `192.168.2.15`), and the Serial Monitor works on the same port. Only one
  Edison can be connected by USB at a time, as they all use that address.
- **Its network port** (found by mDNS, over WiFi or USB): as with other boards' network
  uploads, the Serial Monitor isn't available on it.

The upload is done over SSH by `edison-upload` ([tools/edison-upload](tools/edison-upload)). The first upload from a computer asks for the `edison` user's password, and installs
a key for that computer (kept in `edison-upload` under your user config directory), so
later uploads don't need it. The IDE still asks once for each sketch you open and won't
accept an empty field, so type anything there: with the key installed, it's ignored. On the network, the board's host key is remembered on first use; if it
changes (another board at that address), `edison-upload -forget ADDRESS` clears it.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for building from source, tests, releasing and
conventions.

## License

LGPL-2.1-or-later, like the [ArduinoCore-API](https://github.com/arduino/ArduinoCore-API)
it builds on (vendored in `cores/edison/api`, from commit `0f4e57e`). See
[LICENSE](LICENSE).
