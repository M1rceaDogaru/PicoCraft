// PICOCRAFT -- an infinite 3D voxel sandbox for the Pimoroni PicoSystem (RP2040).
//
// True 3D: cubes projected through a real camera in fixed point, flat shaded per
// face normal, blended into distance fog and drawn back to front so voxels always
// overlap correctly. The world is generated on demand in chunks, so the island
// goes on forever in every direction. Rendering is capped at 60fps: the engine
// loop calls draw() once per ~16ms frame and we never skip any of them.
//
// Controls
//   dpad        walk forward/back and turn
//   A           jump (or swim up)
//   B           break the block at the crosshair (hold to keep digging)
//   X           place the selected block at the crosshair
//   Y tap       change which block you are placing
//   Y hold      aim mode: dpad looks around instead of walking

#include <math.h>

#include "picosystem.hpp"

using namespace picosystem;

// --------------------------------------------------------------- world ----
constexpr int CH = 16, CH_BITS = 4;    // a chunk is 16x16 columns wide
constexpr int WY = 20;                 // world height in blocks
constexpr int SEA = 6;                 // sea level
constexpr int CH_N = 5;                // chunk cache is a 5x5 torus around you
constexpr int CH_AREA = CH * CH;
constexpr int CHUNK_CELLS = CH_AREA * WY;

static uint8_t chunk_data[CH_N * CH_N][CHUNK_CELLS];   // the voxel bytes
static int32_t chunk_ox[CH_N * CH_N], chunk_oz[CH_N * CH_N];  // what is loaded

// floor division by the chunk size, correct for negative coordinates
static inline int chunk_of(int v) { return v >= 0 ? (v >> CH_BITS) : -(((-v + CH - 1) >> CH_BITS)); }
static inline int slot_of(int ccx, int ccz) {
  int mx = ((ccx % CH_N) + CH_N) % CH_N;
  int mz = ((ccz % CH_N) + CH_N) % CH_N;
  return mx * CH_N + mz;
}

// ------------------------------------------------------------- materials ---
enum material_t { AIR, GRASS, DIRT, STONE, SAND, WOOD, LEAF, GOLD, WATER, SNOW };
constexpr uint32_t MAT_COUNT = 10;

static const uint8_t mat_top[MAT_COUNT][3] = {
  {0, 0, 0},     {5, 12, 4},    {9, 6, 4},     {8, 8, 10},   {13, 12, 7},
  {8, 5, 2},     {4, 9, 3},     {15, 13, 3},   {3, 8, 15},   {14, 14, 15},
};
static const uint8_t mat_side[MAT_COUNT][3] = {
  {0, 0, 0},     {9, 6, 4},     {9, 6, 4},     {8, 8, 10},   {13, 12, 7},
  {8, 5, 2},     {4, 9, 3},     {15, 13, 3},   {3, 8, 15},   {14, 14, 14},
};

// the order blocks appear in the hotbar
static const uint8_t hotbar[MAT_COUNT - 2] = {GRASS, DIRT, STONE, SAND, WOOD, LEAF, SNOW, GOLD};

// face directions: 0:+X 1:-X 2:+Y(top) 3:-Y(bottom) 4:+Z 5:-Z
static const uint8_t face_light[6] = {13, 10, 16, 7, 11, 9};
static const int8_t ndx[6] = {1, -1, 0, 0, 0, 0};
static const int8_t ndy[6] = {0, 0, 1, -1, 0, 0};
static const int8_t ndz[6] = {0, 0, 0, 0, 1, -1};

// every corner of a face must lie in that face's own plane: +X at x=1, -X at
// x=0, tops at y=1, bottoms at y=0, +Z at z=1 and -Z at z=0
static const uint8_t fcx[6][4] = {
  {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}
};
static const uint8_t fcy[6][4] = {
  {0, 1, 1, 0}, {0, 1, 1, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 0, 1, 1}
};
static const uint8_t fcz[6][4] = {
  {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 1, 1, 0}, {0, 1, 1, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}
};

static inline bool opaque(uint8_t m) { return m != AIR && m != WATER; }

// ------------------------------------------------------------- world maths --
constexpr int Q = 1024;              // world fixed point scale (Q10)
constexpr int ANG_FULL = 2048;       // angle units in a full circle
constexpr int ANG_Q = ANG_FULL / 4;
static int16_t snlut[ANG_FULL];      // sine table, Q14

static inline int32_t sn(int a) { return snlut[a & (ANG_FULL - 1)]; }
static inline int32_t cs(int a) { return snlut[(a + ANG_Q) & (ANG_FULL - 1)]; }
static inline float fsn(int a) { return (float)sn(a) * (1.0f / 16384.0f); }
static inline float fcs(int a) { return (float)snlut[(a + ANG_Q) & (ANG_FULL - 1)] * (1.0f / 16384.0f); }

