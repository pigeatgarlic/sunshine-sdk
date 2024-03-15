/**
 * @file src/stream.h
 * @brief todo
 */
#pragma once

#include <boost/asio.hpp>

#include "audio.h"
#include "crypto.h"
#include "video.h"

namespace stream {
  constexpr auto CONTROL_PORT = 1;
  constexpr auto VIDEO_STREAM_PORT = 2;
  constexpr auto AUDIO_STREAM_PORT = 3;

  struct session_t;
  struct config_t {
    audio::config_t audio;
    video::config_t monitor;

    int packetsize;
    int minRequiredFecPackets;
    int featureFlags;
    int controlProtocolType;
    int audioQosType;
    int videoQosType;

    std::optional<int> gcmap;
  };

  namespace session {
    enum class state_e : int {
      STOPPED,
      STOPPING,
      STARTING,
      RUNNING,
    };

    std::shared_ptr<session_t>
    alloc(config_t &config, crypto::aes_t &gcm_key, crypto::aes_t &iv);
    int
    start(session_t &session, const std::string &addr_string);
    void
    stop(session_t &session);
    void
    join(session_t &session);
    state_e
    state(session_t &session);
  }  // namespace session
}  // namespace stream
