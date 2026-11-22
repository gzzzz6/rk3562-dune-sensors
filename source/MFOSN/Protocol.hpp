// Distributed under the DUNE licence. mFOSN AQUAGNR1_B.
#ifndef SENSORS_MFOSN_PROTOCOL_HPP
#define SENSORS_MFOSN_PROTOCOL_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Sensors
{
  namespace MFOSN
  {
    using Frame = std::array<uint8_t, 112>;

    inline uint32_t u32(const Frame& f, std::size_t i)
    {
      return uint32_t(f[i]) | (uint32_t(f[i + 1]) << 8)
        | (uint32_t(f[i + 2]) << 16) | (uint32_t(f[i + 3]) << 24);
    }

    inline double i32(const Frame& f, std::size_t i)
    {
      const uint32_t n = u32(f, i);
      return n < 0x80000000u ? double(n) : double(n) - 4294967296.0;
    }

    inline int i16(const Frame& f, std::size_t i)
    {
      const int n = int(f[i]) + 256 * int(f[i + 1]);
      return n < 32768 ? n : n - 65536;
    }

    class Parser
    {
    public:
      bool feed(uint8_t byte, Frame& frame)
      {
        m_buffer[m_used++] = byte;
        for (;;)
        {
          if (!m_used || (m_buffer[0] == 0x99 && m_used == 1))
            return false;
          if (m_buffer[0] == 0x99 && m_buffer[1] == 0x66)
          {
            if (m_used == 2)
              return false;
            if (m_buffer[2] == 0x6c)
            {
              if (m_used < m_buffer.size())
                return false;
              uint8_t sum = 0;
              for (std::size_t i = 2; i < 111; ++i)
                sum = uint8_t(sum + m_buffer[i]);
              if (sum == m_buffer[111])
              {
                frame = m_buffer;
                m_used = 0;
                return true;
              }
              ++m_bad;
            }
          }
          std::move(m_buffer.begin() + 1, m_buffer.begin() + m_used, m_buffer.begin());
          --m_used;
        }
      }

      void reset() { m_used = 0; }
      unsigned badChecksums() const { return m_bad; }

    private:
      Frame m_buffer{};
      std::size_t m_used = 0;
      unsigned m_bad = 0;
    };

    struct Reading
    {
      uint32_t ticks, utc_ms, status_low, status_high, dvl_age_ms;
      double latitude_deg, longitude_deg, elevation_m, roll_rad, pitch_rad, yaw_rad;
      double north_mps, east_mps, down_mps, bottom_m;
      // Keep the device order (forward, up, right) for diagnostics.
      std::array<double, 3> dvl;

      bool navigating() const { return (status_low & ((1u << 3) | (1u << 5))) != 0; }
      bool fusingDvl() const { return (status_low & (1u << 13)) != 0; }
    };

    inline Reading decode(const Frame& f)
    {
      const double angle = 3.14159265358979323846 / 32767.0;
      const double pos = 180.0 / 2147483647.0;
      const double vel = 256.0 / 32767.0;
      return Reading{
        u32(f, 3), u32(f, 7), u32(f, 59), u32(f, 63), u32(f, 107),
        i32(f, 11) * pos, i32(f, 19) * pos, i32(f, 15) * (32767.0 / 2147483647.0),
        i16(f, 29) * angle, i16(f, 33) * angle, -i16(f, 31) * angle,
        i16(f, 23) * vel, i16(f, 27) * vel, -i16(f, 25) * vel, i16(f, 87) * vel,
        {{i16(f, 89) * vel, i16(f, 91) * vel, i16(f, 93) * vel}}
      };
    }

    // Use only with a confirmed bottom-track source and confirmed age-counter semantics.
    class DvlFreshness
    {
    public:
      bool update(const Reading& r, double now)
      {
        const bool new_sample = m_seen && (r.dvl_age_ms < m_age)
          && (r.dvl_age_ms <= 2000) && r.fusingDvl() && r.bottom_m > 0
          && r.navigating();
        m_seen = true;
        m_age = r.dvl_age_ms;
        if (!r.navigating() || !r.fusingDvl() || r.bottom_m <= 0 || m_age > 2000)
          m_valid = false;
        if (new_sample)
        {
          m_valid = true;
          m_observation = now - m_age / 1000.0;
        }
        return new_sample;
      }

      bool fresh(double now) const { return m_valid && now - m_observation < 2.0; }
      void reset() { m_seen = m_valid = false; }

    private:
      uint32_t m_age = 0;
      double m_observation = 0;
      bool m_seen = false;
      bool m_valid = false;
    };
  }
}
#endif
