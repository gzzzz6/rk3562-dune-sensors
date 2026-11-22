// Same licence as DUNE (see LICENCE.md).
// Dedicated KELLER X-Line P1 driver; legacy Sensors.Keller is unchanged.
#include <DUNE/DUNE.hpp>
#include "Protocol.hpp"
#include <iomanip>

namespace Sensors { namespace KellerDepth {
using namespace DUNE;
using DUNE_NAMESPACES;
struct Arguments
{
  std::string device;
  unsigned baud, address;
  bool echo;
  double period, response_timeout, density, gravity, surface, max_depth;
};
struct Task: public Tasks::Task
{
  Arguments m_args;
  Hardware::SerialPort* m_port;
  bool m_reopen;
  double m_next_connect, m_next_sample, m_last_valid, m_last_state;
  std::string m_state;
  Task(const std::string& name, Tasks::Context& ctx): Tasks::Task(name, ctx),
    m_port(nullptr), m_reopen(false), m_next_connect(0), m_next_sample(0),
    m_last_valid(0), m_last_state(0)
  {
    paramActive(Tasks::Parameter::SCOPE_GLOBAL, Tasks::Parameter::VISIBILITY_USER, true);
    param("Serial Port - Device", m_args.device).defaultValue("").description("RS485 adapter device path; no TCP or automatic port probing");
    param("Serial Port - Baud Rate", m_args.baud).defaultValue("9600").values("9600, 115200");
    param("Serial Port - Local Echo", m_args.echo).defaultValue("false");
    param("Device Address", m_args.address).defaultValue("1").minimumValue("1").maximumValue("249");
    param("Sampling Period", m_args.period).defaultValue("0.2").minimumValue("0.2").maximumValue("10");
    param("Response Timeout", m_args.response_timeout).defaultValue("0.25").minimumValue("0.15").maximumValue("1");
    param("Surface Pressure", m_args.surface).defaultValue("-1").description("P1 at surface in bar; explicitly required, 0 for gauge only after confirmation");
    param("Water Density", m_args.density).defaultValue("1025").minimumValue("900").maximumValue("1300");
    param("Gravity", m_args.gravity).defaultValue("9.80665").minimumValue("9").maximumValue("10");
    param("Maximum Depth", m_args.max_depth).defaultValue("11000").minimumValue("1").maximumValue("11000");
  }
  void onUpdateParameters() override { m_reopen = true; }
  void onResourceInitialization() override
  {
    if (m_args.device.empty() || !std::isfinite(m_args.surface) || m_args.surface < 0)
      throw std::runtime_error("explicit serial device and Surface Pressure (bar) required");
  }
  void onResourceRelease() override { Memory::clear(m_port); }
  void state(const std::string& text, bool good = false)
  {
    const double now = Time::Clock::get();
    if (m_state != text || now - m_last_state >= 1)
    {
      setEntityState(good ? IMC::EntityState::ESTA_NORMAL : IMC::EntityState::ESTA_ERROR, text);
      if (m_state != text) inf("%s", text.c_str());
      m_state = text; m_last_state = now;
    }
  }
  void raw(const char* direction, const std::vector<uint8_t>& bytes)
  {
    std::ostringstream out;
    out << direction << " ";
    for (uint8_t byte: bytes) out << std::hex << std::setw(2) << std::setfill('0') << unsigned(byte);
    IMC::DevDataText msg; msg.value = out.str(); dispatch(msg);
  }
  bool transact(uint8_t function, Frame& frame)
  {
    const auto tx = request(m_args.address, function);
    Parser parser(m_args.address, function);
    // One outstanding request. Drain stale data before sending; a timeout closes the link.
    m_port->flush();
    if (m_port->write(tx.data(), tx.size()) != tx.size()) throw std::runtime_error("short serial write");
    raw("TX", tx);
    const double deadline = Time::Clock::get() + m_args.response_timeout;
    size_t echo = 0;
    while (!isStopping() && Time::Clock::get() < deadline)
    {
      if (!IO::Poll::poll(*m_port, std::min(0.02, std::max(0.0, deadline-Time::Clock::get())))) continue;
      uint8_t buf[128]; const size_t count = m_port->read(buf, sizeof(buf));
      if (!count) continue;
      raw("RX", std::vector<uint8_t>(buf, buf+count));
      for (size_t i = 0; i < count; ++i)
      {
        if (m_args.echo && echo < tx.size())
        {
          if (buf[i] != tx[echo++]) throw std::runtime_error("local echo mismatch");
          continue;
        }
        if (parser.feed(buf[i], frame)) return true;
      }
    }
    return false;
  }
  void disconnect(const std::string& reason)
  {
    Memory::clear(m_port); m_next_connect = Time::Clock::get() + 3;
    state(reason);
  }
  void onMain() override
  {
    while (!isStopping())
    {
      consumeMessages();
      if (m_reopen) { Memory::clear(m_port); m_reopen = false; m_next_connect = 0; }
      if (!isActive())
      {
        Memory::clear(m_port);
        setEntityState(IMC::EntityState::ESTA_NORMAL, Status::CODE_IDLE);
        waitForMessages(0.05); continue;
      }
      try
      {
        double now = Time::Clock::get();
        if (!m_port && now >= m_next_connect)
        {
          if (!std::isfinite(m_args.surface) || m_args.surface < 0)
            throw std::runtime_error("Surface Pressure must be explicitly configured in bar");
          m_port = new Hardware::SerialPort(m_args.device, m_args.baud);
          m_port->setCanonicalInput(false);
          m_port->setXONXOFF(false); m_port->setCTSRTS(false);
          Frame init;
          if (!transact(48, init)) throw std::runtime_error("initialization timeout / invalid frame");
          if (!initialized(init)) throw std::runtime_error("unsupported device class/group or initialization response");
          inf("KELLER %u.%u firmware %u.%u; P1 only; software reference %.6f bar; no hardware zero",
              init.bytes[2], init.bytes[3], init.bytes[4], init.bytes[5], m_args.surface);
          m_last_valid = now; m_next_sample = 0;
          state("initialized; awaiting valid P1");
        }
        now = Time::Clock::get();
        if (m_port && now >= m_next_sample)
        {
          m_next_sample = now + m_args.period;
          Frame frame;
          if (!transact(73, frame)) throw std::runtime_error("P1 timeout / invalid frame; no measurement published");
          if (frame.exception())
          {
            if (frame.bytes[2] == 32) throw std::runtime_error("device lost initialization; reconnecting");
            state(String::str("device exception %u; no measurement published", frame.bytes[2]));
          }
          else
          {
            const auto value = measurement(frame, m_args.surface, m_args.density, m_args.gravity, m_args.max_depth);
            if (!value.valid)
              state(String::str("invalid P1/status 0x%02x; no measurement published", value.status));
            else
            {
              // IMC Depth/Pressure have no validity bit: invalid samples are suppressed.
              const double timestamp = Time::Clock::getSinceEpoch();
              IMC::Pressure pressure; pressure.value = value.pascal; pressure.setTimeStamp(timestamp);
              IMC::Depth depth; depth.value = value.depth; depth.setTimeStamp(timestamp);
              dispatch(pressure, DF_KEEP_TIME); dispatch(depth, DF_KEEP_TIME);
              m_last_valid = Time::Clock::get(); state("active; valid P1", true);
            }
          }
        }
        if (m_port && Time::Clock::get() - m_last_valid >= 2)
          state("P1 expired >=2s; no valid measurement");
      }
      catch (const std::exception& error) { disconnect(error.what()); }
      waitForMessages(0.02);
    }
    Memory::clear(m_port);
  }
};
} }
DUNE_TASK
