#ifndef SUNSHINE_UPNP_H
#define SUNSHINE_UPNP_H

#include <common.h>

namespace upnp {
[[nodiscard]] std::unique_ptr<platf::deinit_t> start();
}

#endif