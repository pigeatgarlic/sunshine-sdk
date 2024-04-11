/**
 * @file src/main.cpp
 * @brief Main entry point for Sunshine.
 */

// standard includes
#include <csignal>
#include <fstream>
#include <iostream>
#include <boost/interprocess/managed_shared_memory.hpp>

// local includes
#include "globals.h"
#include "interprocess.h"
#include "logging.h"
#include "main.h"
#include "version.h"
#include "video.h"
#include "audio.h"
#include "input.h"
#include "config.h"

#ifdef _WIN32
#include <Windows.h>
#endif

using namespace std::literals;
using namespace boost::interprocess;

std::map<int, std::function<void()>> signal_handlers;
void
on_signal_forwarder(int sig) {
  signal_handlers.at(sig)();
}

template <class FN>
void
on_signal(int sig, FN &&fn) {
  signal_handlers.emplace(sig, std::forward<FN>(fn));

  std::signal(sig, on_signal_forwarder);
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

  task_pool_util::TaskPool::task_id_t force_shutdown = nullptr;

#ifdef _WIN32
  // Switch default C standard library locale to UTF-8 on Windows 10 1803+
  setlocale(LC_ALL, ".UTF-8");
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Use UTF-8 conversion for the default C++ locale (used by boost::log)
  std::locale::global(std::locale(std::locale(), new std::codecvt_utf8<wchar_t>));
#pragma GCC diagnostic pop

  mail::man = std::make_shared<safe::mail_raw_t>();

  auto log_deinit_guard = logging::init(config::sunshine.min_log_level, config::sunshine.log_file);
  if (!log_deinit_guard) {
    BOOST_LOG(error) << "Logging failed to initialize"sv;
  }

  // logging can begin at this point
  // if anything is logged prior to this point, it will appear in stdout, but not in the log viewer in the UI
  // the version should be printed to the log before anything else
  BOOST_LOG(info) << PROJECT_NAME << " version: " << PROJECT_VER;


#ifdef WIN32
  // Modify relevant NVIDIA control panel settings if the system has corresponding gpu
  if (nvprefs_instance.load()) {
    // Restore global settings to the undo file left by improper termination of sunshine.exe
    nvprefs_instance.restore_from_and_delete_undo_file_if_exists();
    // Modify application settings for sunshine.exe
    nvprefs_instance.modify_application_profile();
    // Modify global settings, undo file is produced in the process to restore after improper termination
    nvprefs_instance.modify_global_profile();
    // Unload dynamic library to survive driver re-installation
    nvprefs_instance.unload();
  }

  // Wait as long as possible to terminate Sunshine.exe during logoff/shutdown
  SetProcessShutdownParameters(0x100, SHUTDOWN_NORETRY);

  // We must create a hidden window to receive shutdown notifications since we load gdi32.dll
  std::promise<HWND> session_monitor_hwnd_promise;
  auto session_monitor_hwnd_future = session_monitor_hwnd_promise.get_future();
  std::promise<void> session_monitor_join_thread_promise;
  auto session_monitor_join_thread_future = session_monitor_join_thread_promise.get_future();
#endif

  task_pool.start(1);

  // Create signal handler after logging has been initialized
  auto process_shutdown_event = mail::man->event<bool>(mail::shutdown);
  on_signal(SIGINT, [&force_shutdown, process_shutdown_event]() {
    BOOST_LOG(info) << "Interrupt handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    process_shutdown_event->raise(true);
  });

  on_signal(SIGTERM, [&force_shutdown, process_shutdown_event]() {
    BOOST_LOG(info) << "Terminate handler called"sv;

    auto task = []() {
      BOOST_LOG(fatal) << "10 seconds passed, yet Sunshine's still running: Forcing shutdown"sv;
      logging::log_flush();
    };
    force_shutdown = task_pool.pushDelayed(task, 10s).task_id;

    process_shutdown_event->raise(true);
  });

  // If any of the following fail, we log an error and continue event though sunshine will not function correctly.
  // This allows access to the UI to fix configuration problems or view the logs.

  auto platf_deinit_guard = platf::init();
  if (!platf_deinit_guard) {
    BOOST_LOG(error) << "Platform failed to initialize"sv;
  }

  auto input_deinit_guard = input::init();

  int queuetype = -1;
  std::stringstream ss; ss << argv[2]; ss >> queuetype;
  if(queuetype != QueueType::Audio && queuetype != QueueType::Input) {
    if (video::probe_encoders()) {
      BOOST_LOG(error) << "Video failed to find working encoder"sv;
      return -1;
    }
  }

  //Get buffer local address from handle
  SharedMemory* memory = obtain_shared_memory(argv[1]);

  auto video_capture = [&](safe::mail_t mail, char* displayin,int codec){
    std::optional<std::string> display = std::nullopt;
    if (strlen(displayin) > 0)
      display = std::string(displayin);

    video::capture(mail,video::config_t{
      display, 1920, 1080, 60, 6000, 1, 0, 1, codec, 0
    },NULL);
  };

  auto audio_capture = [&](safe::mail_t mail){
    audio::capture(mail,audio::config_t{
      10,2,3,0
    },NULL);
  };
    

  auto pull = [process_shutdown_event](safe::mail_t mail, Queue* queue){
    auto input         = input::alloc(mail);
    auto local_shutdown= mail->event<bool>(mail::shutdown);

    queue->metadata.active = 1;
    auto current_index = queue->index;
    while (!process_shutdown_event->peek() && !local_shutdown->peek()) {
      while (current_index < queue->index) {
        current_index++;
        auto real_index = current_index % QUEUE_SIZE;
        auto data = queue->array[real_index].data;
        auto size = queue->array[real_index].size;
        std::vector<uint8_t> raw(data, data + size);
        input::passthrough(input,raw);
      }
      

      std::this_thread::sleep_for(1ms);
    }
    queue->metadata.active = 0;
  };

  auto push = [process_shutdown_event](safe::mail_t mail, Queue* queue){
    auto video_packets = mail->queue<video::packet_t>(mail::video_packets);
    auto audio_packets = mail->queue<audio::packet_t>(mail::audio_packets);
    auto bitrate       = mail->event<int>(mail::bitrate);
    auto framerate     = mail->event<int>(mail::framerate);
    auto idr           = mail->event<bool>(mail::idr);
    auto local_shutdown= mail->event<bool>(mail::shutdown);

    queue->metadata.active = 1;
    while (!process_shutdown_event->peek() && !local_shutdown->peek()) {
      while(video_packets->peek()) {
        auto packet = video_packets->pop();
        push_packet(queue,packet->data(),packet->data_size(),PacketMetadata{ packet->is_idr() });
      }
      while(audio_packets->peek()) {
        auto packet = audio_packets->pop();
        push_packet(queue,packet->second.begin(),packet->second.size(),PacketMetadata{ 0 });
      }

      if(peek_event(queue,EventType::Bitrate))
        bitrate->raise(pop_event(queue,EventType::Bitrate).value_number);
      if(peek_event(queue,EventType::Framerate)) 
        framerate->raise(pop_event(queue,EventType::Framerate).value_number);
      if(peek_event(queue,EventType::Pointer)) 
        display_cursor = pop_event(queue,EventType::Pointer).value_number;
      if(peek_event(queue,EventType::Idr)) 
        idr->raise(pop_event(queue,EventType::Idr).value_number > 0);

      std::this_thread::sleep_for(1ms);
    }

    if (!local_shutdown->peek())
      local_shutdown->raise(true);

    queue->metadata.active = 0;
  };


  auto mail = std::make_shared<safe::mail_raw_t>();
  auto queue = &memory->queues[queuetype];
  BOOST_LOG(info) << "Starting capture on channel " << queuetype;
  if (queuetype == QueueType::Video0 || queuetype == QueueType::Video1) {
    auto capture = std::thread{video_capture,mail,queue->metadata.display,queue->metadata.codec};
    auto forward = std::thread{push,mail,queue};
    capture.detach();
    forward.detach();
  } else if (queuetype == QueueType::Audio) {
    auto capture = std::thread{audio_capture,mail};
    auto forward = std::thread{push,mail,queue};
    capture.detach();
    forward.detach();
  } else if (queuetype == QueueType::Input) {
    auto process = std::thread{pull,mail,queue};
    process.detach();

  }

  auto local_shutdown= mail->event<bool>(mail::shutdown);
  while (!process_shutdown_event->peek() && !local_shutdown->peek())
    std::this_thread::sleep_for(1s);

  BOOST_LOG(info) << "Closed" << queuetype;
  // let other threads to close
  std::this_thread::sleep_for(1s);
  task_pool.stop();
  task_pool.join();

#ifdef WIN32
  // Restore global NVIDIA control panel settings
  if (nvprefs_instance.owning_undo_file() && nvprefs_instance.load()) {
    nvprefs_instance.restore_global_profile();
    nvprefs_instance.unload();
  }
#endif

  return 0;
}
