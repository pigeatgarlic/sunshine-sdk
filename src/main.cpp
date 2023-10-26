/**
 * @file src/main.cpp
 * @brief Main entry point for Sunshine.
 */

// standard includes
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

// lib includes
#include <boost/log/attributes/clock.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/severity_logger.hpp>

// local includes
#include "config.h"
#include "main.h"
#include "dll.h"
#include "platform/common.h"
#include "rtsp.h"
#include "thread_pool.h"
#include "video.h"
#include "stream.h"

extern "C" {
#include <libavutil/log.h>
#include <rs.h>

#ifdef _WIN32
  #include <iphlpapi.h>
#endif
}

safe::mail_t mail::man;

namespace asio = boost::asio;
using asio::ip::udp;
using namespace std::literals;
namespace bl = boost::log;

#ifdef _WIN32
// Define global singleton used for NVIDIA control panel modifications
nvprefs::nvprefs_interface nvprefs_instance;
#endif

thread_pool_util::ThreadPool task_pool;
bl::sources::severity_logger<int> verbose(0);  // Dominating output
bl::sources::severity_logger<int> debug(1);  // Follow what is happening
bl::sources::severity_logger<int> info(2);  // Should be informed about
bl::sources::severity_logger<int> warning(3);  // Strange events
bl::sources::severity_logger<int> error(4);  // Recoverable errors
bl::sources::severity_logger<int> fatal(5);  // Unrecoverable errors

bool display_cursor = true;

using text_sink = bl::sinks::asynchronous_sink<bl::sinks::text_ostream_backend>;
boost::shared_ptr<text_sink> sink;

struct NoDelete {
  void
  operator()(void *) {}
};

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", int)


#ifdef _WIN32
namespace restore_nvprefs_undo {
  int
  entry(const char *name, int argc, char *argv[]) {
    // Restore global NVIDIA control panel settings to the undo file
    // left by improper termination of sunshine.exe, if it exists.
    // This entry point is typically called by the uninstaller.
    if (nvprefs_instance.load()) {
      nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
      nvprefs_instance.unload();
    }
    return 0;
  }
}  // namespace restore_nvprefs_undo
#endif



/**
 * @brief Flush the log.
 *
 * EXAMPLES:
 * ```cpp
 * log_flush();
 * ```
 */
void
log_flush() {
  sink->flush();
}

/**
 * @brief Map a specified port based on the base port.
 * @param port The port to map as a difference from the base port.
 * @return `std:uint16_t` : The mapped port number.
 *
 * EXAMPLES:
 * ```cpp
 * std::uint16_t mapped_port = map_port(1);
 * ```
 */
std::uint16_t
map_port(int port) {
  // calculate the port from the config port
  auto mapped_port = (std::uint16_t)((int) config::sunshine.port + port);

  // Ensure port is in the range of 1024-65535
  if (mapped_port < 1024 || mapped_port > 65535) {
    BOOST_LOG(warning) << "Port out of range: "sv << mapped_port;
  }

  // TODO: Ensure port is not already in use by another application

  return mapped_port;
}




