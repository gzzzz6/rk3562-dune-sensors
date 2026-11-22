// Distributed under the DUNE licence.
#ifndef SENSORS_MFOSN_ALIGNMENT_HPP
#define SENSORS_MFOSN_ALIGNMENT_HPP

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace Sensors { namespace MFOSN {
  struct Position { double lat, lon; };

  inline bool validPosition(Position p)
  {
    return std::isfinite(p.lat) && std::abs(p.lat) <= 90
      && std::isfinite(p.lon) && std::abs(p.lon) <= 180;
  }

  inline double distance(Position a, Position b)
  {
    const double rad = 3.14159265358979323846 / 180;
    const double x = std::sin((b.lat-a.lat)*rad/2);
    const double y = std::sin((b.lon-a.lon)*rad/2);
    const double h = x*x + std::cos(a.lat*rad)*std::cos(b.lat*rad)*y*y;
    return 12742000 * std::asin(std::sqrt(std::max(0.0, std::min(1.0, h))));
  }

  inline std::string coordinate(double value, int width)
  {
    const auto units = std::llround(std::abs(value) * 600000);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(width) << units / 600000
        << std::setw(2) << (units % 600000) / 10000 << '.'
        << std::setw(4) << units % 10000;
    return out.str();
  }

  inline std::string sentence(const std::string& body)
  {
    unsigned sum = 0;
    for (unsigned char c: body)
      sum ^= c;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << '$' << body << '*' << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << sum << "\r\n";
    return out.str();
  }

  inline std::string alignmentNmea(Position p)
  {
    if (!validPosition(p))
      return {};
    return sentence("GPGGA,134658.00," + coordinate(p.lat, 2) + (p.lat < 0 ? ",S," : ",N,")
        + coordinate(p.lon, 3) + (p.lon < 0 ? ",W," : ",E,")
        + "2,09,1.0,433.00,M,0.00,M,08,AAAA")
      + sentence("GPVTG,15.957,T,,M,0.0062,N,0.0015,K,A");
  }

  class Alignment
  {
  public:
    std::string id;
    std::string state = "idle";
    Position target{}, before{};
    bool alreadyNear = false;

    void begin(const std::string& request, Position goal, Position previous, bool previousValid, double now)
    {
      id = request; target = goal; before = previous;
      alreadyNear = previousValid && distance(previous, goal) <= 5;
      m_before_valid = previousValid;
      m_changed = false;
      m_started = now;
      m_near = m_last = -1;
      state = "writing";
    }

    void sent(double now) { m_started = now; state = "waiting"; }
    bool busy() const { return state == "writing" || state == "waiting"; }

    void update(Position p, bool navigating, double now)
    {
      expire(now);
      if (state != "waiting")
        return;
      if (m_last >= 0 && now - m_last >= 0.25)
        m_near = -1;
      if (!navigating || !validPosition(p))
      {
        m_near = -1;
        return;
      }
      m_last = now;
      if (m_before_valid && distance(before, p) > 1)
        m_changed = true;
      if (distance(p, target) > 5)
      {
        m_near = -1;
        return;
      }
      if (m_near < 0)
        m_near = now;
      else if (now - m_near >= 1)
        state = alreadyNear ? "already_near" : "position_matches";
    }

    void expire(double now)
    {
      if (state != "waiting" || now - m_started < 360)
        return;
      if (m_last < 0 || now - m_last >= 1)
        state = "readback_timeout";
      else if (m_before_valid && !m_changed)
        state = "position_unchanged";
      else
        state = "not_confirmed";
    }

  private:
    double m_started = 0, m_near = -1, m_last = -1;
    bool m_before_valid = false, m_changed = false;
  };
}}
#endif