static inline int ifloor(float v) { int i = (int)v; return i - (v < (float)i ? 1 : 0); }
static inline float absf(float v) { return v < 0 ? -v : v; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

// --------------------------------------------------------------- terrain ---
static uint32_t h2(uint32_t x, uint32_t z) {
  uint32_t u = x * 73856079u + z * 19349663u;
  u ^= u >> 13; u *= 0x5bd1e995u; u ^= u >> 15;
  return u;
}

static inline int fdiv(int v, int s) { return v >= 0 ? v / s : -1 - ((-v - 1) / s); }

static int vnoise(int x, int z, int s) {
  int gx = fdiv(x, s), gz = fdiv(z, s);
  int tx = (x - gx * s) * 256 / s, tz = (z - gz * s) * 256 / s;
  uint32_t a = h2((uint32_t)gx, (uint32_t)gz) & 255, b = h2((uint32_t)(gx + 1), (uint32_t)gz) & 255;
  uint32_t c = h2((uint32_t)gx, (uint32_t)(gz + 1)) & 255, d = h2((uint32_t)(gx + 1), (uint32_t)(gz + 1)) & 255;
  int top = (int)a + (((int)b - (int)a) * tx >> 8);
  int bot = (int)c + (((int)d - (int)c) * tx >> 8);
  return top + ((bot - top) * tz >> 8);
}

// the surface height of a column: pure function, so chunks always agree
static int terrain_height(int x, int z) {
  int n = vnoise(x, z, 31) * 9 >> 8;
  n += vnoise(x + 91, z - 47, 13) * 6 >> 8;
  n += vnoise(x - 23, z + 57, 5) * 3 >> 8;
  int h = SEA - 2 + n;                       // 4 .. 20ish
  if(h > WY - 3) h = WY - 3;
  return h;
}

static bool tree_at(int x, int z) {
  return (h2((uint32_t)(x * 7 + 3), (uint32_t)(z * 11 + 5)) % 41u) == 0;
}

// write one block into the chunk that owns it, if that chunk is loaded
static void put_cell(int x, int y, int z, uint8_t m) {
  if(y < 0 || y >= WY) return;
  int ccx = chunk_of(x), ccz = chunk_of(z);
  uint32_t s = (uint32_t)slot_of(ccx, ccz);
  if(chunk_ox[s] != ccx || chunk_oz[s] != ccz) return;   // not loaded yet
  int lx = x - ccx * CH, lz = z - ccz * CH;
  chunk_data[s][(y * CH + lz) * CH + lx] = m;
}

// generate the chunk that owns (x,z): terrain columns plus trees whose canopy
// may reach in from a few blocks outside the chunk so seams stay seamless
static void gen_chunk(int ccx, int ccz) {
  uint32_t s = (uint32_t)slot_of(ccx, ccz);
  chunk_ox[s] = ccx; chunk_oz[s] = ccz;
  uint8_t *c = chunk_data[s];

  for(int lz = 0; lz < CH; lz++) {
    int z = ccz * CH + lz;
    for(int lx = 0; lx < CH; lx++) {
      int x = ccx * CH + lx;
      int h = terrain_height(x, z);
      bool beach = h <= SEA + 1;
      for(int y = 0; y < WY; y++) {
        uint8_t m = AIR;
        if(y < h) {
          if(y >= h - 1)      m = beach ? SAND : (h >= SEA + 9 ? SNOW : GRASS);
          else if(y >= h - 4) m = beach ? SAND : DIRT;
          else                m = STONE;
          if(m == STONE && y < 5 && (h2((uint32_t)(x * 31 + y), (uint32_t)(z * 17 + y)) % 89u) == 0) {
            m = GOLD;
          }
        } else if(y <= SEA) {
          m = WATER;
        }
        c[(uint32_t)y * CH_AREA + (uint32_t)(lz * CH + lx)] = m;
      }
    }
  }

  // trees, scanned a little wider than the chunk so canopies cross seams
  for(int lz = -3; lz < CH + 3; lz++) {
    int z = ccz * CH + lz;
    for(int lx = -3; lx < CH + 3; lx++) {
      int x = ccx * CH + lx;
      if(!tree_at(x, z)) continue;
      int h = terrain_height(x, z);
      if(h <= SEA + 1) continue;                       // no trees on beach/water
      int top = imin(WY - 1, h + 4 + (int)(h2((uint32_t)(z + 3), (uint32_t)(x + 5)) % 2u));
      for(int y = h; y <= top && y < WY; y++) put_cell(x, y, z, WOOD);
      for(int dy = -1; dy <= 1; dy++) {
        for(int dz = -2; dz <= 2; dz++) {
          for(int dx = -2; dx <= 2; dx++) {
            if(dx * dx + dz * dz > 3) continue;
            int yy = top + dy;
            if(yy < h || yy >= WY) continue;
            put_cell(x + dx, yy, z + dz, LEAF);
          }
        }
      }
    }
  }
}

// read a block, generating its chunk on demand
static uint8_t get_cell(int x, int y, int z) {
  if(y < 0) return STONE;              // bedrock: you can never fall out
  if(y >= WY) return AIR;
  int ccx = chunk_of(x), ccz = chunk_of(z);
  uint32_t s = (uint32_t)slot_of(ccx, ccz);
  if(chunk_ox[s] != ccx || chunk_oz[s] != ccz) gen_chunk(ccx, ccz);
  return chunk_data[s][((uint32_t)y * CH + (uint32_t)(z - ccz * CH)) * CH + (uint32_t)(x - ccx * CH)];
}

static inline bool solid_cell(int x, int y, int z) {
  uint8_t m = get_cell(x, y, z);
  return m != AIR && m != WATER;
}

// ------------------------------------------------------------------ player --
float px = 0.5f, py = 12.f, pz = 0.5f;   // feet position in blocks
float velx = 0, vely = 0, velz = 0;      // velocity in blocks/second
int yaw = 0, pitch = -25;               // camera angles

constexpr float PH = 1.67f;             // player height
constexpr float PR = 0.31f;             // half width
constexpr float GRAVITY = 15.f;         // blocks/s^2
constexpr float JUMP_V = 6.2f;          // jump impulse, ~1.28 blocks high
constexpr float WALK = 2.9f;            // walking speed
constexpr float SWIM = 1.7f;
constexpr int REACH = 4;                // block reach

uint8_t health = 6;                     // hearts
int16_t inventory[MAT_COUNT];           // blocks carried
uint32_t score = 0;                     // points from gold
uint32_t gold_mined = 0;                // how much gold has been dug up
uint8_t selected = GRASS;               // block type we place
float air_high = 0;                     // highest point since leaving ground
float breath = 10.f;                    // air left while swimming
bool grounded = false;
float spawn_x = 0.5f, spawn_y = 12.f, spawn_z = 0.5f;

const char *toast = nullptr;            // short message shown on screen
uint32_t toast_until = 0;

enum state_t { TITLE, PLAYING };
state_t state = TITLE;

// --------------------------------------------------------------- rendering --
constexpr int SW = SCREEN_WIDTH;
constexpr int SH = SCREEN_HEIGHT;
constexpr int FOCAL = (SW * 7) / 10;    // focal length in pixels
constexpr int CXS = SW / 2, CYS = SH / 2;

constexpr int RVIEW = 18;               // horizontal draw distance
constexpr int RVUP = 9;                 // vertical draw distance
constexpr int NEAR_Q24 = 1 << 22;       // near plane (quarter block)

// camera basis in Q14 and position in Q10
static int32_t fx, fy, fz, rx, ry, rz, ux, uy, uz;
static int32_t camx, camy, camz;

constexpr int BSHIFT = 22;              // dot(Q24) >> 22 == quarter blocks
constexpr int BUCKETS = (RVIEW * 2 + RVUP * 2 + 8) * 4;
constexpr uint32_t MAXCELLS = 5200;

// one packed entry per visible cell: [cell index 17 bits][bucket 8][faces 6]
static uint32_t cell_list[MAXCELLS];
static uint32_t sorted_list[MAXCELLS];
static uint16_t bucket_count[BUCKETS + 1];

static color_t sky_row[SH];             // sky gradient for this frame
static color_t haze_col;                // fog colour
static int light16 = 16;                // daylight multiplier, 0..16
static int daylight_q8 = 256;           // 0..256 sun brightness
static color_t *fb = nullptr;           // framebuffer pixels
uint32_t render_fps = 60;               // measured over rendered frames only

// the block we are pointing at, refreshed each rendered frame for the HUD
int tgt_valid = false;
int tx, ty, tz;

#ifdef HOST_TEST
uint32_t dbg_faces = 0;   // faces drawn last frame
uint32_t dbg_cells = 0;   // visible cells gathered last frame
float dbg_dt = 0;         // seconds per frame seen by update()
uint32_t dbg_colbad = 0;  // live column/heightmap disagreements
uint32_t dbg_tp = 0;      // frames that moved the player further than allowed
#endif

static inline color_t shade(uint8_t m, bool top, uint8_t lv) {
  const uint8_t *c = (top && m != WATER) ? mat_top[m] : mat_side[m];
  return rgb((uint8_t)((c[0] * lv) >> 4), (uint8_t)((c[1] * lv) >> 4),
             (uint8_t)((c[2] * lv) >> 4), 15);
}

// ------------------------------------------------------ camera and sky -----
static void update_camera_basis() {
  int32_t sy = sn(yaw), cy = cs(yaw), sp = sn(pitch), cp = cs(pitch);

  rx = cy; ry = 0; rz = -sy;
  fx = (cp * sy) >> 14; fy = sp; fz = (cp * cy) >> 14;
  ux = ((fy * rz) - (fz * ry)) >> 14;
  uy = ((fz * rx) - (fx * rz)) >> 14;
  uz = ((fx * ry) - (fy * rx)) >> 14;

  camx = (int32_t)(px * Q);
  camy = (int32_t)((py + PH - 0.08f) * Q);   // eye height
  camz = (int32_t)(pz * Q);
}

constexpr uint32_t DAY_MS = 180000;          // a whole day cycle in milliseconds
static int sun_angle = 0;                    // where the sun is, angle units

static void update_sky() {
  uint32_t t = time() % DAY_MS;
  sun_angle = (int)((t * (uint32_t)ANG_FULL) / DAY_MS);
  daylight_q8 = 96 + ((sn(sun_angle - ANG_Q / 2) + 16384) * 160 >> 14);
  if(daylight_q8 > 256) daylight_q8 = 256;

  light16 = (daylight_q8 + 8) >> 4;
  if(light16 < 4) light16 = 4;

  int m = daylight_q8 >> 4;
  if(m > 15) m = 15;

  color_t top = mix(rgb(2, 3, 9, 15), rgb(6, 11, 15, 15), (uint8_t)m);
  color_t hor = mix(rgb(3, 4, 8, 15), rgb(9, 12, 14, 15), (uint8_t)m);

  // underwater the whole world fades toward deep blue
  bool submerged = get_cell(ifloor(px), ifloor(py + PH - 0.1f), ifloor(pz)) == WATER;
  if(submerged) {
    top = rgb(1, 3, 9, 15);
    hor = rgb(2, 5, 11, 15);
    light16 = light16 > 8 ? 8 : light16;
  }
  haze_col = hor;

  for(int y = 0; y < SH; y++) {
    int t2 = (y * 30) / SH;
    if(t2 > 15) t2 = 15;
    sky_row[y] = mix(top, hor, (uint8_t)t2);
  }

  backlight(45 + (daylight_q8 >> 3));
}

// project a world point given in blocks relative to the camera. returns false
// when it is behind the near plane.
static float proj_depth;   // depth of the last projected point, in blocks

static bool project_point(float wx, float wy, float wz, int &sx, int &sy) {
  int32_t dxq = (int32_t)(wx * Q), dyq = (int32_t)(wy * Q), dzq = (int32_t)(wz * Q);
  int32_t d = fx * dxq + fy * dyq + fz * dzq;      // Q24
  if(d < NEAR_Q24) return false;
  proj_depth = (float)(d >> 20) * (1.f / 16.f);
  int32_t h = rx * dxq + ry * dyq + rz * dzq;
  int32_t v = ux * dxq + uy * dyq + uz * dzq;
  sx = CXS + (int32_t)(((h >> 8) * FOCAL) / (d >> 8));
  sy = CYS - (int32_t)(((v >> 8) * FOCAL) / (d >> 8));
  return true;
}

// ------------------------------------------------------------ rasterising --
static inline void fill_span(int y, int xa, int xb, color_t c) {
  if(y < 0 || y >= SH) return;
  if(xa < 0) xa = 0;
  if(xb >= SW) xb = SW - 1;
  if(xa > xb) return;
  color_t *d = fb + (size_t)y * SW + xa;
  for(int n = xb - xa + 1; n > 0; n--) *d++ = c;
}

static void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, color_t c) {
  if(y1 < y0) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if(y2 < y1) { int t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
  if(y1 < y0) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }

  int dy21 = y1 - y0, dy31 = y2 - y0, dy32 = y2 - y1;
  if(dy31 == 0) {
    fill_span(y0, imin(imin(x0, x1), x2), imax(imax(x0, x1), x2), c);
    return;
  }

  for(int y = y0; y <= y2; y++) {
    int xa = x0 + ((x2 - x0) * (y - y0)) / dy31;
    int xb;
    if(y < y1) xb = dy21 ? x0 + ((x1 - x0) * (y - y0)) / dy21 : x1;
    else       xb = dy32 ? x1 + ((x2 - x1) * (y - y1)) / dy32 : x2;
    if(xa > xb) { int t = xa; xa = xb; xb = t; }
    fill_span(y, xa, xb, c);
  }
}

