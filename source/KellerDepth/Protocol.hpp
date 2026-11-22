// KELLER X-Line bus V3.8 (2026-01), F48 and F73/P1 only.
// Same licence as DUNE (see LICENCE.md). No hardware-zero or configuration writes.
#ifndef SENSORS_KELLER_DEPTH_PROTOCOL_HPP
#define SENSORS_KELLER_DEPTH_PROTOCOL_HPP
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace Sensors { namespace KellerDepth {
inline uint16_t crc16(const uint8_t* bytes, size_t count)
{
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < count; ++i)
  {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
  }
  return crc;
}
inline std::vector<uint8_t> request(uint8_t address, uint8_t function)
{
  std::vector<uint8_t> out{address, function};
  if (function == 73) out.push_back(1); // Only P1; responses carry no channel id.
  else if (function != 48) throw std::invalid_argument("unsupported KELLER request");
  const uint16_t crc = crc16(out.data(), out.size());
  out.push_back(crc >> 8); out.push_back(crc & 255);
  return out;
}
struct Frame
{
  std::vector<uint8_t> bytes;
  bool exception() const { return bytes[1] & 0x80; }
};
// Sliding bounded parser, specific to the outstanding transaction. At most 10 bytes.
class Parser
{
  uint8_t m_address, m_function;
  std::vector<uint8_t> m_buffer;
public:
  Parser(uint8_t address, uint8_t function): m_address(address), m_function(function) { }
  size_t buffered() const { return m_buffer.size(); }
  bool feed(uint8_t byte, Frame& frame)
  {
    m_buffer.push_back(byte);
    while (!m_buffer.empty())
    {
      if (m_buffer[0] != m_address) { m_buffer.erase(m_buffer.begin()); continue; }
      if (m_buffer.size() < 2) return false;
      if (m_buffer[1] != m_function && m_buffer[1] != (m_function | 0x80))
      { m_buffer.erase(m_buffer.begin()); continue; }
      const size_t size = (m_buffer[1] & 0x80) ? 5 : (m_function == 48 ? 10 : 9);
      if (m_buffer.size() < size) return false;
      const uint16_t crc = crc16(m_buffer.data(), size - 2);
      if (m_buffer[size-2] != (crc >> 8) || m_buffer[size-1] != (crc & 255))
      { m_buffer.erase(m_buffer.begin()); continue; }
      frame.bytes.assign(m_buffer.begin(), m_buffer.begin() + size);
      m_buffer.erase(m_buffer.begin(), m_buffer.begin() + size);
      return true;
    }
    return false;
  }
};
inline bool initialized(const Frame& frame)
{
  const auto& b = frame.bytes;
  return b.size() == 10 && b[1] == 48 && b[2] == 5 &&
    (b[3] == 20 || b[3] == 21 || b[3] == 24) && b[7] <= 1;
}
struct Measurement
{
  uint8_t status;
  double bar;
  double pascal;
  double depth;
  bool valid;
};
inline Measurement measurement(const Frame& frame, double surface_bar, double density,
                               double gravity, double max_depth)
{
  Measurement result = {0, 0, 0, 0, false};
  if (frame.bytes.size() != 9 || frame.bytes[1] != 73) return result;
  const auto& b = frame.bytes;
  const uint32_t word = (uint32_t(b[2]) << 24) | (uint32_t(b[3]) << 16) |
    (uint32_t(b[4]) << 8) | b[5];
  float value; std::memcpy(&value, &word, sizeof(value));
  result.status = b[6]; result.bar = value; result.pascal = result.bar * 100000.0;
  if (!std::isfinite(surface_bar) || surface_bar < 0 ||
      !std::isfinite(density) || density <= 0 || !std::isfinite(gravity) || gravity <= 0 ||
      !std::isfinite(max_depth) || max_depth <= 0) return result;
  result.depth = std::max(0.0, (result.bar - surface_bar) * 100000.0 / (density * gravity));
  // Manual P1 mask 0x92 (/STD, TOB1, P1); also conservatively reject ERR2 (0x40).
  result.valid = !(result.status & 0xd2) && std::isfinite(result.bar) &&
    result.bar >= -1.0 && result.bar <= 2000 && result.depth <= max_depth;
  return result;
}
} }
#endif
