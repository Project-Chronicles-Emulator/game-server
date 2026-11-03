#pragma once

// Plain-HTTP listener for the agent engine (src/services/agent_engine/agent_engine_rest.hpp).
//
// The client calls the agent engine without credentials, so by default only loopback peers are
// served. Requests run on a caller-owned worker pool separate from the game server's, because a
// configured language model can take seconds to answer.

#include "services/agent_engine/agent_engine_rest.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <memory>

namespace chronicles::agent_engine {

// Concurrent connections; further connections are closed immediately.
inline constexpr unsigned kMaximumAgentEngineConnections = 32;

class AgentEngineListener : public std::enable_shared_from_this<AgentEngineListener> {
public:
  AgentEngineListener(boost::asio::io_context &io, const boost::asio::ip::tcp::endpoint &endpoint,
                      AgentEngineRest &rest, boost::asio::thread_pool &workers, bool allow_remote);

  unsigned short port() const;

  void run();

private:
  void accept_next();

  boost::asio::ip::tcp::acceptor acceptor_;
  AgentEngineRest &rest_;
  boost::asio::thread_pool &workers_;
  bool allow_remote_{};
  std::shared_ptr<std::atomic_uint> connections_;
};

} // namespace chronicles::agent_engine