// project one voxel face and draw it. yo is a vertical wobble for water.
static void emit_face(int x, int y, int z, uint8_t dir, color_t col, int yo) {
  int32_t sx[4], sy[4];
  int minx = 1 << 29, maxx = -(1 << 29), miny = 1 << 29, maxy = -(1 << 29);
  int32_t maxd = NEAR_Q24;   // if the whole face is behind us, skip it

  for(int k = 0; k < 4; k++) {
    int32_t wx = (int32_t)(x + fcx[dir][k]) * Q - camx;
    int32_t wy = (int32_t)(y + fcy[dir][k]) * Q + yo - camy;
    int32_t wz = (int32_t)(z + fcz[dir][k]) * Q - camz;

    int32_t d = fx * wx + fy * wy + fz * wz;      // view depth, Q24
    if(d > maxd) maxd = d;
    if(d < NEAR_Q24) d = NEAR_Q24;               // clamp rather than clip
    int32_t h = rx * wx + ry * wy + rz * wz;
    int32_t v = ux * wx + uy * wy + uz * wz;

    int px_ = CXS + (int32_t)(((h >> 8) * FOCAL) / (d >> 8));
    int py_ = CYS - (int32_t)(((v >> 8) * FOCAL) / (d >> 8));
    sx[k] = px_; sy[k] = py_;

    if(px_ < minx) minx = px_;
    if(px_ > maxx) maxx = px_;
    if(py_ < miny) miny = py_;
    if(py_ > maxy) maxy = py_;
  }

  if(maxd <= NEAR_Q24) return;                 // entirely behind the near plane
  if(maxx < 0 || minx >= SW || maxy < 0 || miny >= SH) return;

  fill_tri(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], col);
  fill_tri(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3], col);
}

