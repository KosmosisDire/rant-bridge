#pragma once

/* Which browser pages may open a session. A client that sends no Origin is no page and is
 * never asked (Rant's spec/tunnel.md). */

#include <string>
#include <vector>

namespace bridge {

/* What a config with no allowed_origins takes: pages on this computer and the LAN, and the
   explorer's site. */
std::vector<std::string> default_origins();

/* origin as a browser sends it. "*" allows any, "local" a page served from loopback, a private
   IPv4 address or a .local name, and any other entry that one origin. */
bool origin_allowed(const std::vector<std::string>& allowed, const std::string& origin);

}
