#pragma once
//
// Minimal terminal UI for the full node. Two screens — F1 main (wallet +
// chain stats + moderator panels) and F2 traffic (live RPC counters) —
// switched with the F1/F2 function keys; Q quits the node gracefully.
// Drawn via PDCurses on Windows and ncurses on Linux. The TUI is the
// default for `start`; pass --no-tui / --daemon when running under a
// service manager so the terminal doesn't get left in raw mode.
//

#include <atomic>
#include <string>

namespace mc::api { class HttpServer; class RatsApi; }
namespace mc { class Chain; }
namespace mc { class Database; }
namespace mc { class CandidateManager; }
namespace mc::crypto { struct KeyPair; }
namespace mc::store { class SwarmIndex; }
namespace mc::net   { class NetworkManager; }

namespace mc::ui {

/// Run the TUI on the caller's thread. Blocks until the user presses Q
/// or `should_quit` flips true (the same atomic the SIGINT handler
/// sets). Reads chain + swarm + wallet state on every redraw tick, so
/// those must remain valid for the duration of the call.
void run_tui(mc::api::HttpServer&    http,
             mc::api::RatsApi&       api,
             mc::Chain&              chain,
             mc::Database&           db,
             mc::store::SwarmIndex&  swarm,
             mc::net::NetworkManager& net,
             mc::CandidateManager&   candidates,
             const mc::crypto::KeyPair& node_keypair,
             const std::string&      data_dir,
             std::atomic<bool>&      should_quit);

/// Divert std::cout / std::cerr into the F2 in-memory ring. Safe to
/// call before run_tui — startup logs from chain init / network bringup
/// then appear on F2 instead of scrolling on the console.
void start_log_capture();
void stop_log_capture();

} // namespace mc::ui