// is face `dir` of cell (x,y,z) worth drawing?
static inline bool face_visible(int x, int y, int z, uint8_t m, uint8_t d) {
  uint8_t nb = get_cell(x + ndx[d], y + ndy[d], z + ndz[d]);
  if(m == WATER) {
    if(nb != AIR) return false;              // water only shows against air
  } else if(nb != AIR && nb != WATER) {
    return false;                            // neighbour hides the face
  }

  switch(d) {                                // backface test
    case 0: return camx > (int32_t)(x + 1) * Q;
    case 1: return camx < (int32_t)x * Q;
    case 2: return camy > (int32_t)(y + 1) * Q;
    case 3: return camy < (int32_t)y * Q;
    case 4: return camz > (int32_t)(z + 1) * Q;
    default: return camz < (int32_t)z * Q;
  }
}

// gather visible cells around the camera, sort them by view depth and draw far
// to near so overlapping voxels always look right
static void render_world() {
  uint32_t n = 0;
  for(int i = 0; i <= BUCKETS; i++) bucket_count[i] = 0;

  int cx = ifloor((float)camx / (float)Q), cy = ifloor((float)camy / (float)Q);
  int cz = ifloor((float)camz / (float)Q);

  int x0 = cx - RVIEW, y0 = imax(0, cy - RVUP), z0 = cz - RVIEW;
  int x1 = cx + RVIEW, y1 = imin(WY - 1, cy + RVUP), z1 = cz + RVIEW;

  uint32_t rmax = (uint32_t)(RVIEW * Q) * (RVIEW * Q);

  for(int y = y0; y <= y1; y++) {
    int32_t wy = (int32_t)y * Q + Q / 2 - camy;
    uint32_t wy2 = (uint32_t)(wy * wy);
    if(wy2 > rmax) continue;

    for(int z = z0; z <= z1; z++) {
      int32_t wz = (int32_t)z * Q + Q / 2 - camz;
      uint32_t wz2 = (uint32_t)(wz * wz);
      if(wy2 + wz2 > rmax) continue;

      for(int x = x0; x <= x1; x++) {
        int32_t wx = (int32_t)x * Q + Q / 2 - camx;
        uint32_t d2 = (uint32_t)(wx * wx) + wy2 + wz2;
        if(d2 > rmax) continue;

        uint8_t m = get_cell(x, y, z);
        if(m == AIR) continue;

        int bkt = (fx * wx + fy * wy + fz * wz) >> BSHIFT;   // quarter blocks
        if(bkt <= 0) continue;                              // behind the camera
        if(bkt >= BUCKETS) continue;                        // beyond draw distance

        uint8_t mask = 0;
        for(uint8_t d = 0; d < 6; d++) {
          if(face_visible(x, y, z, m, d)) mask |= (uint8_t)(1 << d);
        }
        if(!mask) continue;

        uint32_t packed = ((uint32_t)(x - x0) << 11) | ((uint32_t)(y - y0) << 6) |
                          (uint32_t)(z - z0);
        cell_list[n++] = (packed << 14) | ((uint32_t)bkt << 6) | mask;
        bucket_count[bkt + 1]++;
        if(n >= MAXCELLS) goto gathered;
      }
    }
  }
gathered:
#ifdef HOST_TEST
  dbg_cells = n;
#endif

  for(int i = 1; i <= BUCKETS; i++) bucket_count[i] += bucket_count[i - 1];
  for(uint32_t i = 0; i < n; i++) {
    uint32_t e = cell_list[i];
    int bkt = (int)((e >> 6) & 0xff);
    sorted_list[--bucket_count[bkt + 1]] = e;
  }

  uint32_t faces_drawn = 0;
  for(int b = BUCKETS - 1; b >= 0; b--) {
    uint32_t start = bucket_count[b], end = bucket_count[b + 1];
    for(uint32_t i = start; i < end; i++) {
      uint32_t e = sorted_list[i];
      uint8_t mask = (uint8_t)(e & 0x3f);
      uint32_t packed = e >> 14;

      int x = x0 + (int)(packed >> 11);
      int y = y0 + (int)((packed >> 6) & 31);
      int z = z0 + (int)(packed & 63);
      uint8_t m = get_cell(x, y, z);

      int32_t wx = (int32_t)x * Q + Q / 2 - camx;
      int32_t wy = (int32_t)y * Q + Q / 2 - camy;
      int32_t wz = (int32_t)z * Q + Q / 2 - camz;
      int depth_q4 = (fx * wx + fy * wy + fz * wz) >> BSHIFT;
      int fog = (depth_q4 - (RVIEW - 8) * 4) * 15 / 32;
      if(fog < 0) fog = 0;
      if(fog > 15) fog = 15;

      int yo = m == WATER ? (sn(((((x * 7 + z * 13) & 0xffffffu) << 6)) + (int)(time() >> 2)) >> 9) : 0;

      for(uint8_t d = 0; d < 6; d++) {
        if(!(mask & (1 << d))) continue;
        uint8_t lv = (uint8_t)(((uint32_t)face_light[d] * light16) >> 4);
        color_t col = shade(m, d == 2, lv);
        if(fog) col = mix(col, haze_col, (uint8_t)(15 - fog));
        emit_face(x, y, z, d, col, yo);
        faces_drawn++;
      }
    }
  }
#ifdef HOST_TEST
  dbg_faces = faces_drawn;
#endif
}
static void draw_sky_bodies() {
  // sun and moon: opposite ends of the same arc, high above the player
  float ex = (float)px, ey = py + 40.f, ez = (float)pz;   // sky dome centre

  for(int k = 0; k < 2; k++) {
    int a = sun_angle + (k ? ANG_Q / 2 : 0);
    float wx = ex + fcs(a) * 60.f, wy = ey + fsn(a) * 55.f, wz = ez + 30.f;
    int sx, sy;
    if(!project_point(wx - (float)px, wy - (py + PH), wz - pz, sx, sy)) continue;

    bool night = daylight_q8 < 140;
    uint8_t lv = night ? (uint8_t)(k == 1 ? 15 : 6) : (uint8_t)(k == 0 ? 15 : 6);
    color_t c = (k == 0) ? rgb(lv, (uint8_t)(lv * 9 / 10), (uint8_t)(lv / 3), 15)
                          : rgb((uint8_t)(lv / 2), (uint8_t)(lv / 2), lv, 15);
    pen(c);
    int r = night ? 4 : 6;
    fcircle(sx, sy, r);
  }

  // clouds drift on a coarse grid above the island so they parallax correctly
  if(daylight_q8 > 90) {
    uint8_t lv = (uint8_t)(12 - (256 - daylight_q8) / 32);
    pen(13, 13, 15, 14);
    int bx = ifloor(px), bz = ifloor(pz);
    for(int kx = -3; kx <= 3; kx++) {
      for(int kz = -3; kz <= 3; kz++) {
        int wx = bx + kx * 12, wz = bz + kz * 12;
        uint32_t h = h2((uint32_t)wx, (uint32_t)wz);
        if(h % 5u != 0) continue;                       // not every cell has a cloud
        int wy = WY + 6 + (int)(h % 4u);
        int sx, sy;
        if(!project_point((float)wx - px, (float)wy - py, (float)wz - pz, sx, sy)) continue;
        int w = imin(20, imax(3, (int)(180.f / proj_depth)));
        frect(sx - w, sy - 2, w * 2, 4);
        frect(sx - w / 2, sy - 4, w, 3);
      }
    }
  }

  // stars only show at night
  if(daylight_q8 < 150) {
    pen(14, 14, 15, 15);
    int bx = ifloor(px), bz = ifloor(pz);
    for(int kx = -4; kx <= 4; kx++) {
      for(int kz = -4; kz <= 4; kz++) {
        int wx = bx + kx * 9, wz = bz + kz * 9;
        uint32_t h = h2((uint32_t)(wx * 3 + 1), (uint32_t)(wz * 5 + 7));
        if(h % 3u != 0) continue;
        int wy = WY + 14 + (int)(h % 8u);
        int sx, sy;
        if(!project_point((float)wx - px, (float)wy - py, (float)wz - pz, sx, sy)) continue;
        pixel(sx, sy);
      }
    }
  }
}

