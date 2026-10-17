// Host stand-in for the Pimoroni PicoSystem API, used only to test game logic
// on a desktop machine. The real build uses pimoroni's picosystem.hpp.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <memory>

#define SCREEN_WIDTH 120
#define SCREEN_HEIGHT 120

void init();
void update(uint32_t tick);
void draw(uint32_t tick);

namespace picosystem {

typedef uint16_t color_t;
constexpr float _PI = 3.1415927f;

struct stat_t { uint32_t fps, idle, tick_us, update_us, draw_us, flip_us; };
inline stat_t stats{60, 0, 0, 0, 0, 0};

// ------------------------------------------------------------- framebuffer --
struct buffer_t {
  int32_t w, h;
  color_t *data;
  bool alloc;
  color_t *p(int32_t x, int32_t y) { return data + (x + y * w); }
};

inline color_t _fbmem[SCREEN_WIDTH * SCREEN_HEIGHT];
inline buffer_t _SCREENBUF{SCREEN_WIDTH, SCREEN_HEIGHT, _fbmem, false};
inline buffer_t *SCREEN = &_SCREENBUF;
inline buffer_t *_dt = SCREEN;

// ------------------------------------------------------------------- state --
inline color_t _pen = 0xffff;
inline uint8_t _a = 15;
inline int32_t _cx = 0, _cy = 0, _cw = SCREEN_WIDTH, _ch = SCREEN_HEIGHT;
inline int32_t _tx = 0, _ty = 0, _tlh = 8, _tls = 1, _tlw = -1;

inline color_t rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 15) {
  return (uint16_t)((r & 0xf) | ((a & 0xf) << 4) | ((b & 0xf) << 8) | ((g & 0xf) << 12));
}

inline color_t mix(color_t c1, color_t c2, uint8_t m) {
  auto ch = [](color_t c, int shift) { return (int)((c >> shift) & 0xf); };
  int r = (ch(c1, 0) * m + ch(c2, 0) * (15 - m)) / 15;
  int b = (ch(c1, 8) * m + ch(c2, 8) * (15 - m)) / 15;
  int g = (ch(c1, 12) * m + ch(c2, 12) * (15 - m)) / 15;
  return rgb((uint8_t)r, (uint8_t)g, (uint8_t)b, 15);
}

inline void pen() { _pen = rgb(15, 15, 15, 15); }
inline void pen(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { _pen = rgb(r, g, b, a); }
inline void pen(color_t p) { _pen = p; }
inline void alpha() {}
inline void alpha(uint8_t a) { _a = a; }
inline void clip() {}
inline void blend() {}
inline void target() {}
inline void camera() {}
inline void cursor() {}

inline bool inb(int x, int y) { return x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT; }

inline void clear() {
  for(int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) _fbmem[i] = _pen;
}
inline void frect(int32_t x, int32_t y, int32_t w, int32_t h) {
  for(int j = 0; j < h; j++)
    for(int i = 0; i < w; i++)
      if(inb(x + i, y + j)) _fbmem[(y + j) * SCREEN_WIDTH + x + i] = _pen;
}
inline void line(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
  int dx = abs(x2 - x1), dy = -abs(y2 - y1);
  int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1;
  int err = dx + dy;
  for(;;) {
    if(inb(x1, y1)) _fbmem[y1 * SCREEN_WIDTH + x1] = _pen;
    if(x1 == x2 && y1 == y2) break;
    int e2 = 2 * err;
    if(e2 >= dy) { err += dy; x1 += sx; }
    if(e2 <= dx) { err += dx; y1 += sy; }
  }
}

inline void rect(int32_t x, int32_t y, int32_t w, int32_t h) {
  for(int32_t i = 0; i < w; i++) { if(inb(x + i, y)) _fbmem[y * SCREEN_WIDTH + x + i] = _pen;
    if(inb(x + i, y + h - 1)) _fbmem[(y + h - 1) * SCREEN_WIDTH + x + i] = _pen; }
  for(int32_t j = 0; j < h; j++) { if(inb(x, y + j)) _fbmem[(y + j) * SCREEN_WIDTH + x] = _pen;
    if(inb(x + w - 1, y + j)) _fbmem[(y + j) * SCREEN_WIDTH + x + w - 1] = _pen; }
}
inline void pixel(int32_t x, int32_t y) {
  if(inb(x, y)) _fbmem[y * SCREEN_WIDTH + x] = _pen;
}
inline void fcircle(int32_t cx_, int32_t cy_, int32_t r) {
  for(int32_t j = -r; j <= r; j++)
    for(int32_t i = -r; i <= r; i++)
      if(i * i + j * j <= r * r && inb(cx_ + i, cy_ + j)) _fbmem[(cy_ + j) * SCREEN_WIDTH + cx_ + i] = _pen;
}

// text is faked as blocks so the test can see where it lands
inline void text(const std::string_view &t, int32_t x, int32_t y, int32_t wrap = -1) {
  for(size_t i = 0; i < t.size(); i++) {
    if(t[i] == '\n') continue;
    frect(x + (int32_t)i * 6, y, 5, 7);
  }
}
inline void text(const std::string_view &t, int32_t wrap = -1) {}

inline std::string str(int32_t v) { return std::to_string(v); }
inline std::string str(uint32_t v) { return std::to_string((uint64_t)v); }
inline std::string str(std::size_t v) { return std::to_string((uint64_t)v); }
inline std::string str(float v, uint8_t precision = 2) { return std::to_string(v); }

// ------------------------------------------------------------------ input --
enum button { UP = 23, DOWN = 20, LEFT = 22, RIGHT = 21, A = 18, B = 19, X = 17, Y = 16 };
inline uint32_t _io = 0, _lio = 0;

inline bool button(uint32_t b) { return (_io >> (b - 16)) & 1u; }
inline bool pressed(uint32_t b) { return ((_io & ~_lio) >> (b - 16)) & 1u; }
inline bool released(uint32_t b) { return ((~_io & _lio) >> (b - 16)) & 1u; }

inline void backlight(uint8_t b) {}
inline uint32_t battery() { return 40; }

// ------------------------------------------------------------------- audio --
struct voice_t { int32_t bend; uint32_t bend_ms, attack, decay, sustain, release, reverb, noise, distort; };
inline voice_t voice(uint32_t attack = 100, uint32_t decay = 50, uint32_t sustain = 80,
                     uint32_t release = 100, int32_t bend = 0, uint32_t bend_ms = 0,
                     uint32_t reverb = 0, uint32_t noise = 0, uint32_t distort = 0) {
  return voice_t{bend, bend_ms, attack, decay, sustain, release, reverb, noise, distort};
}
inline void play(voice_t v, uint32_t frequency, uint32_t duration = 500, uint32_t volume = 100) {}

enum note { _ = 0, C4 = 261, E5 = 659 };

// ----------------------------------------------------------------- utility --
uint32_t time();      // milliseconds since start
uint32_t time_us();   // microseconds since start

}   // namespace picosystem
