// Desktop test driver for PICOCRAFT: feeds scripted inputs, runs the game one
// frame at a time and dumps PNG frames so the 3D can be eyeballed.

#include "picosystem.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace picosystem;

extern void host_report(int frame);
extern void host_setcam(int y_, int p_);
extern void host_check();

static bool g_walk = false;   // endless-walk test mode

static uint64_t sim_us = 0;   // virtual clock in microseconds

namespace picosystem {
uint32_t time() { return (uint32_t)(sim_us / 1000); }
uint32_t time_us() { return (uint32_t)sim_us; }
}

// which buttons are down on a given frame: bit i == button (16 + i)
static uint32_t input_for(int frame) {
  if(g_walk) {
    // endless walk: forward all the time, hop over obstacles so hills climb
    uint32_t io = 0;
    if(frame >= 4 && frame < 6) io |= (1u << (A - 16));       // leave title
    if(frame > 8) io |= (1u << (UP - 16));
    if(frame % 13 < 2) io |= (1u << (A - 16));                 // hop constantly
    if(frame % 500 >= 470 && frame % 500 < 490) io |= (1u << (RIGHT - 16));
    return io;
  }

  uint32_t io = 0;
  auto hold = [&](int lo, int hi, int b) {
    if(frame >= lo && frame < hi) io |= (1u << (b - 16));
  };

  // start the game shortly after boot
  if(frame == 45 || frame == 46) io |= (1u << (A - 16));

  // walk forward, turn right, keep walking
  hold(60, 200, UP);
  hold(200, 260, RIGHT);
  hold(260, 420, UP);
  // dig down under the player
  if(frame >= 430 && frame < 520) io |= (1u << (B - 16));
  // jump
  if(frame == 540 || frame == 541) io |= (1u << (A - 16));
  // aim mode: hold Y, look up, then look back down and place blocks ahead
  hold(600, 760, Y);
  hold(620, 680, UP);
  hold(690, 740, DOWN);
  if(frame >= 750 && frame < 800) io |= (1u << (X - 16));

  return io;
}

static void dump_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  if(!f) return;
  fprintf(f, "P6\n%d %d\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
  for(int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
    color_t c = SCREEN->data[i];
    unsigned char px_[3] = {
      (unsigned char)(((c >> 0) & 0xf) * 17),
      (unsigned char)(((c >> 12) & 0xf) * 17),
      (unsigned char)(((c >> 8) & 0xf) * 17)};
    fwrite(px_, 3, 1, f);
  }
  fclose(f);
}

static void profile_column() {
  // print the centre column top to bottom so we can check the horizon
  for(int y = 0; y < SCREEN_HEIGHT; y += 4) {
    color_t c = SCREEN->data[(y * SCREEN_WIDTH) + SCREEN_WIDTH / 2];
    int r = (c >> 0) & 0xf, g = (c >> 12) & 0xf, b = (c >> 8) & 0xf;
    char ch;
    if(b > 10 && g > 7 && r < 9) ch = 'S';             // bright sky
    else if(g >= r && g >= b) ch = 'G';                // vegetation tops
    else if(r >= g && g >= b) ch = 'E';                // earth / stone
    else if(b > r) ch = 'W';                           // water or night sky
    else ch = '?';
    putchar(ch);
  }
  putchar('\n');
}

int main(int argc, char **argv) {
  int frames = argc > 1 ? atoi(argv[1]) : 800;
  bool profile_mode = argc > 2 && strcmp(argv[2], "profile") == 0;
  g_walk = argc > 2 && strcmp(argv[2], "walk") == 0;

  sim_us = 120000;   // start at midday so the test pictures are lit
  init();

  for(int frame = 0; frame < frames; frame++) {
    _lio = _io;
    _io = input_for(frame);
    if(profile_mode && frame == 70) host_setcam(0, 0);
    if(!profile_mode && !g_walk && frame == 70) host_setcam(512, -140);
    if(g_walk && frame == 3650) host_setcam(1024, 430);   // night: stare at the sky

    update((uint32_t)frame);
    draw((uint32_t)frame);

    if(profile_mode) {
      if(frame == 120 || frame == 200) profile_column();
    } else if(frame % 60 == 0 || frame == 100) {
      char path[64];
      snprintf(path, sizeof(path), "/tmp/opencode/tc/host/f%04d.ppm", frame);
      dump_ppm(path);
    }

    sim_us += 16667;   // pretend we are running at 60fps

    if(frame % 120 == 0) host_report(frame);
    // trace the aim rate so sensitivity can be checked
    if(!profile_mode && frame >= 600 && frame <= 700 && frame % 10 == 0) {
      host_report(frame);
    }
  }

  host_report(frames - 1);
  if(g_walk) host_check();
  return 0;
}