// ---------------------------------------------------------- player physics --
static bool head_in_water() { return get_cell(ifloor(px), ifloor(py + PH - 0.1f), ifloor(pz)) == WATER; }
static bool feet_in_water() { return get_cell(ifloor(px), ifloor(py + 0.2f), ifloor(pz)) == WATER; }

// move the player along one axis and slide against anything solid it hits
static void move_axis(int axis, float delta) {
  if(delta == 0.f) return;

  float cur = axis == 0 ? px : (axis == 1 ? py : pz);
  float npos = cur + delta;
  float lim = npos;

  float x0 = axis == 0 ? npos - PR : px - PR;
  float x1 = axis == 0 ? npos + PR : px + PR;
  float y0 = axis == 1 ? npos : py;
  float y1 = axis == 1 ? npos + PH : py + PH;
  float z0 = axis == 2 ? npos - PR : pz - PR;
  float z1 = axis == 2 ? npos + PR : pz + PR;

  int ix0 = ifloor(x0), ix1 = ifloor(x1);
  int iy0 = ifloor(y0), iy1 = ifloor(y1 - 0.001f);
  int iz0 = ifloor(z0), iz1 = ifloor(z1 - 0.001f);

  bool blocked = false;
  bool stopped = false;   // a face in front of us actually stops this move
  for(int yy = iy0; yy <= iy1; yy++) {
    for(int zz = iz0; zz <= iz1; zz++) {
      for(int xx = ix0; xx <= ix1; xx++) {
        if(!solid_cell(xx, yy, zz)) continue;
        blocked = true;

        float cand;
        if(axis == 0)      cand = delta > 0 ? (float)xx - PR : (float)(xx + 1) + PR;
        else if(axis == 1) cand = delta > 0 ? (float)yy - PH : (float)(yy + 1);
        else               cand = delta > 0 ? (float)zz - PR : (float)(zz + 1) + PR;

        // Only faces ahead of the player may stop it. A face behind us (which
        // happens when we clip a corner) must never shove the player, or we
        // would jump several blocks in one frame.
        bool ahead = delta > 0 ? cand > cur : cand < cur;
        if(!ahead) continue;

        stopped = true;
        if(delta > 0) { if(cand < lim) lim = cand; }
        else          { if(cand > lim) lim = cand; }
      }
    }
  }
  // never slide past where we started, and refuse to move while embedded
  if(stopped) {
    if(delta > 0 && lim < cur) lim = cur;
    if(delta < 0 && lim > cur) lim = cur;
  } else if(blocked) {
    lim = cur;
  }

  if(axis == 0) { px = lim; if(blocked) velx = 0; }
  else if(axis == 1) {
    py = lim;
    if(blocked && delta < 0) grounded = true;   // we just landed
    if(blocked) vely = 0;
  } else { pz = lim; if(blocked) velz = 0; }
}

// ------------------------------------------------------------- targeting ---
static bool ray_from(float ox, float oy, float oz, int &hx, int &hy, int &hz,
                     int &nx, int &ny, int &nz) {
  float dx = (float)fx * (1.f / 16384.f);
  float dy = (float)fy * (1.f / 16384.f);
  float dz = (float)fz * (1.f / 16384.f);

  int x = ifloor(ox), y = ifloor(oy), z = ifloor(oz);
  int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;

  float adx = absf(dx), ady = absf(dy), adz = absf(dz);
  float tdx = adx > 1e-6f ? 1.f / adx : 1e9f;
  float tdy = ady > 1e-6f ? 1.f / ady : 1e9f;
  float tdz = adz > 1e-6f ? 1.f / adz : 1e9f;

  float tmx = (sx > 0 ? (float)(x + 1) - ox : ox - (float)x) * tdx;
  float tmy = (sy > 0 ? (float)(y + 1) - oy : oy - (float)y) * tdy;
  float tmz = (sz > 0 ? (float)(z + 1) - oz : oz - (float)z) * tdz;

  for(int i = 0; i < REACH * 4; i++) {
    int nnx = 0, nny = 0, nnz = 0;
    if(tmx < tmy && tmx < tmz)      { x += sx; tmx += tdx; nnx = -sx; }
    else if(tmy < tmz)              { y += sy; tmy += tdy; nny = -sy; }
    else                            { z += sz; tmz += tdz; nnz = -sz; }

    uint8_t m = get_cell(x, y, z);
    if(m != AIR && m != WATER) {
      hx = x; hy = y; hz = z; nx = nnx; ny = nny; nz = nnz;
      return true;
    }
  }
  return false;
}

// the block you are pointing at: centre first, then a small cross of parallel
// rays so d-pad aiming is forgiving rather than pixel perfect
static bool aim_block(int &hx, int &hy, int &hz, int &nx, int &ny, int &nz) {
  float ex = px, ey = py + PH - 0.08f, ez = pz;

  if(ray_from(ex, ey, ez, hx, hy, hz, nx, ny, nz)) return true;

  const float j = 0.16f;   // in blocks
  for(int k = 0; k < 4; k++) {
    float s = (k & 1) ? -j : j;
    float ar = (k < 2) ? s : 0.f;
    float au = (k < 2) ? 0.f : s;
    if(ray_from(ex + rx * (1.f / 16384.f) * ar + ux * (1.f / 16384.f) * au,
                ey + ry * (1.f / 16384.f) * ar + uy * (1.f / 16384.f) * au,
                ez + rz * (1.f / 16384.f) * ar + uz * (1.f / 16384.f) * au,
                hx, hy, hz, nx, ny, nz)) {
      return true;
    }
  }
  return false;
}