extern void __cdecl
Init() {
#ifdef _WIN32
  // Switch default C standard library locale to UTF-8 on Windows 10 1803+
  setlocale(LC_ALL, ".UTF-8");
#endif

  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
  mail::man = std::make_shared<safe::mail_raw_t>();


  if (config::sunshine.min_log_level >= 1) {
    av_log_set_level(AV_LOG_QUIET);
  }
  else {
    av_log_set_level(AV_LOG_DEBUG);
  }
  av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vl) {
    static int print_prefix = 1;
    char buffer[1024];

    av_log_format_line(ptr, level, fmt, vl, buffer, sizeof(buffer), &print_prefix);
    if (level <= AV_LOG_FATAL) {
      BOOST_LOG(fatal) << buffer;
    }
    else if (level <= AV_LOG_ERROR) {
      BOOST_LOG(error) << buffer;
    }
    else if (level <= AV_LOG_WARNING) {
      BOOST_LOG(warning) << buffer;
    }
    else if (level <= AV_LOG_INFO) {
      BOOST_LOG(info) << buffer;
    }
    else if (level <= AV_LOG_VERBOSE) {
      // AV_LOG_VERBOSE is less verbose than AV_LOG_DEBUG
      BOOST_LOG(debug) << buffer;
    }
    else {
      BOOST_LOG(verbose) << buffer;
    }
  });

  sink = boost::make_shared<text_sink>();

  boost::shared_ptr<std::ostream> stream { &std::cout, NoDelete {} };
  sink->locked_backend()->add_stream(stream);
  sink->locked_backend()->add_stream(boost::make_shared<std::ofstream>(config::sunshine.log_file));
  sink->set_filter(severity >= config::sunshine.min_log_level);

  sink->set_formatter([message = "Message"s, severity = "Severity"s](const bl::record_view &view, bl::formatting_ostream &os) {
    constexpr int DATE_BUFFER_SIZE = 21 + 2 + 1;  // Full string plus ": \0"

    auto log_level = view.attribute_values()[severity].extract<int>().get();

    std::string_view log_type;
    switch (log_level) {
      case 0:
        log_type = "Verbose: "sv;
        break;
      case 1:
        log_type = "Debug: "sv;
        break;
      case 2:
        log_type = "Info: "sv;
        break;
      case 3:
        log_type = "Warning: "sv;
        break;
      case 4:
        log_type = "Error: "sv;
        break;
      case 5:
        log_type = "Fatal: "sv;
        break;
    };

    char _date[DATE_BUFFER_SIZE];
    std::time_t t = std::time(nullptr);
    strftime(_date, DATE_BUFFER_SIZE, "[%Y:%m:%d:%H:%M:%S]: ", std::localtime(&t));

    os << _date << log_type << view.attribute_values()[message].extract<std::string>();
  });

  // Flush after each log record to ensure log file contents on disk isn't stale.
  // This is particularly important when running from a Windows service.
  sink->locked_backend()->auto_flush(true);

  bl::core::get()->add_sink(sink);
  auto fg = util::fail_guard(log_flush);

#ifdef WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver reinstallation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);
#endif




  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.
  auto deinit_guard = platf::init();
  if (!deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  BOOST_LOG(error) << "Hello from thinkmay."sv;
  reed_solomon_init();

}

extern void __cdecl
DeInit(){
#ifdef WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif
}

extern void __cdecl
Wait(){
  mail::man->event<bool>(mail::shutdown)->view();
}




/**
 * @brief Main application entry point.
 * @param argc The number of arguments.
 * @param argv The arguments.
 *
 * EXAMPLES:
 * ```cpp
 * main(1, const char* args[] = {"sunshine", nullptr});
 * ```
 */
extern int __cdecl 
StartCallback(DECODE_CALLBACK callback) {
  if (video::probe_encoders()) {
    BOOST_LOG(error) << "Video failed to find working encoder"sv;
    return 1;
  }
  auto video = std::thread {[](){
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
    auto timebase = boost::posix_time::microsec_clock::universal_time();

    // Video traffic is sent on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);
    while (auto packet = packets->pop()) {
      if (shutdown_event->peek()) {
        break;
      }


      DECODE_CALLBACK callback = (DECODE_CALLBACK)packet->channel_data;
      auto raw = packet.get();
      callback(raw->data(),(int)raw->data_size());
    }

    shutdown_event->raise(true);
  }};

  // int width;  // Video width in pixels
  // int height;  // Video height in pixels
  // int framerate;  // Requested framerate, used in individual frame bitrate budget calculation
  // int bitrate;  // Video bitrate in kilobits (1000 bits) for requested framerate
  // int slicesPerFrame;  // Number of slices per frame
  // int numRefFrames;  // Max number of reference frames

  // /* Requested color range and SDR encoding colorspace, HDR encoding colorspace is always BT.2020+ST2084
  //    Color range (encoderCscMode & 0x1) : 0 - limited, 1 - full
  //    SDR encoding colorspace (encoderCscMode >> 1) : 0 - BT.601, 1 - BT.709, 2 - BT.2020 */
  // int encoderCscMode;

  // int videoFormat;  // 0 - H.264, 1 - HEVC, 2 - AV1

  // /* Encoding color depth (bit depth): 0 - 8-bit, 1 - 10-bit
  //    HDR encoding activates when color depth is higher than 8-bit and the display which is being captured is operating in HDR mode */
  // int dynamicRange;
  video::config_t monitor = { 1920, 1080, 60, 1000, 1, 0, 1, 0, 0 };

  auto mail = std::make_shared<safe::mail_raw_t>();
  auto capture = std::thread {[&](){video::capture(mail, monitor, (void*)callback);}};

  // Create signal handler after logging has been initialized
  mail::man->event<bool>(mail::shutdown)->view();
  return 0;
}

