
#include <M5Unified.h>
#include <math.h>

// ====== TUNING KNOBS ======
static constexpr size_t N = 240;     // audio samples per frame (divisible by BARS)
static constexpr int BARS = 12;      // number of bars
static constexpr int SR = 18000;     // sample rate

static constexpr int GAP = 2;        // pixels between bars
static constexpr int SLICE = 4;      // vertical slice height for gradients (bigger = faster, less smooth)

static constexpr int PEAK_DROP_MS = 30; // peak-hold drop interval (ms)
static constexpr int BASE_DIM = 120;    // bottom brightness (0..255). Higher = brighter bottom
static constexpr int TOP_BOOST_DIV = 2; // brightness ramp: v = BASE_DIM + t / TOP_BOOST_DIV

// Rainbow-fill: where rainbow begins (0..255). 128 = starts halfway up bar
static constexpr int START_RAINBOW_AT = 50;

// ====== GLOBALS ======
static int16_t samples[N];

int barSmooth[BARS] = {0};
int peakHold[BARS]  = {0};
unsigned long lastDrop = 0;

// Themes (Button A cycles)
// 0 = GREEN solid, 1 = RED solid, 2 = Volume Gradient, 3 = Rainbow Fill (green -> yellow/orange/red)
uint8_t theme = 3;

// Sensitivity (Button B cycles)
int sensLevel = 1;  // 0=LOW, 1=MID, 2=HIGH
float sensMult[] = {0.70f, 1.00f, 1.50f};
unsigned long sensMsgUntil = 0;

// Simple HSV->RGB565
uint16_t hsvTo565(uint8_t h, uint8_t s, uint8_t v) {
  uint8_t region = h / 43;
  uint8_t remainder = (h - (region * 43)) * 6;

  uint8_t p = (uint16_t)v * (255 - s) / 255;
  uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * remainder / 255)) / 255;
  uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) / 255)) / 255;

  uint8_t r, g, b;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default:r = v; g = p; b = q; break;
  }
  return M5.Display.color565(r, g, b);
}

uint16_t solidThemeColor() {
  if (theme == 0) return GREEN;
  return RED; // theme == 1
}

void handleButtons() {
  M5.update();

  // Button A: cycle theme
  if (M5.BtnA.wasPressed()) {
    theme = (theme + 1) % 4;
  }

  // Button B: sensitivity LOW/MID/HIGH + 1s label
  if (M5.BtnB.wasPressed()) {
    sensLevel = (sensLevel + 1) % 3;
    sensMsgUntil = millis() + 1000;
  }
}
void bootAnimation(int H, int W, int barW, int gap) {
  // quick 800ms intro
  for (int t = 0; t <= 100; t += 5) {
    M5.Display.startWrite();

    // clear full screen during boot (only during boot, so no flicker worries)
    M5.Display.fillScreen(BLACK);

    for (int b = 0; b < BARS; b++) {
      int x = b * (barW + gap);

      // stagger the rise per bar for a wave effect
      int phase = (b * 6) % 30;
      int tt = t - phase;
      if (tt < 0) tt = 0;
      if (tt > 100) tt = 100;

      int h = (tt * H) / 100;
      int yTop = H - h;

      // use your current solid theme color for the boot
      uint16_t c = (theme == 0) ? GREEN : RED;
      M5.Display.fillRect(x, yTop, barW, h, c);
    }

    M5.Display.setTextColor(WHITE, BLACK);

M5.Display.setTextSize(2);
M5.Display.setCursor(8, 4);
M5.Display.print("S3 EQ");

M5.Display.setTextSize(1);
M5.Display.setCursor(28, 22);
M5.Display.print("v1.0");

    M5.Display.endWrite();
    delay(20);
  }

  // brief pause
  delay(150);
}

void setup() {
  M5.begin();
  M5.Speaker.end();   // mic + speaker can conflict on Stick S3
  M5.Mic.begin();

  M5.Display.setRotation(1);
  M5.Display.setBrightness(100);
  M5.Display.fillScreen(BLACK);
  int H = M5.Display.height();
int W = M5.Display.width();
int barW = (W - (BARS - 1) * GAP) / BARS;
if (barW < 4) barW = 4;

bootAnimation(H, W, barW, GAP);
M5.Display.fillScreen(BLACK);
}