static bool overlaps_player(int cx, int cy, int cz) {
  return px + PR > (float)cx && px - PR < (float)(cx + 1) &&
         py + PH > (float)cy && py < (float)(cy + 1) &&
         pz + PR > (float)cz && pz - PR < (float)(cz + 1);
}

static void do_break() {
  int hx, hy, hz, nx, ny, nz;
  if(!aim_block(hx, hy, hz, nx, ny, nz)) return;

  uint8_t m = get_cell(hx, hy, hz);
  if(m == AIR || m == WATER) return;

  put_cell(hx, hy, hz, AIR);
  inventory[m]++;
  play(voice(10, 40, 30, 90, 0, 0, 0, 70, 2), 150 + (uint32_t)m * 25u, 90, 85);

  if(m == GOLD) {
    score += 10;
    gold_mined++;
    play(voice(30, 60, 60, 200, -140, 260, 220, 0, 0), note::E5, 260, 100);
    if(gold_mined % 10u == 0) {
      toast = "GOLD MINE";
      toast_until = time() + 2500;
    }
  }
}

static void do_place() {
  int hx, hy, hz, nx, ny, nz;
  if(!aim_block(hx, hy, hz, nx, ny, nz)) return;

  int tx_ = hx + nx, ty_ = hy + ny, tz_ = hz + nz;
  uint8_t here = get_cell(tx_, ty_, tz_);
  if(here != AIR && here != WATER) return;
  if(overlaps_player(tx_, ty_, tz_)) return;

  if(inventory[selected] <= 0) {
    play(voice(10, 30, 0, 60, 0, 0, 0, 20, 4), 120, 70, 60);
    toast = "OUT OF BLOCKS";
    toast_until = time() + 1500;
    return;
  }

  put_cell(tx_, ty_, tz_, selected);
  inventory[selected]--;
  play(voice(20, 30, 60, 80, 0, 0, 0, 40, 1), 220 + (uint32_t)selected * 20u, 70, 80);
}

// --------------------------------------------------------------- interface --
static uint8_t y_held = 0;           // frames Y has been held for
static bool aiming = false;          // aim mode (Y held down)
static float walk_phase = 0;         // head bob while walking

static void cycle_selected() {
  do {
    selected++;
    if(selected >= MAT_COUNT) selected = GRASS;
  } while(selected == AIR || selected == WATER);
}

// ------------------------------------------------------------------ title --
static void draw_title_cube() {
  int t = (int)(time() >> 6);
  yaw = t & (ANG_FULL - 1);
  pitch = -300;
  update_camera_basis();

  camx = (int32_t)((1.5f + fsn(yaw) * 2.4f) * Q);
  camy = (int32_t)(3.1f * Q);
  camz = (int32_t)((1.5f - fcs(yaw) * 2.4f) * Q);

  uint8_t faces[6];
  float fd[6];
  uint8_t n = 0;

  for(uint8_t d = 0; d < 6; d++) {
    float cx_ = 1.5f + (float)ndx[d] * 0.5f - (float)camx / (float)Q;
    float cy_ = 1.5f + (float)ndy[d] * 0.5f - (float)camy / (float)Q;
    float cz_ = 1.5f + (float)ndz[d] * 0.5f - (float)camz / (float)Q;

    // visible when the camera sits on the side the normal points to
    if((float)ndx[d] * cx_ + (float)ndy[d] * cy_ + (float)ndz[d] * cz_ >= 0.f) continue;

    fd[n] = (float)(fx * (int32_t)(cx_ * Q) + fy * (int32_t)(cy_ * Q) + fz * (int32_t)(cz_ * Q));
    faces[n++] = d;
  }

  for(uint8_t a = 0; a < n; a++) {
    for(uint8_t b = a + 1; b < n; b++) {
      if(fd[faces[a]] < fd[faces[b]]) { uint8_t t2 = faces[a]; faces[a] = faces[b]; faces[b] = t2; }
    }
  }

  for(uint8_t i = 0; i < n; i++) {
    uint8_t d = faces[i];
    uint8_t lv = (uint8_t)(((uint32_t)face_light[d] * light16) >> 4);
    emit_face(1, 1, 1, d, shade(GRASS, d == 2, lv), 0);
  }
}

// -------------------------------------------------------------------- hud --
static void draw_hud() {
  // hearts top left
  for(int i = 0; i < 6; i++) {
    bool full = (uint8_t)i < health;
    pen(full ? 13 : 4, full ? 3 : 1, full ? 3 : 2, 15);
    frect(2 + i * 5, 2, 4, 4);
  }

  // score and gold mined top right
  std::string shown = str(score);
  pen(15, 15, 10, 15);
  text(shown, SW - 8 - (int)shown.size() * 6, 2);
  pen(13, 11, 3, 15);
  std::string g = str(gold_mined);
  text(g, SW - 8 - (int)g.size() * 6, SH / 2 - 4);

  // hotbar: one slot per block type you can carry, selected one bracketed
  int hx0 = 2, hy0 = SH - 11;
  for(uint8_t i = 0; i < (uint8_t)(MAT_COUNT - 2); i++) {
    uint8_t m = hotbar[i];
    int sx_ = hx0 + i * 7;
    bool sel = (m == selected);
    int16_t have = inventory[m];

    uint8_t lv = have > 0 ? 14 : 5;               // dim the slots you cannot use
    pen(shade(m, true, lv)); frect(sx_, hy0, 5, 2);          // top face of the icon
    pen(shade(m, false, lv)); frect(sx_, hy0 + 2, 5, 4);     // front face

    if(sel) {
      pen(15, 15, 15, 15);
      rect(sx_ - 1, hy0 - 1, 7, 8);
    } else if(have > 0) {                          // stock gauge under the icon
      int w = have >= 32 ? 5 : (have * 5 / 32 + 1);
      pen(9, 12, 6, 15);
      frect(sx_, hy0 + 7, w, 1);
    }
  }

  // exact count of the selected block next to the hotbar
  pen(15, 15, 15, 15);
  std::string cnt = str((int32_t)inventory[selected]);
  text(cnt, hx0 + (int)(MAT_COUNT - 2) * 7 + 2, hy0 + 1);

  // crosshair: gold when something is in reach; also outline that block
  int hx_ = 0, hy_ = 0, hz_ = 0, nx_ = 0, ny_ = 0, nz_ = 0;
  bool hit = aim_block(hx_, hy_, hz_, nx_, ny_, nz_);
  tgt_valid = hit ? 1 : 0;
  if(hit) { tx = hx_; ty = hy_; tz = hz_; }

  pen(hit ? 15 : 12, hit ? 13 : 12, hit ? 3 : 12, 15);
  line(CXS - 3, CYS, CXS - 1, CYS);
  line(CXS + 1, CYS, CXS + 3, CYS);
  line(CXS, CYS - 3, CXS, CYS - 1);
  line(CXS, CYS + 1, CXS, CYS + 3);

  // outline the block you would break or build against
  if(hit) {
    int cx_[8] = {0, 1, 1, 0, 0, 1, 1, 0};
    int cy_[8] = {0, 0, 1, 1, 0, 0, 1, 1};
    int cz_[8] = {0, 0, 1, 1, 1, 1, 0, 0};
    int sx[8], sy[8];
    bool vis[8];
    for(int k = 0; k < 8; k++) {
      vis[k] = project_point((float)(tx + cx_[k]) - px,
                             (float)(ty + cy_[k]) - py,
                             (float)(tz + cz_[k]) - pz, sx[k], sy[k]);
    }
    static const int edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    pen(15, 15, 8, 12);
    for(int e = 0; e < 12; e++) {
      int a = edges[e][0], b = edges[e][1];
      if(vis[a] && vis[b]) line(sx[a], sy[a], sx[b], sy[b]);
    }
  }

  // aim mode indicator and measured render fps
  if(aiming) {
    pen(15, 6, 2, 15);
    text("AIM", CXS - 8, SH - 18);
  }
  pen(9, 12, 6, 15);
  std::string fps = str(render_fps);
  text(fps, SW - 6 - (int)fps.size() * 6, SH - 9);
}

