#include "Protocol.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace Sensors::MFOSN;

static Frame fixture()
{
  Frame f{};
  f[0] = 0x99; f[1] = 0x66; f[2] = 0x6c;
  f[59] = 8; f[60] = 32;
  f[87] = 128; f[88] = 1;
  f[89] = 128; f[91] = 64; f[93] = 192; f[94] = 255;
  for (unsigned i = 2; i < 111; ++i)
    f[111] += f[i];
  return f;
}

int main()
{
  const Frame f = fixture();
  Frame out{};
  for (unsigned split = 0; split <= f.size(); ++split)
  {
    Parser p;
    unsigned hits = 0;
    for (unsigned i = 0; i < split; ++i) hits += p.feed(f[i], out);
    for (unsigned i = split; i < f.size(); ++i) hits += p.feed(f[i], out);
    assert(hits == 1 && out == f);
  }
  Parser p;
  for (unsigned i = 0; i < 10000; ++i)
    assert(!p.feed(0x99, out));
  assert(!p.feed(0x66, out) && !p.feed(0x00, out));
  auto corrupt = f;
  corrupt[56] ^= 1;
  for (auto b: corrupt) assert(!p.feed(b, out));
  unsigned hits = 0;
  for (unsigned i = 0; i < 2; ++i)
    for (auto b: f) hits += p.feed(b, out);
  assert(hits == 2 && p.badChecksums() == 1);
  Reading r = decode(out);
  assert(r.navigating() && r.fusingDvl());
  assert(std::abs(r.bottom_m - 384 * 256.0 / 32767) < 1e-12);
  assert(r.dvl[0] > 1 && r.dvl[1] > 0.5 && r.dvl[2] < -0.5);
  Frame extremes = f;
  extremes[14] = 128;
  assert(i32(extremes, 11) == -2147483648.0);
  DvlFreshness dvl;
  r.dvl_age_ms = 10;
  assert(!dvl.update(r, 0) && !dvl.fresh(0));
  assert(!dvl.update(r, 0.1));
  r.dvl_age_ms = 0;
  assert(dvl.update(r, 0.2) && dvl.fresh(0.2));
  assert(!dvl.update(r, 0.3) && dvl.fresh(0.3));
  assert(!dvl.update(r, 2.21) && !dvl.fresh(2.21));
  r.dvl_age_ms = 900;
  dvl.update(r, 3);
  r.dvl_age_ms = 0;
  assert(dvl.update(r, 3.1) && dvl.fresh(3.1));
  r.status_low = 8;
  assert(!dvl.update(r, 3.2) && !dvl.fresh(3.2));
  dvl.reset();
  assert(!dvl.fresh(3.3));
  std::cout << "MFOSN framing, wire values, and DVL freshness: passed\n";
}