void loop() {
  handleButtons();

  // Record audio
  if (!M5.Mic.record(samples, N, SR, false)) return;

  const int chunk = N / BARS;

  int H = M5.Display.height();
  int W = M5.Display.width();

  int barW = (W - (BARS - 1) * GAP) / BARS;
  if (barW < 4) barW = 4;

  // ---- Compute bars ----
  for (int b = 0; b < BARS; b++) {
    int32_t peak = 0;
    int start = b * chunk;
    int end = start + chunk;

    for (int i = start; i < end; i++) {
      int32_t v = samples[i];
      if (v < 0) v = -v;
      if (v > peak) peak = v;
    }

    // Compress a bit so it doesn't slam max constantly
    int32_t shaped = (int32_t)sqrt((double)peak) * 181;  // ~0..32767-ish

    // Apply sensitivity
    int h = (int)((shaped * sensMult[sensLevel] * H) / 32767.0f);
    if (h < 0) h = 0;
    if (h > H) h = H;

    // Smooth
    barSmooth[b] = (barSmooth[b] * 6 + h * 2) / 8;

    // Peak hold
    if (barSmooth[b] > peakHold[b]) peakHold[b] = barSmooth[b];
  }

  // Drop peak-hold slowly
  unsigned long now = millis();
  if (now - lastDrop > PEAK_DROP_MS) {
    for (int b = 0; b < BARS; b++) {
      if (peakHold[b] > 0) peakHold[b]--;
    }
    lastDrop = now;
  }

  // ---- Draw ----
  M5.Display.startWrite();

  for (int b = 0; b < BARS; b++) {
    int x = b * (barW + GAP);
    int h = barSmooth[b];
    int yTop = H - h;

    // Clear only this bar column (reduces flicker)
    M5.Display.fillRect(x, 0, barW, H, BLACK);

    if (theme == 2) {
      // Volume gradient: bottom dim -> top bright, hue varies by bar
      for (int yy = 0; yy < h; yy += SLICE) {
        int sliceH = (yy + SLICE <= h) ? SLICE : (h - yy);

        // t: 0 bottom -> 255 top
        int t = (h > 1) ? ((h - 1 - yy) * 255) / (h - 1) : 0;
        uint8_t v = (uint8_t)(BASE_DIM + (t / TOP_BOOST_DIV));

        uint8_t hue = (uint8_t)(b * (255 / BARS));
        uint16_t c = hsvTo565(hue, 255, v);

        M5.Display.fillRect(x, yTop + yy, barW, sliceH, c);
      }

    } else if (theme == 3) {
      // Rainbow fill: bottom mostly green, then rainbow begins ~halfway up
      for (int yy = 0; yy < h; yy += SLICE) {
        int sliceH = (yy + SLICE <= h) ? SLICE : (h - yy);

        // t: 0 bottom -> 255 top
        int t = (h > 1) ? ((h - 1 - yy) * 255) / (h - 1) : 0;

        uint8_t hue;
        if (t < START_RAINBOW_AT) {
          hue = 85; // green
        } else {
          // tt: 0..255 within rainbow portion
          int tt = (t - START_RAINBOW_AT) * 255 / (255 - START_RAINBOW_AT);

          // Green -> Yellow -> Orange -> Red (includes orange!)
          hue = (uint8_t)(85 - (tt * 85 / 255));
        }

        // Bottom dim -> top bright
        uint8_t v = (uint8_t)(BASE_DIM + (t / TOP_BOOST_DIV));

        uint16_t c = hsvTo565(hue, 255, v);
        M5.Display.fillRect(x, yTop + yy, barW, sliceH, c);
      }

    } else {
      // Solid color themes: GREEN / RED
      uint16_t c = solidThemeColor();
      M5.Display.fillRect(x, yTop, barW, h, c);
    }

    // Peak line
    int py = H - peakHold[b];
    M5.Display.fillRect(x, py, barW, 2, WHITE);
  }

  // Sensitivity label (1s). Clear area when not showing to avoid artifacts.
  if (millis() < sensMsgUntil) {
    M5.Display.setTextSize(1);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setCursor(2, 2);

    const char* label =
        (sensLevel == 0) ? "LOW" :
        (sensLevel == 1) ? "MID" : "HIGH";

    M5.Display.printf("SENS:%s", label);
  } else {
    M5.Display.fillRect(0, 0, 70, 14, BLACK);
  }

  M5.Display.endWrite();

  delay(20);
}