static void draw_toast() {
  if(!toast || time() > toast_until) return;
  pen(15, 15, 8, 15);
  text(toast, (SW - (int)__builtin_strlen(toast) * 6) / 2, SH - 26);
}

// ------------------------------------------------------------------ update --
static uint32_t last_us = 0;

void init() {
  fb = SCREEN->data;

  // no chunk is loaded yet; zero would collide with real chunk coord (0,0)
  for(int i = 0; i < CH_N * CH_N; i++) {
    chunk_ox[i] = INT32_MIN;
    chunk_oz[i] = INT32_MIN;
  }

  for(int i = 0; i < ANG_FULL; i++) {
    snlut[i] = (int16_t)(sinf(2.0f * _PI * (float)i / (float)ANG_FULL) * 16384.0f);
  }

  // spawn on dry land near the origin of the infinite world
  int sx = 4, sz = 4;
  for(int r = 0; r < 24; r++) {
    if(terrain_height(sx, sz) > SEA + 1) break;
    sx += (r & 1) ? 3 : -2;
    sz += (r & 2) ? 2 : -3;
  }
  int sy = WY - 1;
  while(sy > 0 && terrain_height(sx, sz) <= sy) sy--;

  spawn_x = px = (float)sx + 0.5f;
  spawn_y = py = (float)sy + 1.f;
  spawn_z = pz = (float)sz + 0.5f;

  inventory[GRASS] = 12; inventory[DIRT] = 30; inventory[STONE] = 20;
  inventory[SAND] = 10;  inventory[WOOD] = 12;  inventory[LEAF] = 6;
  inventory[GOLD] = 0;   inventory[SNOW] = 4;

  state = TITLE;
}

void update(uint32_t tick) {
  uint32_t now = time_us();
  float dt = (float)(now - last_us) * (1.0f / 1000000.0f);
  last_us = now;
  if(dt < 0.004f) dt = 0.004f;
  if(dt > 0.05f) dt = 0.05f;

  // Y: tap changes block, hold aims
  if(button(Y)) {
    y_held++;
    aiming = y_held >= 8;
  } else {
    if(y_held > 0 && y_held < 8) cycle_selected();
    y_held = 0;
    aiming = false;
  }

  if(state == TITLE) {
    if(pressed(A)) {
      state = PLAYING;
      yaw = 0;
      pitch = -25;              // the title card spins its own camera
      play(voice(30, 60, 70, 240, -180, 320, 260, 0, 0), note::C4, 320, 100);
    }
    return;
  }

  if(health == 0) {             // respawn on the nearest spawn point
    px = spawn_x; py = spawn_y; pz = spawn_z;
    velx = vely = velz = 0;
    health = 6;
    toast = "YOU FELL!";
    toast_until = time() + 2500;
    return;
  }

  // turning and walking
  float walk_turn = (float)ANG_FULL * 0.47f;   // ~170 deg/s while walking
  float aim_turn = (float)ANG_FULL * 0.20f;    // ~72 deg/s when aiming
  float aim_pitch = (float)ANG_FULL * 0.13f;   // ~47 deg/s up/down
  if(aiming) {
    // standing still while aiming: without this the last walking frame's
    // momentum keeps sliding the player around
    velx = 0.f;
    velz = 0.f;
    if(button(RIGHT)) yaw += (int32_t)(aim_turn * dt);
    if(button(LEFT))  yaw -= (int32_t)(aim_turn * dt);
    if(button(UP))    pitch += (int32_t)(aim_pitch * dt);   // up means look up
    if(button(DOWN))  pitch -= (int32_t)(aim_pitch * dt);
    int lim = ANG_Q - 45;                    // look up to about +/-87 degrees
    if(pitch > lim) pitch = lim;
    if(pitch < -lim) pitch = -lim;
  } else {
    if(button(RIGHT)) yaw += (int32_t)(walk_turn * dt);
    if(button(LEFT))  yaw -= (int32_t)(walk_turn * dt);

    float fwd = button(UP) ? WALK : (button(DOWN) ? -WALK * 0.7f : 0.f);
    bool water = feet_in_water();
    if(water) fwd *= SWIM / WALK;

    velx = fsn(yaw) * fwd;
    velz = fcs(yaw) * fwd;
    if(fwd != 0.f && grounded) walk_phase += dt * (fwd > 0 ? 9.f : -9.f);
  }

  // digging and building, timed so the repeat rate is frame-rate independent
  static uint32_t dig_ms = 999, build_ms = 999;
  uint32_t dtms = (uint32_t)(dt * 1000.f);
  if(button(B)) {
    if(pressed(B) || dig_ms >= 150) { do_break(); dig_ms = 0; }
    dig_ms += dtms;
  } else dig_ms = 0;
  if(button(X)) {
    if(pressed(X) || build_ms >= 150) { do_place(); build_ms = 0; }
    build_ms += dtms;
  } else build_ms = 0;

  // jumping and swimming
  bool water = feet_in_water();
  if(water) {
    vely *= 0.86f;
    if(button(A)) vely = SWIM;
    if(head_in_water()) breath -= dt; else breath = 10.f;
    if(breath <= 0.f) {
      breath = 4.f;
      if(health > 0) health--;
      play(voice(10, 30, 20, 90, 0, 0, 0, 60, 3), 70, 140, 80);
    }
  } else {
    vely -= GRAVITY * dt;
    breath = 10.f;
    if(pressed(A) && grounded) {
      vely = JUMP_V;
      grounded = false;
    }
  }

  // integrate: horizontal first, then vertical
  bool was_grounded = grounded;
  grounded = false;
  move_axis(0, velx * dt);
  move_axis(2, velz * dt);
  move_axis(1, vely * dt);

  if(!grounded) {
    if(!was_grounded) air_high = py;              // stepped off a ledge
    else if(py > air_high) air_high = py;         // still climbing
  } else if(!was_grounded) {                      // landed: check the fall
    float fell = air_high - py;
    air_high = py;
    if(fell > 3.2f) {
      uint8_t dmg = (uint8_t)(fell - 2.4f);
      if(dmg > health) dmg = health;
      health -= dmg;
      play(voice(10, 40, 20, 120, 0, 0, 0, 70, 0), 90, 160, 100);
    }
  }

#ifdef HOST_TEST
  // live consistency: the chunks around the player must always agree with the
  // pure heightmap, otherwise walking would reveal seams or holes
  int cx = ifloor(px), cz = ifloor(pz);
  for(int x = cx - 4; x <= cx + 4; x++) {
    for(int z = cz - 4; z <= cz + 4; z++) {
      int h = terrain_height(x, z);
      uint8_t m = get_cell(x, h - 1, z);
      if(m == AIR || m == WATER) {
        dbg_colbad++;
        static bool once = false;
        if(!once) {
          once = true;
          int ccx = chunk_of(x), ccz = chunk_of(z);
          uint32_t s = (uint32_t)slot_of(ccx, ccz);
          printf("first bad column: x=%d z=%d th=%d got=%d | y0..y9:%d %d %d %d %d"
                 " slot=%u ox=%d oz=%d\n",
                 x, z, h, (int)m,
                 (int)get_cell(x, 0, z), (int)get_cell(x, 1, z), (int)get_cell(x, 2, z),
                 (int)get_cell(x, 3, z), (int)get_cell(x, 4, z), s,
                 chunk_ox[s], chunk_oz[s]);
          fflush(stdout);
        }
      }
    }
  }

  // teleport detector: a frame should never move the player further than its
  // own velocity allows; wall clipping used to shove it several blocks
  static float prev_px = 0, prev_py = 0, prev_pz = 0;
  static bool tp_started = false;
  if(!tp_started && state == PLAYING) {
    tp_started = true;   // first playing frame just seeds the reference point
    prev_px = px; prev_py = py; prev_pz = pz;
  } else {
    float mx = px - prev_px, my = py - prev_py, mz = pz - prev_pz;
    prev_px = px; prev_py = py; prev_pz = pz;
    if(state == PLAYING) {
    float moved = absf(mx) + absf(my) + absf(mz);
    float budget = (absf(velx) + absf(velz) + absf(vely)) * dt + 0.35f;
    if(moved > budget && moved > 1.f) {
      dbg_tp++;
      static bool once_tp = false;
      if(!once_tp) {
        once_tp = true;
        printf("first teleport: prev %.2f %.2f %.2f -> now %.2f %.2f %.2f (moved %.2f)\n",
               prev_px - mx, prev_py - my, prev_pz - mz, px, py, pz, moved);
        fflush(stdout);
      }
    }
    }
  }
#endif
}

