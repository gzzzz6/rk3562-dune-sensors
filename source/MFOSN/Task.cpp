// Distributed under the DUNE licence. mFOSN COMA receiver.
#include <DUNE/DUNE.hpp>
#include "Protocol.hpp"
#include "Alignment.hpp"
#include <chrono>
#include <fstream>
#include <map>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cerrno>
#include <fcntl.h>
#include <iomanip>
#include <locale>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace Sensors
{
  namespace MFOSN
  {
    using DUNE_NAMESPACES;

    struct Task: public DUNE::Tasks::Task
    {
      Task(const std::string& name, Tasks::Context& ctx):
        DUNE::Tasks::Task(name, ctx)
      {
        paramActive(Tasks::Parameter::SCOPE_GLOBAL, Tasks::Parameter::VISIBILITY_USER, true);
        param("Serial Port - Device", m_device).defaultValue("");
        param("Alignment Socket", m_socket_path).defaultValue("")
          .description("Empty: read-only COMA. Otherwise use a root-private runtime directory");
        param("Confirmed Bottom Track", m_confirmed).defaultValue("false")
          .description("Enable DVL IMC measurements only after confirming axis order, age and bottom-track source");
      }

      void onUpdateParameters() override
      {
        if (paramChanged(m_device) || paramChanged(m_socket_path))
          m_reopen = true;
      }
      void onResourceRelease() override { disconnect(); closeSocket(); }

      void disconnect()
      {
        if (m_fd >= 0)
          close(m_fd);
        m_fd = -1;
        m_seen_frame = false;
        m_seen_tick = false;
        m_parser.reset();
        m_dvl.reset();
      }

      void openPort()
      {
        if (m_device.empty())
          throw std::runtime_error("configure COMA serial device");
        m_fd = open(m_device.c_str(), (m_server < 0 ? O_RDONLY : O_RDWR)
                    | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (m_fd < 0)
          throw std::runtime_error("cannot open COMA");
        termios options{};
        if (tcgetattr(m_fd, &options) < 0)
          throw std::runtime_error("cannot read COMA serial settings");
        cfmakeraw(&options);
        options.c_iflag &= ~(IXON | IXOFF | IXANY);
        options.c_cflag &= ~(CSTOPB | CRTSCTS);
        options.c_cflag |= CLOCAL | CREAD;
        if (cfsetispeed(&options, B115200) < 0 || cfsetospeed(&options, B115200) < 0
            || tcsetattr(m_fd, TCSANOW, &options) < 0)
          throw std::runtime_error("cannot set COMA to 115200 8N1");
        m_last_frame = Time::Clock::get();
      }

      void status(const char* text, bool ok)
      {
        if (text == m_status)
          return;
        m_status = text;
        setEntityState(ok ? IMC::EntityState::ESTA_NORMAL : IMC::EntityState::ESTA_ERROR, text);
        inf("%s", text);
      }

      void publishDvl(const Reading* r, double stamp)
      {
        IMC::GroundVelocity speed;
        IMC::Distance distance;
        if (r)
        {
          speed.x = r->dvl[0];
          speed.y = r->dvl[2];
          speed.z = -r->dvl[1];
          speed.validity = 7;
          distance.value = r->bottom_m;
          distance.validity = IMC::Distance::DV_VALID;
        }
        else
        {
          speed.validity = 0;
          distance.validity = IMC::Distance::DV_INVALID;
        }
        speed.setTimeStamp(stamp);
        distance.setTimeStamp(stamp);
        dispatch(speed, DF_KEEP_TIME);
        dispatch(distance, DF_KEEP_TIME);
        m_dvl_published = r != nullptr;
      }

      void invalidateDvl()
      {
        if (m_dvl_published)
          publishDvl(nullptr, Time::Clock::getSinceEpoch());
      }

      void handle(const Frame& frame, double now)
      {
        const Reading r = decode(frame);
        const double stamp = Time::Clock::getSinceEpoch();
        m_last_frame = now;
        m_seen_frame = true;
        if (!m_seen_tick || r.ticks != m_last_tick)
        {
          m_seen_tick = true;
          m_last_tick = r.ticks;
          m_position = {r.latitude_deg, r.longitude_deg};
          m_navigating = r.navigating();
          m_fix_time = steady();
          m_alignment.update(m_position, m_navigating, m_fix_time);
        }
        if (r.navigating())
        {
          IMC::EulerAngles msg;
          msg.time = double(r.ticks) * 0.005;
          msg.phi = r.roll_rad;
          msg.theta = r.pitch_rad;
          msg.psi = r.yaw_rad;
          msg.setTimeStamp(stamp);
          dispatch(msg, DF_KEEP_TIME);
        }
        if (m_dvl.update(r, now) && m_confirmed && m_dvl.fresh(now))
          publishDvl(&r, stamp);
        if (!m_confirmed || !m_dvl.fresh(now))
          invalidateDvl();
        status(!r.navigating() ? "COMA receiving; navigation not ready" :
               !m_confirmed ? "COMA active; DVL diagnostic only" :
               m_dvl.fresh(now) ? "COMA active; DVL fresh" : "COMA active; DVL unavailable", r.navigating());

        if (now >= m_next_report)
        {
          m_next_report = now + 0.2;
          std::ostringstream out;
          out.imbue(std::locale::classic());
          out << std::fixed << std::setprecision(8)
              << "MFOSN ticks=" << r.ticks << " utc_ms=" << r.utc_ms
              << " status_low=" << r.status_low << " status_high=" << r.status_high
              << " nav=" << r.navigating() << " dvl_fused=" << r.fusingDvl()
              << " dvl_valid=" << (m_confirmed && m_dvl.fresh(now))
              << " dvl_age_ms=" << r.dvl_age_ms
              << " lat_deg=" << r.latitude_deg << " lon_deg=" << r.longitude_deg
              << " elevation_m=" << r.elevation_m << " n_mps=" << r.north_mps
              << " e_mps=" << r.east_mps << " d_mps=" << r.down_mps
              << " dvl_forward_mps=" << r.dvl[0] << " dvl_up_mps=" << r.dvl[1]
              << " dvl_right_mps=" << r.dvl[2] << " bottom_m=" << r.bottom_m
              << " bad_checksums=" << m_parser.badChecksums();
          IMC::DevDataText text;
          text.value = out.str();
          text.setTimeStamp(stamp);
          dispatch(text, DF_KEEP_TIME);
        }
      }

      void onMain() override
      {
        while (!isStopping())
        {
          consumeMessages();
          if (m_reopen || !isActive())
          {
            invalidateDvl();
            disconnect();
            finishWrite("serial_disconnected");
            closeSocket();
            m_reopen = false;
            m_retry = 0;
          }
          if (!isActive())
          {
            waitForMessages(0.05);
            continue;
          }
          try
          {
            const double now = Time::Clock::get();
            if (m_server < 0 && !m_socket_path.empty() && steady() >= m_socket_retry)
            {
              m_socket_retry = steady() + 5;
              openSocket();
              if (m_server >= 0)
                disconnect(); // Only on enabling the listener, never to send a request.
            }
            serviceAlignment();
            if (m_fd < 0 && now >= m_retry)
              openPort();
            if (m_fd < 0)
            {
              waitForMessages(0.05);
              continue;
            }
            pollfd port{m_fd, short(POLLIN | (m_tx.empty() ? 0 : POLLOUT)), 0};
            const int ready = poll(&port, 1, 20);
            if ((ready < 0 && errno != EINTR) || (port.revents & (POLLERR | POLLHUP | POLLNVAL)))
              throw std::runtime_error("COMA disconnected");
            if (ready > 0 && (port.revents & POLLIN))
            {
              uint8_t bytes[4096];
              const ssize_t count = read(m_fd, bytes, sizeof(bytes));
              if (count < 0 && errno != EAGAIN && errno != EINTR)
                throw std::runtime_error("COMA read failed");
              Frame frame;
              for (ssize_t i = 0; i < count; ++i)
                if (m_parser.feed(bytes[i], frame))
                  handle(frame, Time::Clock::get());
            }
            if (Time::Clock::get() - m_last_frame >= 1.0)
              throw std::runtime_error("COMA stale: 1 s without a valid frame");
            if (port.revents & POLLOUT)
              writeAlignment();
          }
          catch (const std::exception& e)
          {
            invalidateDvl();
            finishWrite("serial_disconnected");
            disconnect();
            status(e.what(), false);
            m_retry = Time::Clock::get() + 1.0;
          }
        }
        invalidateDvl();
      }

    private:
      static double steady()
      {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
      }

      void openSocket()
      {
        sockaddr_un addr{};
        struct stat info{};
        const auto slash = m_socket_path.find_last_of('/');
        if (m_socket_path.compare(0, 5, "/run/") != 0 || slash <= 4
            || m_socket_path.size() >= sizeof(addr.sun_path)
            || lstat(m_socket_path.substr(0, slash).c_str(), &info) < 0
            || !S_ISDIR(info.st_mode) || info.st_uid != 0 || (info.st_mode & 0077))
        {
          err("Alignment disabled: a root-private /run subdirectory is required");
          return;
        }
        std::ifstream uuid("/proc/sys/kernel/random/uuid");
        std::string token;
        uuid >> token;
        if (token.size() != 36)
        {
          err("Alignment disabled: cannot generate socket session ID");
          return;
        }
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
          return;
        addr.sun_family = AF_UNIX;
        std::copy(m_socket_path.begin(), m_socket_path.end(), addr.sun_path);
        const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
        if (!bound || chmod(addr.sun_path, 0600) < 0 || listen(fd, 4) < 0)
        {
          err("Alignment socket unavailable: %s", strerror(errno));
          close(fd);
          if (bound)
            unlink(addr.sun_path);
          return;
        }
        m_server = fd;
        m_bound_path = m_socket_path;
        m_token = token;
        m_results.clear();
        m_alignment = Alignment{};
        m_last_logged.clear();
      }

      void closeClient()
      {
        if (m_client >= 0)
          close(m_client);
        m_client = -1;
      }

      void closeSocket()
      {
        closeClient();
        if (m_server >= 0)
        {
          close(m_server);
          unlink(m_bound_path.c_str());
        }
        m_server = -1;
      }

      bool serialFresh() const
      {
        return m_fd >= 0 && m_seen_frame && Time::Clock::get() - m_last_frame < 1;
      }

      void finishWrite(const char* failure)
      {
        if (m_alignment.state != "writing")
          return;
        inf("Alignment id=%s target=%.8f,%.8f write=%s bytes=%zu/%zu",
            m_alignment.id.c_str(), m_alignment.target.lat, m_alignment.target.lon,
            failure ? failure : "submitted", m_tx_offset, m_tx.size());
        if (failure)
          m_alignment.state = failure;
        else
          m_alignment.sent(steady());
        m_tx.clear();
      }

      void writeAlignment()
      {
        if (steady() >= m_write_deadline)
        {
          finishWrite("write_timeout");
          return;
        }
        const ssize_t n = write(m_fd, m_tx.data() + m_tx_offset, m_tx.size() - m_tx_offset);
        if (n > 0)
        {
          m_tx_offset += static_cast<size_t>(n);
          if (m_tx_offset == m_tx.size())
            finishWrite(nullptr);
        }
        else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
          finishWrite("write_failed");
      }

      void serviceAlignment()
      {
        const double now = steady();
        if (!m_tx.empty() && now >= m_write_deadline)
          finishWrite("write_timeout");
        m_alignment.expire(now);
        if (!m_alignment.id.empty() && m_alignment.state != m_last_logged)
        {
          m_last_logged = m_alignment.state;
          m_results[m_alignment.id] = m_last_logged;
          inf("Alignment id=%s state=%s target=%.8f,%.8f readback=%.8f,%.8f nav=%d fresh=%d already_near=%d",
              m_alignment.id.c_str(), m_last_logged.c_str(), m_alignment.target.lat, m_alignment.target.lon,
              m_position.lat, m_position.lon, m_navigating, serialFresh(), m_alignment.alreadyNear);
        }
        if (m_server < 0)
          return;
        if (m_client < 0)
        {
          m_client = accept4(m_server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          m_client_deadline = now + 0.25;
        }
        if (m_client < 0)
          return;
        char packet[256];
        const ssize_t count = recv(m_client, packet, sizeof(packet), MSG_TRUNC);
        if (count < 0 && (errno == EAGAIN || errno == EINTR) && now < m_client_deadline)
          return;
        if (count > 0 && count < static_cast<ssize_t>(sizeof(packet)))
        {
          const std::string answer = command(std::string(packet, static_cast<size_t>(count)), now);
          send(m_client, answer.data(), answer.size(), MSG_NOSIGNAL);
        }
        closeClient();
      }

      std::string command(const std::string& packet, double now)
      {
        std::istringstream in(packet);
        in.imbue(std::locale::classic());
        std::string verb, id, extra, result = "invalid_request";
        Position goal{};
        const bool fix = serialFresh() && m_navigating && now - m_fix_time < 1 && validPosition(m_position);
        in >> verb >> id;
        if (verb == "status" && (id.empty() || !(in >> extra)))
        {
          if (id.empty())
            result = m_alignment.busy() ? "busy" : (serialFresh() ? "ready" : "serial_unavailable");
          else
          {
            auto found = m_results.find(id);
            result = found == m_results.end() ? "unknown_request" : found->second;
          }
        }
        else if (verb == "send" && id.size() == 69 && id.compare(0, 37, m_token + ":") == 0
                 && id.find_first_not_of("0123456789abcdef", 37) == std::string::npos
                 && (in >> goal.lat >> goal.lon) && !(in >> extra) && validPosition(goal))
        {
          const auto found = m_results.find(id);
          if (found != m_results.end())
            result = found->second;
          else if (m_results.size() >= 1024)
            result = "session_limit";
          else if (m_alignment.busy() || !serialFresh())
          {
            result = m_alignment.busy() ? "busy" : "serial_unavailable";
            m_results.emplace(id, result);
            inf("Alignment id=%s target=%.8f,%.8f rejected=%s", id.c_str(), goal.lat, goal.lon, result.c_str());
          }
          else
          {
            m_alignment.begin(id, goal, m_position, fix, now);
            m_tx = alignmentNmea(goal);
            m_tx_offset = 0;
            m_write_deadline = now + 0.5;
            result = m_alignment.state;
            m_results.emplace(id, result);
          }
        }
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << result << ' ' << m_token << ' ' << fix << ' ' << std::fixed << std::setprecision(8)
            << m_position.lat << ' ' << m_position.lon << ' '
            << (!id.empty() && id == m_alignment.id && m_alignment.alreadyNear) << '\n';
        return out.str();
      }

      int m_server = -1, m_client = -1;
      std::string m_socket_path, m_bound_path, m_token, m_tx, m_last_logged;
      std::map<std::string, std::string> m_results;
      Alignment m_alignment;
      Position m_position{};
      double m_socket_retry = 0, m_client_deadline = 0, m_write_deadline = 0, m_fix_time = 0;
      size_t m_tx_offset = 0;
      bool m_seen_frame = false, m_navigating = false, m_seen_tick = false;
      uint32_t m_last_tick = 0;
      int m_fd = -1;
      bool m_confirmed = false;
      bool m_reopen = false;
      bool m_dvl_published = false;
      double m_retry = 0, m_last_frame = 0, m_next_report = 0;
      std::string m_device, m_status;
      Parser m_parser;
      DvlFreshness m_dvl;
    };
  }
}
DUNE_TASK
