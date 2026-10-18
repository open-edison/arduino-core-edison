// Drive an ST7735 TFT (set TAB below) over SPI with Adafruit's libraries ("Adafruit ST7735 and
// ST7789 Library" and Adafruit GFX): draw a test card, to check the colours and that all
// four edges of the yellow border show, and time full-screen redraws and other drawing.
// Timings go to Serial and to the sketch's log (journalctl -u edison-sketch).
//
// Adafruit's library sends each pixel as two 1-byte SPI.transfer() calls on boards it has
// no fast path for (this one included), so its timings mostly measure the cost of a call.
// "bulk fill" sends a whole screen in one SPI.transfer(): what a driver that sends blocks
// gets.
//
// Wiring: SCK to 13, MOSI (SDA) to 11, CS to 10, DC (A0) to 8, RST to 9; VCC to 5V (or
// 3.3V), backlight to 3.3V. Set IOREF (J9) to 3.3V: most of these displays take 3.3 V
// logic.
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <stdio.h>

const int TFT_CS = 10, TFT_DC = 8, TFT_RST = 9;
// Which module: INITR_144GREENTAB for the 1.44" 128x128 one tested (with the wrong option,
// red and blue were swapped and the image was off by a few pixels: no border, noise along
// the right edge). 1.8" 128x160 modules want INITR_BLACKTAB, INITR_GREENTAB or INITR_REDTAB.
const uint8_t TAB = INITR_144GREENTAB;

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

void report(const char *what, unsigned long us) {
    char line[80];
    snprintf(line, sizeof line, "%-22s %9lu us", what, us);
    Serial.println(line);
    fprintf(stderr, "%s\n", line);
}

template <typename F>
void timed(const char *what, F draw) {
    unsigned long start = micros();
    draw();
    report(what, micros() - start);
}

void testCard() {
    const int w = tft.width(), h = tft.height();
    tft.fillScreen(ST77XX_BLACK);
    const uint16_t colours[] = {ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_WHITE};
    const char *names[] = {"RED", "GREEN", "BLUE", "WHITE"};
    const int bar = (h - 56) / 4;  // four bars between the header and footer text
    for (int i = 0; i < 4; i++) {
        tft.fillRect(0, 34 + i * bar, w, bar, colours[i]);
        tft.setCursor(6, 34 + i * bar + (bar - 8) / 2);
        tft.setTextColor(i == 3 ? ST77XX_BLACK : ST77XX_WHITE);
        tft.print(names[i]);
    }
    tft.drawRect(0, 0, w, h, ST77XX_YELLOW);  // all four edges should show
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(6, 8);
    tft.print("open-edison");
    tft.setCursor(6, 20);
    tft.print("ST7735 ");
    tft.print(w);
    tft.print("x");
    tft.print(h);
    tft.setCursor(6, h - 16);
    tft.print("bottom edge");
}

// Fill the screen with one SPI transfer, using the library only to set the window.
void bulkFill(uint16_t colour) {
    static uint8_t frame[160 * 128 * 2];
    const size_t bytes = size_t(tft.width()) * tft.height() * 2;
    for (size_t i = 0; i < bytes; i += 2) {
        frame[i] = colour >> 8;  // the display takes pixels most significant byte first
        frame[i + 1] = colour & 0xFF;
    }
    tft.startWrite();
    tft.setAddrWindow(0, 0, tft.width(), tft.height());
    SPI.transfer(frame, bytes);
    tft.endWrite();
}

void setup() {
    Serial.begin(115200);
    timed("initR", [] { tft.initR(TAB); });
}

void loop() {
    timed("fillScreen (red)", [] { tft.fillScreen(ST77XX_RED); });
    timed("fillScreen (green)", [] { tft.fillScreen(ST77XX_GREEN); });
    timed("fillScreen (blue)", [] { tft.fillScreen(ST77XX_BLUE); });
    timed("bulk fill (yellow)", [] { bulkFill(ST77XX_YELLOW); });
    timed("text (a screenful)", [] {
        tft.fillScreen(ST77XX_BLACK);
        tft.setCursor(0, 0);
        tft.setTextColor(ST77XX_GREEN);
        for (int i = 0; i < tft.height() / 8; i++) tft.println("The quick brown fox");
    });
    timed("lines (64)", [] {
        tft.fillScreen(ST77XX_BLACK);
        for (int x = 0; x < tft.width(); x += 4) {
            tft.drawLine(0, 0, x, tft.height() - 1, ST77XX_CYAN);
        }
        for (int y = 0; y < tft.height(); y += 5) {
            tft.drawLine(0, 0, tft.width() - 1, y, ST77XX_MAGENTA);
        }
    });
    timed("test card", testCard);
    Serial.println();
    delay(10000);
}