// -------------------------------------------------------------------- draw --
constexpr uint8_t RENDER_SKIP = 1;   // draw every frame; the ~60Hz engine loop caps it
static uint8_t skip_count = 0;
static uint32_t last_render_us = 0;

void draw(uint32_t tick) {
  if(skip_count > 0) { skip_count--; return; }   // keep the previous picture
  skip_count = RENDER_SKIP - 1;

  uint32_t now = time_us();
  uint32_t delta = now - last_render_us;
  last_render_us = now;
  if(delta > 0) {
    uint32_t inst = (uint32_t)(1000000u / delta);
    render_fps = (render_fps * 7 + inst + 4) / 8;
  }

  update_sky();

  for(int y = 0; y < SH; y++) {
    color_t *d = fb + (size_t)y * SW;
    for(int x = 0; x < SW; x++) d[x] = sky_row[y];
  }

  if(state == TITLE) {
    draw_sky_bodies();
    draw_title_cube();
    pen(15, 15, 15, 15);
    text("PICOCRAFT", (SW - 54) / 2, 8);
    pen(9, 12, 6, 15);
    text("INFINITE 3D VOXEL WORLD", 6, SH - 30);
    pen(15, 15, 10, 15);
    text("DPAD WALK/TURN A JUMP\nB DIG X BUILD Y AIM", 6, SH - 22);
    if(((time() >> 9) & 1) == 0) {
      pen(15, 13, 3, 15);
      text("PRESS A", (SW - 42) / 2, SH - 40);
    }
    return;
  }

  update_camera_basis();
  // a gentle head bob while walking on the ground
  if(grounded && (velx != 0.f || velz != 0.f)) camy += (int32_t)(fsn((int)walk_phase * 512) * 8.f);

  draw_sky_bodies();
  render_world();
  draw_hud();
  draw_toast();
}

#ifdef HOST_TEST
#include <cstdio>
void host_setcam(int y_, int p_) { yaw = y_; pitch = p_; }
// sample columns far from the player and check that the chunks we generated
// agree with the pure heightmap: this is what keeps the world seamless
void host_check() {
  int bad = 0, checked = 0;
  for(int x = -71; x <= 71; x += 5) {
    for(int z = -71; z <= 71; z += 5) {
      int h = terrain_height(x, z);
      uint8_t m = get_cell(x, h - 1, z);
      if(m == AIR || m == WATER) bad++;
      checked++;
    }
  }
  printf("column check: %d sampled, %d mismatches\n", checked, bad);
  fflush(stdout);
  static const int probe[][2] = {{14,0},{15,0},{8,-3},{-1,-1},{-16,-1},{17,1}};
  for(auto &p : probe) {
    int h = terrain_height(p[0], p[1]);
    printf("probe x=%d z=%d th=%d y=h-1:%d y=0:%d\n", p[0], p[1], h,
           (int)get_cell(p[0], h - 1, p[1]), (int)get_cell(p[0], 0, p[1]));
  }
}
void host_report(int frame) {
  printf("frame %d: pos %.2f %.2f %.2f grounded=%d health=%u score=%u gold=%u "
         "inv[DIRT]=%d inv[GRASS]=%d yaw=%d pitch=%d state=%d faces=%u cells=%u fps=%u "
         "| th=%d below(y-1)=%d at=%d colbad=%u tp=%u\n",
         frame, (double)px, (double)py, (double)pz, (int)grounded,
         (unsigned)health, (unsigned)score, (unsigned)gold_mined,
         (int)inventory[DIRT], (int)inventory[GRASS], yaw, pitch, (int)state,
         (unsigned)dbg_faces, (unsigned)dbg_cells, (unsigned)render_fps,
         terrain_height(ifloor(px), ifloor(pz)),
         (int)get_cell(ifloor(px), ifloor(py) - 1, ifloor(pz)),
         (int)get_cell(ifloor(px), ifloor(py), ifloor(pz)), (unsigned)dbg_colbad, (unsigned)dbg_tp);
  fflush(stdout);
}
#endif
