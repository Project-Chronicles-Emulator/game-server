#pragma once

#include "protocol/protocol.hpp"

namespace chronicles {
// A private ephemeral loopback socket pair, never the retail client or a remote server.
void replay_tcp(const std::vector<Record> &records, bool paced,
                const std::function<void(const Record &)> &consume);

} // namespace chronicles
