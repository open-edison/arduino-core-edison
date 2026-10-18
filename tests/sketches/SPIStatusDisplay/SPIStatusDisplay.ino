// Board status on a 1.44" 128x128 ST7735 TFT (wired as in SPIST7735, IOREF at 3.3 V): the
// time and date, the WiFi network, signal and address, the USB link's address and the
// uptime, once a second. Uses Adafruit's libraries to draw into a canvas in memory, then
// sends the whole screen in one SPI transfer (fast even where the library draws a pixel at
// a time). WiFi details come from wpa_supplicant's control socket, which the sketch's user
// (edison, in the netdev group) may use.
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

const int TFT_CS = 10, TFT_DC = 8, TFT_RST = 9;
const int W = 128, H = 128;

Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);
GFXcanvas16 canvas(W, H);

// The interface's IPv4 address, or "-" if it has none.
void ipv4(const char *name, char *out, size_t n) {
    snprintf(out, n, "-");
    ifaddrs *list;
    if (getifaddrs(&list) != 0) return;
    for (ifaddrs *a = list; a; a = a->ifa_next) {
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, name) == 0) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(a->ifa_addr)->sin_addr, out,
                      socklen_t(n));
            break;
        }
    }
    freeifaddrs(list);
}

// Send a command to wpa_supplicant for wlan0 and read its reply. Returns false if it
// doesn't answer.
bool wpa(const char *cmd, char *reply, size_t n) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un local = {}, remote = {};
    local.sun_family = remote.sun_family = AF_UNIX;
    snprintf(local.sun_path, sizeof local.sun_path, "/tmp/status-display-%d", int(getpid()));
    snprintf(remote.sun_path, sizeof remote.sun_path, "/run/wpa_supplicant/wlan0");
    unlink(local.sun_path);
    bool ok = false;
    if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof local) == 0 &&
        connect(fd, reinterpret_cast<sockaddr *>(&remote), sizeof remote) == 0 &&
        send(fd, cmd, strlen(cmd), 0) >= 0) {
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 500) > 0) {
            ssize_t got = recv(fd, reply, n - 1, 0);
            if (got > 0) {
                reply[got] = 0;
                ok = true;
            }
        }
    }
    close(fd);
    unlink(local.sun_path);
    return ok;
}

// A value from a "key=value" per line reply.
bool field(const char *reply, const char *key, char *out, size_t n) {
    size_t k = strlen(key);
    for (const char *line = reply; *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : "") {
        if (strncmp(line, key, k) == 0 && line[k] == '=') {
            const char *v = line + k + 1;
            size_t len = strcspn(v, "\n");
            if (len >= n) len = n - 1;
            memcpy(out, v, len);
            out[len] = 0;
            return true;
        }
    }
    return false;
}

void text(int x, int y, uint8_t size, uint16_t colour, const char *s) {
    canvas.setTextSize(size);
    canvas.setTextColor(colour);
    canvas.setCursor(x, y);
    canvas.print(s);
}

// Text centred across the screen.
void centred(int y, uint8_t size, uint16_t colour, const char *s) {
    text((W - int(strlen(s)) * 6 * size) / 2, y, size, colour, s);
}

// Four signal bars, from RSSI in dBm.
void bars(int x, int y, int rssi) {
    int level = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
    uint16_t colour = level >= 3 ? ST77XX_GREEN : level == 2 ? ST77XX_YELLOW : ST77XX_RED;
    for (int i = 0; i < 4; i++) {
        int h = 3 + i * 2;
        if (i < level) {
            canvas.fillRect(x + i * 4, y + 9 - h, 3, h, colour);
        } else {
            canvas.drawRect(x + i * 4, y + 9 - h, 3, h, 0x4208);  // dark grey outline
        }
    }
}

void draw() {
    char buf[64], value[48];
    canvas.fillScreen(ST77XX_BLACK);

    gethostname(buf, sizeof buf);
    text(2, 2, 1, ST77XX_CYAN, buf);
    long up = 0;
    if (FILE *f = fopen("/proc/uptime", "r")) {
        double s;
        if (fscanf(f, "%lf", &s) == 1) up = long(s);
        fclose(f);
    }
    if (up >= 86400) {
        snprintf(value, sizeof value, "up %ldd %ldh", up / 86400, up % 86400 / 3600);
    } else {
        snprintf(value, sizeof value, "up %ldh %02ldm", up / 3600, up % 3600 / 60);
    }
    text(W - 2 - int(strlen(value)) * 6, 2, 1, 0x8410, value);  // grey

    time_t now = time(nullptr);
    tm t;
    localtime_r(&now, &t);
    strftime(buf, sizeof buf, "%H:%M:%S", &t);
    centred(16, 2, ST77XX_WHITE, buf);
    strftime(buf, sizeof buf, "%a %e %b %Y", &t);
    centred(36, 1, ST77XX_WHITE, buf);
    canvas.drawFastHLine(0, 48, W, 0x4208);

    text(2, 53, 1, ST77XX_YELLOW, "WiFi");
    char reply[1024];
    if (wpa("STATUS", reply, sizeof reply) && field(reply, "wpa_state", value, sizeof value)) {
        if (strcmp(value, "COMPLETED") == 0 && field(reply, "ssid", buf, 22)) {
            text(2, 64, 1, ST77XX_WHITE, buf);
            if (wpa("SIGNAL_POLL", reply, sizeof reply) && field(reply, "RSSI", value, sizeof value)) {
                int rssi = atoi(value);
                bars(W - 18, 52, rssi);
                snprintf(buf, sizeof buf, "%d dBm", rssi);
                text(W - 22 - int(strlen(buf)) * 6, 53, 1, 0x8410, buf);
            }
        } else {
            text(2, 64, 1, ST77XX_RED, value);  // e.g. SCANNING, DISCONNECTED
        }
    } else {
        text(2, 64, 1, ST77XX_RED, "no wpa_supplicant");
    }
    ipv4("wlan0", buf, sizeof buf);
    text(2, 75, 1, ST77XX_GREEN, buf);
    canvas.drawFastHLine(0, 88, W, 0x4208);

    text(2, 93, 1, ST77XX_YELLOW, "USB");
    ipv4("usb0", buf, sizeof buf);
    text(2, 104, 1, ST77XX_GREEN, buf);
    text(2, 117, 1, 0x8410, "edison.local");
}

// Send the canvas to the display in one transfer: the controller takes each pixel most
// significant byte first, and transfer() overwrites its buffer, so send a converted copy.
void show() {
    static uint8_t frame[W * H * 2];
    const uint16_t *px = canvas.getBuffer();
    for (int i = 0; i < W * H; i++) {
        frame[2 * i] = uint8_t(px[i] >> 8);
        frame[2 * i + 1] = uint8_t(px[i]);
    }
    tft.startWrite();
    tft.setAddrWindow(0, 0, W, H);
    SPI.transfer(frame, sizeof frame);
    tft.endWrite();
}

void setup() {
    tft.initR(INITR_144GREENTAB);  // see SPIST7735 for other modules
}

void loop() {
    draw();
    show();
    // Redraw just after the next second starts, so the clock ticks evenly.
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    delay(1000 - ts.tv_nsec / 1000000 + 5);
}