/**
 * @brief Main application entry point.
 * @param argc The number of arguments.
 * @param argv The arguments.
 *
 * EXAMPLES:
 * ```cpp
 * main(1, const char* args[] = {"sunshine", nullptr});
 * ```
 */
extern int __cdecl 
StartQueue() {
  if (video::probe_encoders()) {
    BOOST_LOG(error) << "Video failed to find working encoder"sv;
    return 1;
  }
  // int width;  // Video width in pixels
  // int height;  // Video height in pixels
  // int framerate;  // Requested framerate, used in individual frame bitrate budget calculation
  // int bitrate;  // Video bitrate in kilobits (1000 bits) for requested framerate
  // int slicesPerFrame;  // Number of slices per frame
  // int numRefFrames;  // Max number of reference frames

  // /* Requested color range and SDR encoding colorspace, HDR encoding colorspace is always BT.2020+ST2084
  //    Color range (encoderCscMode & 0x1) : 0 - limited, 1 - full
  //    SDR encoding colorspace (encoderCscMode >> 1) : 0 - BT.601, 1 - BT.709, 2 - BT.2020 */
  // int encoderCscMode;

  // int videoFormat;  // 0 - H.264, 1 - HEVC, 2 - AV1

  // /* Encoding color depth (bit depth): 0 - 8-bit, 1 - 10-bit
  //    HDR encoding activates when color depth is higher than 8-bit and the display which is being captured is operating in HDR mode */
  // int dynamicRange;
  video::config_t monitor = { 1920, 1080, 60, 1000, 1, 0, 1, 0, 0 };

  void* null = 0;
  auto mail = std::make_shared<safe::mail_raw_t>();
  auto capture = std::thread {[&](){video::capture(mail, monitor, (void*)null);}};

  // Create signal handler after logging has been initialized
  mail::man->event<bool>(mail::shutdown)->view();
  DeInit();
  return 0;
}

int __cdecl 
PopFromQueue(void* data,int* duration) 
{
  auto packets = mail::man->queue<video::packet_t>(mail::video_packets);
  auto packet = packets->pop();
  // packet->
  memcpy(data,packet->data(),packet->data_size());
  return packet->data_size();
}

void 
DecodeCallbackEx(void* data, int size) {
  printf("received packet with size %d\n",size);
}

/**
 * @brief Main application entry point.
 * @param argc The number of arguments.
 * @param argv The arguments.
 *
 * EXAMPLES:
 * ```cpp
 * main(1, const char* args[] = {"sunshine", nullptr});
 * ```
 */
int
main(int argc, char *argv[]) {
  Init();
  // StartCallback(DecodeCallbackEx);

  auto video = std::thread {[](){
    auto shutdown_event = mail::man->event<bool>(mail::broadcast_shutdown);
    // Video traffic is sent on this thread
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    int duration = 0;
    void* data = malloc(100 * 1000 * 1000);
    while (true) {
      int size = PopFromQueue(data,&duration);
      if (shutdown_event->peek()) {
        break;
      }



      DecodeCallbackEx(data,size);
    }

    shutdown_event->raise(true);
  }};

  StartQueue();
  return 0;
}