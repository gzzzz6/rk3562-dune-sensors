#include "Alignment.hpp"
#include <cassert>
#include <limits>

using namespace Sensors::MFOSN;

int main()
{
  const Position reference{34.02678, 108.765755};
  assert(alignmentNmea(reference) ==
      "$GPGGA,134658.00,3401.6068,N,10845.9453,E,2,09,1.0,433.00,M,0.00,M,08,AAAA*5B\r\n"
      "$GPVTG,15.957,T,,M,0.0062,N,0.0015,K,A*32\r\n");
  assert(alignmentNmea({-90, -180}).find("9000.0000,S,18000.0000,W") != std::string::npos);
  assert(alignmentNmea({89.99999999, 179.99999999}).find("9000.0000,N,18000.0000,E") != std::string::npos);
  assert(alignmentNmea({90.000001, 0}).empty());
  assert(alignmentNmea({0, std::numeric_limits<double>::quiet_NaN()}).empty());
  assert(distance({0, -180}, {0, 180}) < 0.001);
  const Position previous{34, 108};
  Alignment alignment;
  auto start = [&](Position before, double t) {
    alignment.begin("test", reference, before, true, t);
    alignment.sent(t);
  };
  auto observe = [&](double first, int intervals) {
    for (int i = 0; i <= intervals; ++i)
      alignment.update(reference, true, first + i * 0.125);
  };
  start(previous, 0);
  observe(0.125, 4);
  alignment.update(reference, false, 0.75);
  observe(0.875, 4);
  observe(2, 7);
  assert(alignment.state == "waiting");
  alignment.update(reference, true, 3);
  assert(alignment.state == "position_matches");
  start(reference, 10);
  observe(10.125, 8);
  assert(alignment.state == "already_near");
  start(previous, 0);
  alignment.update(previous, true, 359.875);
  alignment.expire(360);
  assert(alignment.state == "position_unchanged");
  start(previous, 0);
  alignment.update(reference, false, 359.875);
  alignment.expire(360);
  assert(alignment.state == "readback_timeout");
  start(previous, 0);
  alignment.update({33, 108}, true, 359.875);
  alignment.expire(360);
  assert(alignment.state == "not_confirmed");
}
