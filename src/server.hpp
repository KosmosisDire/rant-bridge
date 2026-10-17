#pragma once

/* The WebSocket server: checks the Origin and the passphrase, gives each client a tunnel
 * session, hands a returning client its session back, and ends sessions whose grace ran out. */

#include "config.hpp"

#include <atomic>
#include <functional>
#include <string>

namespace bridge {

/* Serves until stop() returns true, which is polled a few times a second. The exit code. */
int serve(const Config& cfg, const std::function<bool()>& stop, const std::function<void(const std::string& url)>& listening);

}
