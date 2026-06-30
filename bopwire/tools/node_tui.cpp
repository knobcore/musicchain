#include "node_tui.h"

#include "../src/api/rats_api.h"
#include "../src/api/server.h"
#include "../src/consensus/candidate.h"
#include "../src/core/chain.h"
#include "../src/crypto/hash.h"
#include "../src/crypto/keys.h"
#include "../src/crypto/signature.h"
#include "../src/crypto/ecies.h"
#include "../src/crypto/bip39.h"
#include "../src/network/manager.h"
#include "../src/store/swarm.h"
#include "../src/storage/database.h"
#include "../src/tokens/ledger.h"
#include "../src/util/traffic.h"

#include <leveldb/write_batch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shellapi.h>
  #include <io.h>
  #include <fcntl.h>
  #pragma comment(lib, "shell32.lib")
#endif

// PDCurses on Windows (CMake wires MC_TUI_PDCURSES), system ncurses
// elsewhere. Both expose the same Curses API surface we use here.
#ifdef MC_TUI_PDCURSES
  #include <curses.h>
#else
  #include <ncurses.h>
#endif

namespace fs = std::filesystem;

namespace mc::ui {

namespace {

constexpr int CP_TITLE      = 1;  // cyan-on-black title
constexpr int CP_TAB_ON     = 2;  // active tab, reversed
constexpr int CP_TAB_OFF    = 3;  // inactive tab
constexpr int CP_LABEL      = 4;  // yellow labels
constexpr int CP_VALUE      = 5;  // bright values
constexpr int CP_OK         = 6;  // green OK
constexpr int CP_WARN       = 7;  // red warning
constexpr int CP_BORDER     = 8;  // panel border
constexpr int CP_FOOTER_KEY = 9;  // black-on-cyan F-key chip
constexpr int CP_FOOTER_LBL = 10; // white-on-blue label
constexpr int CP_PANEL_HDR  = 11; // bold panel header

void setup_colors() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();
    init_pair(CP_TITLE,      COLOR_CYAN,    -1);
    init_pair(CP_TAB_ON,     COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_TAB_OFF,    COLOR_WHITE,   -1);
    init_pair(CP_LABEL,      COLOR_YELLOW,  -1);
    init_pair(CP_VALUE,      COLOR_WHITE,   -1);
    init_pair(CP_OK,         COLOR_GREEN,   -1);
    init_pair(CP_WARN,       COLOR_RED,     -1);
    init_pair(CP_BORDER,     COLOR_CYAN,    -1);
    init_pair(CP_FOOTER_KEY, COLOR_BLACK,   COLOR_CYAN);
    init_pair(CP_FOOTER_LBL, COLOR_WHITE,   COLOR_BLUE);
    init_pair(CP_PANEL_HDR,  COLOR_BLACK,   COLOR_CYAN);
}

// ---- Drawing primitives ---------------------------------------------

void draw_box(int y, int x, int h, int w) {
    attron(COLOR_PAIR(CP_BORDER));
    mvaddch(y,         x,         ACS_ULCORNER);
    mvaddch(y,         x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x,         ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    mvhline(y,         x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x,         ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    attroff(COLOR_PAIR(CP_BORDER));
}

void draw_panel_header(int y, int x, int w, const char* title) {
    attron(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
    mvhline(y, x + 1, ' ', w - 2);
    mvprintw(y, x + 2, " %s ", title);
    attroff(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
}

void draw_header_bar(int active_page) {
    int max_x = getmaxx(stdscr);
    move(0, 0);
    clrtoeol();
    attron(COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvprintw(0, 1, " bopwire-node ");
    attroff(COLOR_PAIR(CP_TITLE) | A_BOLD);

    auto tab = [&](int x, int p, const char* label) {
        int pair = (p == active_page) ? CP_TAB_ON : CP_TAB_OFF;
        int attr = (p == active_page) ? (A_BOLD | A_REVERSE) : A_DIM;
        attron(COLOR_PAIR(pair) | attr);
        mvprintw(0, x, " %s ", label);
        attroff(COLOR_PAIR(pair) | attr);
    };
    tab(20, 1, "F1 Main");
    tab(34, 2, "F2 Logs");

    move(1, 0);
    clrtoeol();
    attron(COLOR_PAIR(CP_BORDER));
    mvhline(1, 0, ACS_HLINE, max_x);
    attroff(COLOR_PAIR(CP_BORDER));
}

struct FooterKey { const char* key; const char* label; };

void draw_footer_bar(const std::vector<FooterKey>& keys) {
    int max_y = getmaxy(stdscr);
    int y = max_y - 1;
    int max_x = getmaxx(stdscr);
    move(y, 0);
    clrtoeol();
    int x = 0;
    for (const auto& k : keys) {
        if (x + (int)strlen(k.key) + (int)strlen(k.label) + 2 >= max_x) break;
        attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        mvprintw(y, x, "%s", k.key);
        attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        x += (int)strlen(k.key);
        attron(COLOR_PAIR(CP_FOOTER_LBL));
        mvprintw(y, x, "%-*s", (int)strlen(k.label) + 1, k.label);
        attroff(COLOR_PAIR(CP_FOOTER_LBL));
        x += (int)strlen(k.label) + 1;
    }
    // Pad to end with footer-label color so the bar is continuous.
    attron(COLOR_PAIR(CP_FOOTER_LBL));
    while (x < max_x) { mvaddch(y, x, ' '); ++x; }
    attroff(COLOR_PAIR(CP_FOOTER_LBL));
}

void label_value(int y, int x, int label_w, const char* label,
                 const std::string& value) {
    attron(COLOR_PAIR(CP_LABEL));
    mvprintw(y, x, "%-*s", label_w, label);
    attroff(COLOR_PAIR(CP_LABEL));
    attron(COLOR_PAIR(CP_VALUE));
    mvprintw(y, x + label_w + 1, "%s", value.c_str());
    attroff(COLOR_PAIR(CP_VALUE));
}

// ---- Moderator key (loaded once at startup) -------------------------
//
// `ModeratorState` represents the SESSION-AUTH state: the moderator
// the human at the keyboard has identified as. The TUI starts with
// `logged_in = false` regardless of whether moderator.txt exists on
// disk; the operator must press L and present the private key to
// unlock the moderator console. This matches IRC-style identify
// semantics (the key file is purely a convenience offered as a default
// in the L prompt) and means a node left unattended in a terminal
// window doesn't ship a logged-in mod session to whoever walks up.

struct ModeratorState {
    bool                logged_in = false;
    mc::crypto::KeyPair kp{};
    std::string         addr_hex;
    // Phase 1 level is "moderator or not." Phase 2 replaces this with
    // the on-chain level (FOUNDER / OP / VOICE) read from `mlvl:<addr>`.
    int                 level     = 0;
};

// Hard-zero the key material before we drop the session struct so a
// process-image dump or post-logout swap-file write can't recover the
// private key. We don't rely on the compiler not optimizing this away;
// SecureZero would be better but adds a Windows-only dependency, and
// the explicit-write loop on a volatile pointer is generally a no-op
// stripper to current MSVC + Clang.
void wipe_session_key(ModeratorState& s) {
    volatile uint8_t* p = s.kp.private_key.data();
    for (size_t i = 0; i < s.kp.private_key.size(); ++i) p[i] = 0;
    s.kp.private_key.assign(s.kp.private_key.size(), 0);
    s.logged_in = false;
    s.addr_hex.clear();
    s.level = 0;
}

// Read the private key out of moderator.txt if present. Returned key
// is NOT auto-logged-in — the login flow uses this as the default
// value pre-filled in the L prompt so the bootstrap moderator (whose
// key is auto-generated into moderator.txt at first start) doesn't
// have to copy-paste from a sticky note.
std::string read_moderator_key_file(const std::string& data_dir) {
    const std::string path = data_dir + "/moderator.txt";
    std::ifstream f(path);
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
        const std::string prefix = "Private Key:";
        auto p = line.find(prefix);
        if (p == std::string::npos) continue;
        std::string rest = line.substr(p + prefix.size());
        size_t a = rest.find_first_not_of(" \t");
        size_t b = rest.find_last_not_of(" \t\r\n");
        if (a == std::string::npos || b == std::string::npos) break;
        return rest.substr(a, b - a + 1);
    }
    return {};
}

// ---- Login / logout actions ----------------------------------------

void action_login(mc::Database& db, const std::string& data_dir,
                  ModeratorState& mod, struct ModView& mv);
void action_logout(ModeratorState& mod, struct ModView& mv);
void action_bootstrap_founder(mc::Database& db, const std::string& data_dir,
                              struct ModView& mv);
void action_view_proposals(mc::Database& db, const ModeratorState& mod,
                           struct ModView& mv);
void action_manage_labels(mc::Database& db, const ModeratorState& mod,
                          struct ModView& mv);

// Returns the path of the founder seed file (plaintext BIP39 mnemonic)
// living next to the chain data dir. Forward-declared here so
// action_login can probe for it before action_bootstrap_founder's
// definition appears.
std::string founder_seed_path(const std::string& data_dir);

// Domain-separated salt for the founder's PBKDF2-HMAC-SHA512 derivation.
// Versioned so that if we ever migrate to a different KDF / parameter
// set we can change the salt and let the old chain's founder key keep
// working without confusion.
constexpr const char* kFounderSalt = "bopwire-founder-v1";
constexpr uint32_t    kFounderIters = 200000;

// ---- Inbox scans ----------------------------------------------------

std::vector<std::string> scan_inbox(const std::string& data_dir,
                                     const std::string& subdir,
                                     const std::vector<std::string>& exts) {
    std::vector<std::string> out;
    const fs::path inbox = fs::path(data_dir) / subdir;
    std::error_code ec;
    if (!fs::exists(inbox, ec)) {
        fs::create_directories(inbox, ec);
        return out;
    }
    for (auto& e : fs::directory_iterator(inbox, ec)) {
        if (!e.is_regular_file()) continue;
        const auto& p = e.path();
        std::string ext = p.extension().string();
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool ok = exts.empty();
        for (const auto& a : exts) if (ext == a) { ok = true; break; }
        if (!ok) continue;
        out.push_back(p.filename().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

inline std::vector<std::string> scan_dmca_inbox(const std::string& data_dir) {
    return scan_inbox(data_dir, "dmca", {".pdf"});
}

inline std::vector<std::string> scan_kyc_inbox(const std::string& data_dir) {
    return scan_inbox(data_dir, "kyc",
                       {".pdf", ".jpg", ".jpeg", ".png"});
}

// ---- Modal prompt ---------------------------------------------------

// Draw a centered single-line prompt at the bottom of the screen and
// read input until Enter or ESC. Returns false on ESC / empty.
// Same as prompt_string but with echo suppressed for sensitive input
// (passphrases / private keys). The user sees no characters as they
// type — there isn't even a `*` mask because draw artifacts from
// PDCurses overlay made the mask version look like a corrupted prompt.
bool prompt_secret(const char* title, std::string& out, int max_len = 256) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int box_w = std::min(max_x - 4, std::max(40, (int)strlen(title) + 12));
    int box_h = 3;
    int y = max_y - 5;
    int x = (max_x - box_w) / 2;

    attron(COLOR_PAIR(CP_FOOTER_LBL));
    for (int dy = 0; dy < box_h; ++dy) {
        move(y + dy, x); for (int dx = 0; dx < box_w; ++dx) addch(' ');
    }
    attroff(COLOR_PAIR(CP_FOOTER_LBL));
    draw_box(y, x, box_h, box_w);
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(y, x + 2, " %s ", title);
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);

    noecho();         // already noecho in main loop, but be explicit
    curs_set(1);
    nodelay(stdscr, FALSE);

    move(y + 1, x + 2);
    refresh();

    std::string buf;
    while ((int)buf.size() < max_len) {
        int ch = getch();
        if (ch == ERR) continue;
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) break;
        if (ch == 27 /* ESC */) { buf.clear(); break; }
        if (ch == KEY_BACKSPACE || ch == 8 /*BS*/ || ch == 127 /*DEL*/) {
            if (!buf.empty()) buf.pop_back();
            continue;
        }
        if (ch >= 32 && ch < 127) buf.push_back(static_cast<char>(ch));
    }

    curs_set(0);
    nodelay(stdscr, TRUE);

    out = std::move(buf);
    return !out.empty();
}

bool prompt_string(const char* title, std::string& out, int max_len = 80) {
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    int box_w = std::min(max_x - 4, max_len + (int)strlen(title) + 8);
    int box_h = 3;
    int y = max_y - 5;
    int x = (max_x - box_w) / 2;

    // Background
    attron(COLOR_PAIR(CP_FOOTER_LBL));
    for (int dy = 0; dy < box_h; ++dy) {
        move(y + dy, x); for (int dx = 0; dx < box_w; ++dx) addch(' ');
    }
    attroff(COLOR_PAIR(CP_FOOTER_LBL));
    draw_box(y, x, box_h, box_w);
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(y, x + 2, " %s ", title);
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);

    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);

    move(y + 1, x + 2);
    char buf[256] = {0};
    int rc = getnstr(buf, std::min((int)sizeof(buf) - 1, box_w - 4));

    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);

    if (rc == ERR) return false;
    out.assign(buf);
    // Strip trailing CR/LF/space.
    while (!out.empty() &&
           (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return !out.empty();
}

void flash_message(const std::string& msg, int color_pair) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);
    int y = max_y - 3;
    int w = std::min(max_x - 4, (int)msg.size() + 4);
    int x = (max_x - w) / 2;
    attron(COLOR_PAIR(color_pair) | A_BOLD);
    mvhline(y, x, ' ', w);
    mvprintw(y, x + 2, "%s", msg.c_str());
    attroff(COLOR_PAIR(color_pair) | A_BOLD);
    refresh();
    napms(900);
}

// ---- Main page (F1) -------------------------------------------------

struct ModView {
    std::vector<std::string> dmca_files;
    std::vector<std::string> kyc_files;
    std::string              last_action;
    int                      last_action_color = CP_OK;
};

void draw_main_page(mc::Chain& chain,
                    mc::Database& db,
                    mc::store::SwarmIndex& swarm,
                    mc::net::NetworkManager& net,
                    const mc::crypto::KeyPair& node_kp,
                    const ModeratorState& mod,
                    const ModView& mv) {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);

    // Reserve rows 0..1 for header, max_y-1 for footer.
    int top = 2;
    int bot = max_y - 1;
    int h   = bot - top;

    // Left pane: 36 cols wide (wallet + chain). Right pane: rest.
    int left_w = 36;
    if (max_x < 70) left_w = std::max(28, max_x / 2);
    int right_x = left_w;
    int right_w = max_x - right_x;

    // ---- Left pane: wallet + chain
    draw_box(top, 0, h, left_w);
    draw_panel_header(top, 0, left_w, "Wallet / Chain");

    const std::string node_addr = mc::crypto::to_checksum_hex(node_kp.address);
    const uint64_t    node_bal  = db.get_balance(node_kp.address);
    const Address     node_esc  = mc::crypto::escrow_address_for(node_kp.address);
    const uint64_t    esc_bal   = db.get_balance(node_esc);
    const auto        tip       = chain.tip();
    const size_t      songs     = swarm.song_count();
    const size_t      peers     = net.peer_count();
    const bool        node_is_mod = db.is_moderator(node_kp.address);

    int r = top + 2;
    int cx = 2;
    int lw = 11;
    label_value(r++, cx, lw, "Node addr",
                node_addr.substr(0, left_w - cx - lw - 3));
    label_value(r++, cx, lw, "Balance",
                mc::Ledger::format_balance(node_bal) + " mc");
    label_value(r++, cx, lw, "Escrow",
                mc::Ledger::format_balance(esc_bal) + " mc");
    ++r;
    label_value(r++, cx, lw, "Chain ht",   std::to_string(tip.height));
    label_value(r++, cx, lw, "Songs",      std::to_string(songs));
    label_value(r++, cx, lw, "Peers",      std::to_string(peers));
    ++r;

    // Session-auth status. When logged in we also surface the
    // moderator's own wallet balance / escrow so they can confirm
    // they're operating as the right account.
    if (mod.logged_in) {
        const uint64_t mod_bal  = db.get_balance(mod.kp.address);
        const Address  mod_esc  = mc::crypto::escrow_address_for(mod.kp.address);
        const uint64_t mod_eb   = db.get_balance(mod_esc);
        const bool     active   = db.is_moderator(mod.kp.address);

        attron(COLOR_PAIR(CP_BORDER));
        mvhline(r++, 2, ACS_HLINE, left_w - 4);
        attroff(COLOR_PAIR(CP_BORDER));

        attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
        mvprintw(r++, cx, "Signed in as moderator");
        attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);

        label_value(r++, cx, lw, "Address",
                    mod.addr_hex.substr(0, left_w - cx - lw - 3));
        label_value(r++, cx, lw, "Balance",
                    mc::Ledger::format_balance(mod_bal) + " mc");
        label_value(r++, cx, lw, "Escrow",
                    mc::Ledger::format_balance(mod_eb) + " mc");

        attron(COLOR_PAIR(CP_LABEL));
        mvprintw(r, cx, "%-*s", lw, "Active");
        attroff(COLOR_PAIR(CP_LABEL));
        if (active) {
            attron(COLOR_PAIR(CP_OK) | A_BOLD);
            mvprintw(r, cx + lw + 1, "yes");
            attroff(COLOR_PAIR(CP_OK) | A_BOLD);
        } else {
            // This shouldn't happen — login rejects non-moderator
            // addresses — but if a moderator was revoked mid-session
            // we want a visible warning.
            attron(COLOR_PAIR(CP_WARN));
            mvprintw(r, cx + lw + 1, "REVOKED — log out");
            attroff(COLOR_PAIR(CP_WARN));
        }
        ++r;
    } else {
        attron(COLOR_PAIR(CP_BORDER));
        mvhline(r++, 2, ACS_HLINE, left_w - 4);
        attroff(COLOR_PAIR(CP_BORDER));
        attron(A_DIM);
        mvprintw(r++, cx, "Signed out");
        attroff(A_DIM);
    }

    (void)node_is_mod;

    // ---- Right pane: moderator panel
    draw_box(top, right_x, h, right_w);
    draw_panel_header(top, right_x, right_w, "Moderator console");

    int rr = top + 2;
    int rcx = right_x + 2;
    int rlw = 16;

    if (!mod.logged_in) {
        // Logged-out state. We intentionally show nothing about the
        // inboxes here — DMCA / KYC files are about to become
        // moderator-only encrypted blobs (Phase 4) and even the
        // existence of files is a leak we don't want non-moderators
        // glancing at the node terminal to see.
        const bool has_founder = db.get_founder().has_value();

        attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
        mvprintw(rr++, rcx, "Signed out");
        attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
        ++rr;

        if (!has_founder) {
            // Genesis state. No founder has been recorded on chain yet,
            // so the moderator console is fully empty — the next step
            // is for the operator to bootstrap themselves as founder.
            attron(A_DIM);
            mvprintw(rr++, rcx,
                     "No founder has been recorded on chain yet.");
            mvprintw(rr++, rcx,
                     "Press B to bootstrap this node's operator as");
            mvprintw(rr++, rcx,
                     "founder using a passphrase you'll memorise.");
            mvprintw(rr++, rcx,
                     "The passphrase is hashed locally and never");
            mvprintw(rr++, rcx,
                     "written anywhere — only the derived address");
            mvprintw(rr++, rcx,
                     "and pubkey land on chain.");
            attroff(A_DIM);
            ++rr;

            attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
            mvprintw(rr, rcx, " B ");
            attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
            attron(COLOR_PAIR(CP_VALUE));
            mvprintw(rr, rcx + 4, "Bootstrap founder");
            attroff(COLOR_PAIR(CP_VALUE));
            ++rr;
        } else {
            attron(A_DIM);
            mvprintw(rr++, rcx,
                     "Press L to authenticate. You can enter either");
            mvprintw(rr++, rcx,
                     "your moderator passphrase (hashed locally) or");
            mvprintw(rr++, rcx,
                     "a raw 64-hex private key. The moderator");
            mvprintw(rr++, rcx,
                     "console (DMCA / KYC inboxes, library hide /");
            mvprintw(rr++, rcx,
                     "unhide, escrow release) unlocks once the");
            mvprintw(rr++, rcx,
                     "derived address shows on chain with a non-zero");
            mvprintw(rr++, rcx,
                     "ModLevel.");
            attroff(A_DIM);
            ++rr;

            attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
            mvprintw(rr, rcx, " L ");
            attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
            attron(COLOR_PAIR(CP_VALUE));
            mvprintw(rr, rcx + 4, "Log in");
            attroff(COLOR_PAIR(CP_VALUE));
            ++rr;
        }

        if (!mv.last_action.empty()) {
            ++rr;
            attron(COLOR_PAIR(mv.last_action_color) | A_BOLD);
            mvprintw(rr++, rcx, "%s", mv.last_action.c_str());
            attroff(COLOR_PAIR(mv.last_action_color) | A_BOLD);
        }
    } else {
        // Helper to render a labelled inbox section with up to N entries.
        auto draw_inbox = [&](const char* label,
                              const std::vector<std::string>& files,
                              int max_show) {
            attron(COLOR_PAIR(CP_LABEL));
            mvprintw(rr, rcx, "%-*s", rlw, label);
            attroff(COLOR_PAIR(CP_LABEL));
            attron(COLOR_PAIR(CP_VALUE));
            mvprintw(rr, rcx + rlw + 1, "%zu file%s pending",
                     files.size(), files.size() == 1 ? "" : "s");
            attroff(COLOR_PAIR(CP_VALUE));
            ++rr;
            int shown = 0;
            for (const auto& name : files) {
                if (shown >= max_show) break;
                std::string trimmed = name;
                int avail = right_w - 6;
                if ((int)trimmed.size() > avail)
                    trimmed = trimmed.substr(0, avail - 1) + "…";
                attron(COLOR_PAIR(CP_VALUE));
                mvprintw(rr++, rcx + 2, "• %s", trimmed.c_str());
                attroff(COLOR_PAIR(CP_VALUE));
                ++shown;
            }
            if (files.size() > (size_t)max_show) {
                attron(A_DIM);
                mvprintw(rr++, rcx + 2, "(+%zu more)",
                         files.size() - max_show);
                attroff(A_DIM);
            }
        };
        // Split the available list space between DMCA and KYC; reserve
        // 14 lines for everything below (actions + counts + flash line).
        const int total_avail = std::max(0, h - 14);
        const int per_inbox   = std::min(5, total_avail / 2);
        draw_inbox("DMCA inbox", mv.dmca_files, per_inbox);
        ++rr;
        draw_inbox("KYC inbox",  mv.kyc_files,  per_inbox);
        ++rr;

        attron(COLOR_PAIR(CP_BORDER));
        mvhline(rr++, rcx, ACS_HLINE, right_w - 4);
        attroff(COLOR_PAIR(CP_BORDER));

        attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
        mvprintw(rr++, rcx, "Actions");
        attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);

        auto action_line = [&](char k, const char* desc) {
            attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
            mvprintw(rr, rcx, " %c ", k);
            attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
            attron(COLOR_PAIR(CP_VALUE));
            mvprintw(rr, rcx + 4, "%s", desc);
            attroff(COLOR_PAIR(CP_VALUE));
            ++rr;
        };
        action_line('I', "Rescan DMCA + KYC inboxes");
        action_line('K', "KYC review · approve + release escrow");
        action_line('H', "Library · browse, hide, unhide");
        action_line('E', "Release escrow (manual prompts)");

        ++rr;
        // Live hide counts so the mod can tell at a glance whether the
        // public listing is being filtered and by how much.
        const size_t n_art   = db.list_hidden_artists().size();
        const size_t n_alb   = db.list_hidden_albums().size();
        const size_t n_title = db.list_hidden_titles().size();
        attron(COLOR_PAIR(CP_LABEL));
        mvprintw(rr, rcx, "Currently hidden");
        attroff(COLOR_PAIR(CP_LABEL));
        attron(COLOR_PAIR(CP_VALUE));
        mvprintw(rr, rcx + 18, "%zu artist%s · %zu album%s · %zu title%s",
                 n_art,   n_art   == 1 ? "" : "s",
                 n_alb,   n_alb   == 1 ? "" : "s",
                 n_title, n_title == 1 ? "" : "s");
        attroff(COLOR_PAIR(CP_VALUE));
        ++rr;
        ++rr;

        if (!mv.last_action.empty()) {
            attron(COLOR_PAIR(mv.last_action_color) | A_BOLD);
            mvprintw(rr++, rcx, "%s", mv.last_action.c_str());
            attroff(COLOR_PAIR(mv.last_action_color) | A_BOLD);
        }
    }
}

// ---- Live log capture (rdbuf swap → ring buffer) --------------------
//
// We swap std::cout / std::cerr's stream buffers to a custom streambuf
// that strips ANSI codes and pushes complete lines into a bounded ring.
// No fd manipulation — PDCurses' console handles are left alone — and
// every line written via the C++ stream API (librats logger, chain,
// rats_api, swarm, etc.) lands in the ring. C-style printf callers
// were converted to std::cout so this single channel catches everything.

struct LogRing {
    std::mutex              m;
    std::deque<std::string> lines;
    size_t                  cap = 1000;

    void push(std::string l) {
        std::lock_guard<std::mutex> g(m);
        lines.push_back(std::move(l));
        while (lines.size() > cap) lines.pop_front();
    }
    std::vector<std::string> tail(size_t n) {
        std::lock_guard<std::mutex> g(m);
        std::vector<std::string> out;
        const size_t start = lines.size() > n ? lines.size() - n : 0;
        out.reserve(lines.size() - start);
        for (size_t i = start; i < lines.size(); ++i) out.push_back(lines[i]);
        return out;
    }
};

LogRing g_logs;

class RingStreambuf : public std::streambuf {
public:
    explicit RingStreambuf(LogRing& r) : ring_(r) {}
protected:
    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) return c;
        std::lock_guard<std::mutex> g(write_m_);
        char ch = static_cast<char>(c);
        absorb_(ch);
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> g(write_m_);
        for (std::streamsize i = 0; i < n; ++i) absorb_(s[i]);
        return n;
    }
private:
    // Caller holds write_m_.
    void absorb_(char ch) {
        if (in_ansi_) {
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
                in_ansi_ = false;
            return;
        }
        if (ch == '\x1b') { in_ansi_ = true; return; }
        if (ch == '\r')   return;
        if (ch == '\n') {
            ring_.push(std::move(line_));
            line_.clear();
            return;
        }
        line_.push_back(ch);
    }

    LogRing&    ring_;
    std::mutex  write_m_;
    std::string line_;
    bool        in_ansi_ = false;
};

RingStreambuf* g_rb_cout = nullptr;
RingStreambuf* g_rb_cerr = nullptr;
std::streambuf* g_prev_cout = nullptr;
std::streambuf* g_prev_cerr = nullptr;

void draw_logs_page() {
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);
    int top = 2;
    int h   = max_y - 1 - top;
    int w   = max_x;

    draw_box(top, 0, h, w);
    draw_panel_header(top, 0, w, "Node log (live tail)");

    const int inner_h = h - 3;
    auto tail = g_logs.tail(inner_h);
    int r = top + 2;
    int avail = w - 4;
    for (auto& s : tail) {
        if ((int)s.size() > avail) s.resize(avail);
        mvprintw(r++, 2, "%s", s.c_str());
        if (r >= top + h - 1) break;
    }
    if (tail.empty()) {
        attron(A_DIM);
        mvprintw(top + 2, 2, "(no log lines captured yet)");
        attroff(A_DIM);
    }
}

// ---- Song browser modal (H key) -------------------------------------
//
// Full-screen list of every song on chain (live read from db each open
// so DMCA-relevant uploads appear immediately). Type to filter against
// title / artist / album, Up/Down navigate, Enter opens the hide menu
// for the selected row, ESC backs out.

struct SongRow {
    std::string ch_hex;
    std::string title;
    std::string artist;
    std::string album;
    // Hide status, populated by load_song_rows:
    //   "" if visible, otherwise the scope that's masking it:
    //   "title" | "album" | "artist" | "hash"
    std::string hidden_by;
};

std::vector<SongRow> load_song_rows(mc::Database& db) {
    std::vector<SongRow> rows;
    auto hashes = db.get_all_song_hashes();
    rows.reserve(hashes.size());
    for (const auto& ch : hashes) {
        auto meta = db.get_song_meta(ch);
        if (!meta) continue;
        SongRow r;
        r.ch_hex = mc::crypto::to_hex(ch);
        r.title  = meta->title;
        r.artist = meta->artist;
        r.album  = meta->album;
        // Resolve hide status — chain block stays put either way; the
        // hidden_by field just tells the browser which mask to remove
        // when the moderator hits Enter to unhide.
        if      (db.is_song_deleted(ch))           r.hidden_by = "hash";
        else if (db.is_hidden_title (r.title))     r.hidden_by = "title";
        else if (db.is_hidden_album (r.album))     r.hidden_by = "album";
        else if (db.is_hidden_artist(r.artist))    r.hidden_by = "artist";
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const SongRow& a, const SongRow& b){
        if (a.artist != b.artist) return a.artist < b.artist;
        if (a.album  != b.album)  return a.album  < b.album;
        return a.title < b.title;
    });
    return rows;
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool row_matches(const SongRow& r, const std::string& q_lc) {
    if (q_lc.empty()) return true;
    return lower(r.title ).find(q_lc) != std::string::npos
        || lower(r.artist).find(q_lc) != std::string::npos
        || lower(r.album ).find(q_lc) != std::string::npos;
}

// Menu shown after picking a song in the browser. Returns true if the
// hide was applied so the browser can redraw.
bool hide_menu_for_row(mc::api::RatsApi& api, const ModeratorState& mod,
                       const SongRow& row, ModView& mv) {
    if (!mod.logged_in) {
        mv.last_action = "hide: no moderator key on disk";
        mv.last_action_color = CP_WARN;
        return false;
    }
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);
    int w = std::min(max_x - 6, 60);
    int h = 9;
    int y = (max_y - h) / 2;
    int x = (max_x - w) / 2;

    attron(COLOR_PAIR(CP_FOOTER_LBL));
    for (int dy = 0; dy < h; ++dy) {
        move(y + dy, x); for (int dx = 0; dx < w; ++dx) addch(' ');
    }
    attroff(COLOR_PAIR(CP_FOOTER_LBL));
    draw_box(y, x, h, w);
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(y, x + 2, " Hide which scope? ");
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);

    int r = y + 2;
    auto opt = [&](char k, const char* desc, const std::string& val){
        attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        mvprintw(r, x + 2, " %c ", k);
        attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        attron(COLOR_PAIR(CP_VALUE));
        mvprintw(r, x + 6, "%s", desc);
        attroff(COLOR_PAIR(CP_VALUE));
        attron(A_DIM);
        std::string clipped = val;
        int avail = w - 6 - (int)strlen(desc) - 4;
        if (avail < 8) avail = 8;
        if ((int)clipped.size() > avail) clipped = clipped.substr(0, avail - 1) + "…";
        mvprintw(r, x + 6 + (int)strlen(desc) + 2, "(%s)", clipped.c_str());
        attroff(A_DIM);
        ++r;
    };
    opt('T', "Title ",  row.title);
    opt('L', "Album ",  row.album);
    opt('A', "Artist", row.artist);
    opt('H', "Hash  ", row.ch_hex);
    attron(A_DIM);
    mvprintw(y + h - 2, x + 2, "ESC to cancel");
    attroff(A_DIM);
    refresh();

    nodelay(stdscr, FALSE);
    int key = getch();
    nodelay(stdscr, TRUE);

    std::string action, value, note;
    switch (key) {
        case 'T': case 't':
            if (row.title.empty()) {
                mv.last_action = "hide: empty title"; mv.last_action_color = CP_WARN; return false;
            }
            action = "hide_title"; value = row.title;
            note   = "hide title: " + row.title;
            break;
        case 'L': case 'l':
            if (row.album.empty()) {
                mv.last_action = "hide: empty album"; mv.last_action_color = CP_WARN; return false;
            }
            action = "hide_album"; value = row.album;
            note   = "hide album: " + row.album;
            break;
        case 'A': case 'a':
            if (row.artist.empty()) {
                mv.last_action = "hide: empty artist"; mv.last_action_color = CP_WARN; return false;
            }
            action = "hide_artist"; value = row.artist;
            note   = "hide artist: " + row.artist;
            break;
        case 'H': case 'h':
            action = "hide_hash"; value = row.ch_hex;
            note   = "hide hash: " + row.ch_hex.substr(0, 12) + "…";
            break;
        default:
            return false;
    }
    if (!api.publish_mod_action(action, value, mod.kp)) {
        mv.last_action = "hide: publish failed";
        mv.last_action_color = CP_WARN;
        return false;
    }
    if ((int)note.size() > 60) note = note.substr(0, 59) + "…";
    mv.last_action = note + "  (gossiped)";
    mv.last_action_color = CP_OK;
    return true;
}

// Clears whatever mask is currently hiding `row` (per its hidden_by
// field), so Enter on a hidden row toggles back to visible.
bool unhide_row(mc::api::RatsApi& api, const ModeratorState& mod,
                const SongRow& row, ModView& mv) {
    if (row.hidden_by.empty()) return false;
    if (!mod.logged_in) {
        mv.last_action = "unhide: no moderator key on disk";
        mv.last_action_color = CP_WARN;
        return false;
    }
    std::string action, value, note;
    if (row.hidden_by == "hash") {
        action = "unhide_hash"; value = row.ch_hex;
        note   = "unhide hash: " + row.ch_hex.substr(0, 12) + "…";
    } else if (row.hidden_by == "title") {
        action = "unhide_title"; value = row.title;
        note   = "unhide title: " + row.title;
    } else if (row.hidden_by == "album") {
        action = "unhide_album"; value = row.album;
        note   = "unhide album: " + row.album;
    } else if (row.hidden_by == "artist") {
        action = "unhide_artist"; value = row.artist;
        note   = "unhide artist: " + row.artist;
    } else {
        return false;
    }
    if (!api.publish_mod_action(action, value, mod.kp)) {
        mv.last_action = "unhide: publish failed";
        mv.last_action_color = CP_WARN;
        return false;
    }
    if ((int)note.size() > 60) note = note.substr(0, 59) + "…";
    mv.last_action = note + "  (gossiped)";
    mv.last_action_color = CP_OK;
    return true;
}

void action_browse_library(mc::api::RatsApi& api, mc::Database& db,
                            const ModeratorState& mod, ModView& mv) {
    auto rows = load_song_rows(db);
    std::string filter;
    int sel = 0;      // index into the filtered view
    int scroll = 0;   // top row of the filtered view that's visible

    while (true) {
        int max_y = getmaxy(stdscr);
        int max_x = getmaxx(stdscr);

        // Recompute filtered view each iteration so typing reflects.
        const std::string q = lower(filter);
        std::vector<int> view;
        view.reserve(rows.size());
        for (int i = 0; i < (int)rows.size(); ++i)
            if (row_matches(rows[i], q)) view.push_back(i);
        if (view.empty()) sel = 0;
        else if (sel >= (int)view.size()) sel = (int)view.size() - 1;
        if (sel < 0) sel = 0;

        // Counts for the header.
        size_t shown_hidden = 0;
        for (int i : view) if (!rows[i].hidden_by.empty()) ++shown_hidden;

        const int top = 0;
        const int h   = max_y;
        erase();

        // Header bar
        attron(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
        mvhline(top, 0, ' ', max_x);
        mvprintw(top, 2,
                 " Library · %zu of %zu match · %zu hidden · type to filter ",
                 view.size(), rows.size(), shown_hidden);
        attroff(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);

        // Filter line
        attron(COLOR_PAIR(CP_LABEL));
        mvprintw(top + 1, 2, "filter:");
        attroff(COLOR_PAIR(CP_LABEL));
        attron(A_BOLD);
        mvprintw(top + 1, 10, "%s_", filter.c_str());
        attroff(A_BOLD);

        // List header — columns: status (8) | title | artist | album
        const int list_top = top + 3;
        const int list_h   = h - list_top - 1;
        const int status_w = 8;
        const int meta_w   = max_x - 4 - status_w;
        const int title_w  = std::max(20, meta_w * 40 / 100);
        const int artist_w = std::max(15, (meta_w - title_w) * 50 / 100);
        const int album_w  = std::max(15, meta_w - title_w - artist_w);

        attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
        mvprintw(list_top - 1, 2, " %-*s %-*s %-*s %-*s",
                 status_w, "Status",
                 title_w, "Title",
                 artist_w, "Artist",
                 album_w, "Album");
        attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);

        // Keep selection in view
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + list_h) scroll = sel - list_h + 1;
        if (scroll < 0) scroll = 0;

        auto clip = [](std::string s, int w){
            if ((int)s.size() > w) s = s.substr(0, std::max(0, w - 1)) + "…";
            return s;
        };
        for (int i = 0; i < list_h && scroll + i < (int)view.size(); ++i) {
            const SongRow& r = rows[view[scroll + i]];
            int y = list_top + i;
            bool active = (scroll + i == sel);
            if (active) {
                attron(COLOR_PAIR(CP_TAB_ON) | A_BOLD);
                mvhline(y, 1, ' ', max_x - 2);
            }
            // Status badge: red "HIDDEN" for masked rows, dim "ok" for
            // visible rows. The status column tells the moderator at a
            // glance which action Enter will perform on that row.
            const char* status_text = r.hidden_by.empty()
                                          ? "visible"
                                          : r.hidden_by.c_str();
            int status_color = r.hidden_by.empty() ? CP_VALUE : CP_WARN;
            int status_attr  = r.hidden_by.empty() ? A_DIM    : A_BOLD;
            if (!active) {
                attron(COLOR_PAIR(status_color) | status_attr);
            } else {
                attron(A_BOLD);
            }
            mvprintw(y, 2, " %-*s", status_w, status_text);
            if (!active) {
                attroff(COLOR_PAIR(status_color) | status_attr);
            } else {
                attroff(A_BOLD);
            }
            mvprintw(y, 2 + status_w + 1, " %-*s %-*s %-*s",
                     title_w, clip(r.title, title_w).c_str(),
                     artist_w, clip(r.artist, artist_w).c_str(),
                     album_w, clip(r.album, album_w).c_str());
            if (active) attroff(COLOR_PAIR(CP_TAB_ON) | A_BOLD);
        }

        // Footer — Enter does the contextually-right thing.
        const bool sel_hidden =
            !view.empty() && !rows[view[sel]].hidden_by.empty();
        draw_footer_bar({
            {"↑↓",    "Move"},
            {"Enter", sel_hidden ? "Unhide" : "Hide…"},
            {"Bksp",  "Filter"},
            {"ESC",   "Back"},
        });
        refresh();

        nodelay(stdscr, FALSE);
        int key = getch();
        nodelay(stdscr, TRUE);

        if (key == 27 /*ESC*/) break;
        if (key == KEY_UP)    { if (sel > 0) --sel; continue; }
        if (key == KEY_DOWN)  { if (sel + 1 < (int)view.size()) ++sel; continue; }
        if (key == KEY_PPAGE) { sel = std::max(0, sel - list_h); continue; }
        if (key == KEY_NPAGE) { sel = std::min((int)view.size() - 1, sel + list_h); continue; }
        if (key == KEY_HOME)  { sel = 0; continue; }
        if (key == KEY_END)   { sel = (int)view.size() - 1; continue; }
        if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            if (!view.empty()) {
                const SongRow& r = rows[view[sel]];
                bool changed = r.hidden_by.empty()
                                   ? hide_menu_for_row(api, mod, r, mv)
                                   : unhide_row(api, mod, r, mv);
                if (changed) rows = load_song_rows(db);
            }
            continue;
        }
        if (key == KEY_BACKSPACE || key == 127 || key == 8) {
            if (!filter.empty()) filter.pop_back();
            continue;
        }
        if (key >= 32 && key < 127) {
            filter.push_back(static_cast<char>(key));
            sel = 0; scroll = 0;
            continue;
        }
    }
}

// ---- KYC review modal (K key) ---------------------------------------
//
// Lists every file in <data_dir>/kyc/, parses the artist's wallet
// address out of the filename (we embed the full 40-hex at upload
// time), and exposes review actions: O to open the file in the OS
// default viewer, A to approve + release the full escrow balance to
// the artist's address, D to reject (delete the file). The moderator
// can come back to this screen anytime; an approve mints a transfer
// inside the same db.write so the listing refreshes naturally.

struct KycRow {
    std::filesystem::path path;        // absolute path on disk
    std::string           filename;    // basename
    std::string           artist_hex;  // 40-hex if parseable, else ""
};

std::vector<KycRow> load_kyc_rows(const std::string& data_dir) {
    std::vector<KycRow> rows;
    const fs::path inbox = fs::path(data_dir) / "kyc";
    std::error_code ec;
    if (!fs::exists(inbox, ec)) return rows;
    for (auto& e : fs::directory_iterator(inbox, ec)) {
        if (!e.is_regular_file()) continue;
        KycRow r;
        r.path     = e.path();
        r.filename = e.path().filename().string();
        // Filename shape: YYYYMMDD_hhmmss_<40hex>_<rest>.ext  — pull the
        // 40-hex out so the approve action has a real target.
        const auto& f = r.filename;
        if (f.size() > 16 + 1 + 40 + 1) {
            const size_t start = 16;
            if (f[start] == '_') {
                std::string maybe = f.substr(start + 1, 40);
                bool hex = maybe.size() == 40;
                for (char c : maybe) {
                    if (!((c >= '0' && c <= '9') ||
                          (c >= 'a' && c <= 'f') ||
                          (c >= 'A' && c <= 'F'))) {
                        hex = false; break;
                    }
                }
                if (hex && f[start + 1 + 40] == '_') r.artist_hex = maybe;
            }
        }
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(),
              [](const KycRow& a, const KycRow& b){
                  return a.filename < b.filename;
              });
    return rows;
}

void open_in_os(const std::filesystem::path& p) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", p.string().c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open '" + p.string() + "' &";
    std::system(cmd.c_str());
#endif
}

// Decrypt-on-open helper used by the inbox review screens. If the file
// at `src` is plaintext (no ECIES magic) we return its path unchanged.
// Otherwise we attempt to decrypt with the logged-in moderator's key
// and stash the plaintext in a fresh temp file so the OS viewer can
// open it. Returns an empty path on failure (not encrypted to me, GCM
// auth failure, etc.). The temp file is intentionally leaked — the
// user closes the viewer when done; future passes can sweep
// %TEMP%/bopwire_decrypt_* on TUI shutdown.
std::filesystem::path decrypt_to_temp(const std::filesystem::path& src,
                                       const ModeratorState& mod) {
    std::ifstream f(src, std::ios::binary);
    if (!f) return {};
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    f.close();

    if (!mc::crypto::ecies_looks_encrypted(bytes.data(), bytes.size())) {
        return src;
    }
    if (!mod.logged_in) return {};

    auto pt = mc::crypto::ecies_decrypt(bytes, mod.kp.address, mod.kp.private_key);
    if (!pt) return {};

    auto base = src.filename().string();
    // Strip the .enc suffix so the OS picks the right viewer by extension.
    if (base.size() > 4 &&
        base.substr(base.size() - 4) == ".enc") {
        base.resize(base.size() - 4);
    }
    std::error_code ec;
    auto tmp_dir = std::filesystem::temp_directory_path(ec) /
                   ("bopwire_decrypt_" + std::to_string(
                       std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(tmp_dir, ec);
    auto out_path = tmp_dir / base;
    std::ofstream o(out_path, std::ios::binary | std::ios::trunc);
    if (!o) return {};
    o.write(reinterpret_cast<const char*>(pt->data()),
            static_cast<std::streamsize>(pt->size()));
    o.close();
    return out_path;
}

void action_review_kyc(mc::Database& db, const ModeratorState& mod,
                        ModView& mv, const std::string& data_dir) {
    auto rows = load_kyc_rows(data_dir);
    int sel = 0, scroll = 0;
    std::string note;
    int note_color = CP_OK;

    while (true) {
        int max_y = getmaxy(stdscr);
        int max_x = getmaxx(stdscr);

        if (sel >= (int)rows.size()) sel = std::max(0, (int)rows.size() - 1);
        if (sel < 0) sel = 0;

        erase();
        attron(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
        mvhline(0, 0, ' ', max_x);
        mvprintw(0, 2, " KYC review · %zu pending ", rows.size());
        attroff(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);

        // Split into left list + right detail pane.
        const int left_w  = std::min(max_x / 2, 56);
        const int right_x = left_w;
        const int right_w = max_x - right_x;
        const int top     = 2;
        const int h       = max_y - top - 1;

        // Left: list of filenames
        attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
        mvprintw(top - 1, 2, "Files");
        attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);

        const int list_h = h;
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + list_h) scroll = sel - list_h + 1;
        if (scroll < 0) scroll = 0;

        if (rows.empty()) {
            attron(A_DIM);
            mvprintw(top + 1, 4, "(no pending KYC forms)");
            attroff(A_DIM);
        }
        for (int i = 0; i < list_h && scroll + i < (int)rows.size(); ++i) {
            const KycRow& r = rows[scroll + i];
            int y = top + i;
            bool active = (scroll + i == sel);
            if (active) {
                attron(COLOR_PAIR(CP_TAB_ON) | A_BOLD);
                mvhline(y, 1, ' ', left_w - 2);
            }
            std::string disp = r.filename;
            int avail = left_w - 4;
            if ((int)disp.size() > avail) disp = disp.substr(0, avail - 1) + "…";
            mvprintw(y, 2, " %s", disp.c_str());
            if (active) attroff(COLOR_PAIR(CP_TAB_ON) | A_BOLD);
        }

        // Right: detail / actions for the selected row
        attron(COLOR_PAIR(CP_BORDER));
        mvvline(top, right_x - 1, ACS_VLINE, h);
        attroff(COLOR_PAIR(CP_BORDER));

        if (!rows.empty()) {
            const KycRow& r = rows[sel];
            int rr = top;

            attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
            mvprintw(rr++, right_x + 1, "Detail");
            attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
            ++rr;

            auto kv = [&](const char* k, const std::string& v){
                attron(COLOR_PAIR(CP_LABEL));
                mvprintw(rr, right_x + 1, "%-10s", k);
                attroff(COLOR_PAIR(CP_LABEL));
                std::string val = v;
                int avail = right_w - 14;
                if ((int)val.size() > avail) val = val.substr(0, avail - 1) + "…";
                attron(COLOR_PAIR(CP_VALUE));
                mvprintw(rr, right_x + 12, "%s", val.c_str());
                attroff(COLOR_PAIR(CP_VALUE));
                ++rr;
            };
            kv("Filename", r.filename);
            kv("Path",     r.path.string());
            if (r.artist_hex.empty()) {
                attron(COLOR_PAIR(CP_WARN));
                mvprintw(rr++, right_x + 1,
                         "no wallet address parsed from filename");
                attroff(COLOR_PAIR(CP_WARN));
            } else {
                kv("Artist",   r.artist_hex);
                Address artist_addr{};
                if (mc::crypto::parse_address(r.artist_hex, artist_addr)) {
                    const Address esc =
                        mc::crypto::escrow_address_for(artist_addr);
                    const uint64_t bal = db.get_balance(esc);
                    kv("Escrow",  mc::crypto::to_checksum_hex(esc));
                    kv("Balance", mc::Ledger::format_balance(bal) + " mc");
                }
            }
            ++rr;

            attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
            mvprintw(rr++, right_x + 1, "Actions");
            attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
            auto act_line = [&](char k, const char* d){
                attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
                mvprintw(rr, right_x + 1, " %c ", k);
                attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
                attron(COLOR_PAIR(CP_VALUE));
                mvprintw(rr, right_x + 5, "%s", d);
                attroff(COLOR_PAIR(CP_VALUE));
                ++rr;
            };
            act_line('O', "Open in OS default viewer");
            act_line('A', "Approve + release full escrow");
            act_line('D', "Reject (delete file)");

            if (!note.empty()) {
                ++rr;
                attron(COLOR_PAIR(note_color) | A_BOLD);
                mvprintw(rr++, right_x + 1, "%s", note.c_str());
                attroff(COLOR_PAIR(note_color) | A_BOLD);
            }
        }

        draw_footer_bar({
            {"↑↓",    "Move"},
            {"O",     "Open"},
            {"A",     "Approve"},
            {"D",     "Reject"},
            {"ESC",   "Back"},
        });
        refresh();

        nodelay(stdscr, FALSE);
        int key = getch();
        nodelay(stdscr, TRUE);

        if (key == 27) break;
        if (key == KEY_UP)    { if (sel > 0) --sel; continue; }
        if (key == KEY_DOWN)  { if (sel + 1 < (int)rows.size()) ++sel; continue; }
        if (key == KEY_PPAGE) { sel = std::max(0, sel - list_h); continue; }
        if (key == KEY_NPAGE) { sel = std::min((int)rows.size() - 1, sel + list_h); continue; }

        if (rows.empty()) continue;
        const KycRow& r = rows[sel];

        if (key == 'O' || key == 'o') {
            // The file on disk is ECIES-encrypted to the active mods if
            // any were on chain at submit time. decrypt_to_temp inlines
            // the no-op for plaintext files and writes a temp PDF when
            // it has to decrypt.
            auto open_path = decrypt_to_temp(r.path, mod);
            if (open_path.empty()) {
                note = "open: file is encrypted to other moderators (or auth failed)";
                note_color = CP_WARN;
                continue;
            }
            open_in_os(open_path);
            note = open_path == r.path
                ? ("opened " + r.filename)
                : ("decrypted + opened " + open_path.filename().string());
            note_color = CP_OK;
            continue;
        }
        if (key == 'A' || key == 'a') {
            if (!mod.logged_in) {
                note = "no moderator key — release aborted";
                note_color = CP_WARN; continue;
            }
            if (r.artist_hex.empty()) {
                note = "no artist address in filename — open + use main 'E' flow";
                note_color = CP_WARN; continue;
            }
            Address artist_addr{};
            if (!mc::crypto::parse_address(r.artist_hex, artist_addr)) {
                note = "could not parse artist address";
                note_color = CP_WARN; continue;
            }
            const Address esc = mc::crypto::escrow_address_for(artist_addr);
            const uint64_t bal = db.get_balance(esc);
            if (bal == 0) {
                note = "escrow already empty for this artist";
                note_color = CP_WARN; continue;
            }
            leveldb::WriteBatch batch;
            mc::Ledger ledger(db);
            if (!ledger.transfer(batch, esc, artist_addr, bal)) {
                note = "ledger refused transfer";
                note_color = CP_WARN; continue;
            }
            db.write(batch);
            // Move the file to <data_dir>/kyc/approved/ so it disappears
            // from the pending list but the audit trail survives.
            const fs::path approved = r.path.parent_path() / "approved";
            std::error_code ec;
            fs::create_directories(approved, ec);
            fs::rename(r.path, approved / r.path.filename(), ec);
            note = "released " + mc::Ledger::format_balance(bal) + " mc";
            note_color = CP_OK;
            mv.last_action = "kyc approve: " + r.artist_hex.substr(0, 12)
                          + "… +" + mc::Ledger::format_balance(bal) + " mc";
            mv.last_action_color = CP_OK;
            mv.kyc_files = scan_kyc_inbox(data_dir);
            rows = load_kyc_rows(data_dir);
            continue;
        }
        if (key == 'D' || key == 'd') {
            // Reject = move to kyc/rejected/ so the moderator can
            // recover if they hit the key by accident.
            const fs::path rejected = r.path.parent_path() / "rejected";
            std::error_code ec;
            fs::create_directories(rejected, ec);
            fs::rename(r.path, rejected / r.path.filename(), ec);
            note = "rejected " + r.filename;
            note_color = CP_WARN;
            mv.last_action = "kyc reject: " + r.filename;
            mv.last_action_color = CP_WARN;
            mv.kyc_files = scan_kyc_inbox(data_dir);
            rows = load_kyc_rows(data_dir);
            continue;
        }
    }
}

// Helper shared by the proposal-building actions: sign + enqueue. Sets
// `mv.last_action` and returns true on success.
bool submit_proposal_tx(mc::Database& db, const ModeratorState& mod,
                         mc::ProposalTx& tx, ModView& mv,
                         const std::string& action_label) {
    if (db.get_mod_level(mod.kp.address)
            < static_cast<uint8_t>(mc::ModLevel::OP)) {
        mv.last_action = action_label + ": need OP level to propose";
        mv.last_action_color = CP_WARN;
        return false;
    }

    tx.proposer        = mod.kp.address;
    tx.proposer_pubkey = mod.kp.public_key;
    tx.nonce           = db.get_nonce(mod.kp.address);

    auto msg_bytes = tx.sign_message();
    auto msg_hash  = mc::crypto::sha256(msg_bytes.data(), msg_bytes.size());
    tx.signature   = mc::crypto::sign_ecdsa(msg_hash, mod.kp.private_key);
    if (!tx.verify_signature()) {
        mv.last_action = action_label + ": internal sign/verify mismatch";
        mv.last_action_color = CP_WARN;
        return false;
    }

    auto h = tx.tx_hash();
    if (!db.put_pending_tx(h, tx.serialize())) {
        mv.last_action = action_label + ": failed to enqueue tx";
        mv.last_action_color = CP_WARN;
        return false;
    }
    return true;
}

void action_release_escrow(mc::Database& db, const ModeratorState& mod,
                            ModView& mv) {
    if (!mod.logged_in) {
        mv.last_action = "release: no moderator key";
        mv.last_action_color = CP_WARN;
        return;
    }

    // The escrow source is derived from the artist's address so a
    // moderator can ONLY shift tokens from `escrow_address_for(artist)`
    // to that artist, never from an arbitrary source to an arbitrary
    // destination. Proposal-driven means a single moderator can't even
    // do that one transfer — quorum of the active mods has to vote YES
    // before the chain executes the move.
    std::string artist_hex;
    if (!prompt_string("Artist wallet address (40 hex)", artist_hex, 40))
        return;
    Address artist_addr{};
    if (!mc::crypto::parse_address(artist_hex, artist_addr)) {
        mv.last_action = "release: bad artist address";
        mv.last_action_color = CP_WARN; return;
    }
    const Address  esc_addr = mc::crypto::escrow_address_for(artist_addr);
    const uint64_t esc_bal  = db.get_balance(esc_addr);
    if (esc_bal == 0) {
        mv.last_action = "release: escrow empty for that artist";
        mv.last_action_color = CP_WARN; return;
    }

    std::string amt_str;
    if (!prompt_string(
            "Amount (blank=all, e.g. 12.5)", amt_str, 32))
        return;
    uint64_t amount;
    if (amt_str.empty() || amt_str == "all" || amt_str == "ALL") {
        amount = esc_bal;
    } else {
        if (!mc::Ledger::parse_balance(amt_str, amount)) {
            mv.last_action = "release: bad amount";
            mv.last_action_color = CP_WARN; return;
        }
        if (amount > esc_bal) {
            mv.last_action =
                "release: amount > escrow balance (cap "
                + mc::Ledger::format_balance(esc_bal) + " mc)";
            mv.last_action_color = CP_WARN; return;
        }
    }

    mc::ProposalTx tx{};
    tx.kind         = static_cast<uint8_t>(mc::ProposalKind::RELEASE_ESCROW);
    tx.target_addr  = artist_addr;
    tx.amount       = amount;
    if (!submit_proposal_tx(db, mod, tx, mv, "release")) return;

    const size_t active_n = db.list_active_moderators().size();
    const size_t needed   = (active_n / 2) + 1;
    mv.last_action = "release: proposal queued, "
                   + mc::Ledger::format_balance(amount) + " mc → "
                   + artist_hex.substr(0, 12) + "…  ("
                   + std::to_string(needed) + "/" + std::to_string(active_n)
                   + " YES needed)";
    mv.last_action_color = CP_OK;
}

// ---- Proposal browser (V key) ---------------------------------------
//
// Lists every PENDING chain-level proposal so an OP can see what
// needs their attention and cast a YES vote with a single keypress.
// Uses arrow keys for selection, Enter to vote, q/ESC to leave.

struct ProposalRow {
    mc::Hash256    prop_hash;
    mc::ProposalTx tx;
    size_t         votes;
    bool           already_voted;
};

std::vector<ProposalRow> load_proposal_rows(mc::Database& db,
                                            const Address& self) {
    std::vector<ProposalRow> out;
    auto hashes = db.list_pending_proposals();
    out.reserve(hashes.size());
    for (const auto& h : hashes) {
        auto raw = db.get_proposal(h);
        if (!raw) continue;
        mc::ProposalTx tx;
        if (!mc::ProposalTx::deserialize(raw->data(), raw->size(), tx)) continue;
        ProposalRow r;
        r.prop_hash      = h;
        r.tx             = tx;
        r.votes          = db.count_proposal_votes(h);
        r.already_voted  = db.has_proposal_vote(h, self);
        out.push_back(std::move(r));
    }
    return out;
}

std::string format_proposal_summary(const ProposalRow& r,
                                    size_t needed, size_t active_n) {
    const auto kind = static_cast<mc::ProposalKind>(r.tx.kind);
    std::string body;
    switch (kind) {
        case mc::ProposalKind::HIDE_CONTENT: {
            body = "HIDE  " + mc::crypto::to_hex(r.tx.target_hash).substr(0, 16) + "…";
            break;
        }
        case mc::ProposalKind::RELEASE_ESCROW: {
            body = "RELEASE  "
                 + mc::Ledger::format_balance(r.tx.amount) + " mc → "
                 + mc::crypto::to_checksum_hex(r.tx.target_addr).substr(0, 14) + "…";
            break;
        }
        case mc::ProposalKind::VOTE_YES:
            body = "VOTE? (orphaned)"; break;
    }
    char votes_buf[64];
    std::snprintf(votes_buf, sizeof(votes_buf), "  [%zu/%zu of %zu YES]",
                  r.votes, needed, active_n);
    body += votes_buf;
    if (r.already_voted) body += "  ✓ you voted";
    return body;
}

void action_view_proposals(mc::Database& db, const ModeratorState& mod,
                           ModView& mv) {
    if (!mod.logged_in) {
        mv.last_action = "vote: not logged in";
        mv.last_action_color = CP_WARN;
        return;
    }
    if (db.get_mod_level(mod.kp.address)
            < static_cast<uint8_t>(mc::ModLevel::OP)) {
        mv.last_action = "vote: need OP level to cast votes";
        mv.last_action_color = CP_WARN;
        return;
    }

    int sel = 0;
    while (true) {
        auto rows = load_proposal_rows(db, mod.kp.address);
        const size_t active_n = db.list_active_moderators().size();
        const size_t needed   = (active_n / 2) + 1;

        int max_y = getmaxy(stdscr);
        int max_x = getmaxx(stdscr);
        if (rows.empty()) sel = 0;
        else if (sel >= (int)rows.size()) sel = (int)rows.size() - 1;
        if (sel < 0) sel = 0;

        erase();
        attron(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
        mvhline(0, 0, ' ', max_x);
        mvprintw(0, 2, " Pending proposals · %zu open · quorum needs %zu/%zu YES ",
                 rows.size(), needed, active_n);
        attroff(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);

        const int list_top = 2;
        const int list_h   = max_y - list_top - 2;

        if (rows.empty()) {
            attron(A_DIM);
            mvprintw(list_top + 1, 4,
                     "No open proposals. Press E to propose an escrow");
            mvprintw(list_top + 2, 4,
                     "release; HIDE proposals are built via H on the");
            mvprintw(list_top + 3, 4,
                     "library page (per-song).");
            attroff(A_DIM);
        } else {
            for (size_t i = 0; i < rows.size() && (int)i < list_h; ++i) {
                bool is_sel = (int)i == sel;
                if (is_sel) attron(A_REVERSE);
                mvprintw(list_top + (int)i, 2, " %s",
                         format_proposal_summary(rows[i], needed, active_n).c_str());
                if (is_sel) attroff(A_REVERSE);
            }
        }

        // Footer hint
        attron(COLOR_PAIR(CP_FOOTER_LBL));
        mvhline(max_y - 1, 0, ' ', max_x);
        attroff(COLOR_PAIR(CP_FOOTER_LBL));
        attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        mvprintw(max_y - 1, 1, " ↑↓ ");
        attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        attron(COLOR_PAIR(CP_FOOTER_LBL));
        mvprintw(max_y - 1, 6, " select ");
        attroff(COLOR_PAIR(CP_FOOTER_LBL));
        attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        mvprintw(max_y - 1, 15, " Enter ");
        attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        attron(COLOR_PAIR(CP_FOOTER_LBL));
        mvprintw(max_y - 1, 22, " cast YES ");
        attroff(COLOR_PAIR(CP_FOOTER_LBL));
        attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        mvprintw(max_y - 1, 33, " Q ");
        attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
        attron(COLOR_PAIR(CP_FOOTER_LBL));
        mvprintw(max_y - 1, 36, " back ");
        attroff(COLOR_PAIR(CP_FOOTER_LBL));

        refresh();

        int key = getch();
        if (key == 'q' || key == 'Q' || key == 27 /*ESC*/) break;
        if (key == KEY_UP)   { if (sel > 0) --sel; continue; }
        if (key == KEY_DOWN) { if (sel + 1 < (int)rows.size()) ++sel; continue; }
        if ((key == '\n' || key == '\r' || key == KEY_ENTER) && !rows.empty()) {
            const auto& r = rows[sel];
            if (r.already_voted) {
                mv.last_action = "vote: already cast on that proposal";
                mv.last_action_color = CP_WARN;
                continue;
            }
            // Build a VOTE_YES tx referencing the proposal hash.
            mc::ProposalTx vote{};
            vote.kind        = static_cast<uint8_t>(mc::ProposalKind::VOTE_YES);
            vote.target_hash = r.prop_hash;
            if (!submit_proposal_tx(db, mod, vote, mv, "vote")) continue;
            mv.last_action = "vote: YES queued ("
                           + std::to_string(r.votes + 1) + "/"
                           + std::to_string(needed) + ")";
            mv.last_action_color = CP_OK;
        }
    }
}

// ---- Login / logout (Phase 1 of the IRC-style moderator system) ----

void action_login(mc::Database& db, const std::string& data_dir,
                  ModeratorState& mod, ModView& mv) {
    if (mod.logged_in) {
        mv.last_action = "login: already authenticated as "
                       + mod.addr_hex.substr(0, 12) + "…";
        mv.last_action_color = CP_OK;
        return;
    }

    auto try_kp_for_login = [&](const mc::crypto::KeyPair& kp) -> bool {
        uint8_t lvl = db.get_mod_level(kp.address);
        if (lvl == 0) return false;
        mod.kp        = kp;
        mod.addr_hex  = mc::crypto::to_checksum_hex(kp.address);
        mod.level     = lvl;
        mod.logged_in = true;
        return true;
    };

    // Path 0: on-disk founder.seed (BIP39 mnemonic written by
    // action_bootstrap_founder). Loaded automatically — no prompt at
    // all, just a tap on L. The seed file lives next to the chain
    // data dir; deleting it forces a fresh bootstrap.
    {
        const auto seed_path = founder_seed_path(data_dir);
        std::ifstream sf(seed_path);
        if (sf.is_open()) {
            std::string mnemonic;
            std::getline(sf, mnemonic);
            sf.close();
            // Strip trailing CR/whitespace defensively.
            while (!mnemonic.empty()
                   && (mnemonic.back() == '\r' || mnemonic.back() == '\n'
                       || mnemonic.back() == ' ')) {
                mnemonic.pop_back();
            }
            if (!mnemonic.empty() && mc::crypto::bip39_validate(mnemonic)) {
                auto kp_opt = mc::crypto::bip39_mnemonic_to_keypair(mnemonic, "");
                std::fill(mnemonic.begin(), mnemonic.end(), '\0');
                if (kp_opt) {
                    if (try_kp_for_login(*kp_opt)) {
                        mv.last_action = "login: founder seed (level "
                                       + std::to_string(mod.level) + ")";
                        mv.last_action_color = CP_OK;
                        return;
                    }
                    // Disk seed is well-formed but the bound GRANT has
                    // not yet applied on chain. Common right after the
                    // first B-press: producer mints within ~30 s of a
                    // pending tx. Tell the operator instead of falling
                    // through to the prompt (which would also fail).
                    mv.last_action = "login: GRANT not yet on chain — "
                                     "wait ~30s then press L again";
                    mv.last_action_color = CP_WARN;
                    return;
                }
            }
        }
    }

    // The login prompt accepts either:
    //   1. A 12-word BIP39 mnemonic typed in (for restoring on a new
    //      machine without copying founder.seed).
    //   2. A legacy passphrase (any length, not pure 64-hex), which
    //      gets PBKDF2'd with the founder salt to yield a 32-byte seed.
    //   3. A raw 64-hex private key, used as-is.
    std::string entered;
    if (!prompt_secret("BIP39 mnemonic, passphrase, or 64-hex private key",
                       entered, 256)) {
        mv.last_action = "login: cancelled";
        mv.last_action_color = CP_WARN;
        return;
    }
    if (entered.empty()) {
        mv.last_action = "login: empty input";
        mv.last_action_color = CP_WARN;
        return;
    }

    // Path 1a: input is a valid BIP39 mnemonic.
    if (mc::crypto::bip39_validate(entered)) {
        auto kp_opt = mc::crypto::bip39_mnemonic_to_keypair(entered, "");
        if (kp_opt && try_kp_for_login(*kp_opt)) {
            mv.last_action = "login: mnemonic ok (level "
                           + std::to_string(mod.level) + ")";
            mv.last_action_color = CP_OK;
            return;
        }
    }

    // Path 1b: input looks like a username — look up on chain, then
    // prompt for the mnemonic to prove ownership.
    {
        std::string lower = entered;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool looks_like_username =
            lower.size() >= 3 && lower.size() <= 30 &&
            (lower[0] >= 'a' && lower[0] <= 'z');
        if (looks_like_username) {
            for (char c : lower) {
                if (!((c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') ||
                      c == '_')) {
                    looks_like_username = false;
                    break;
                }
            }
        }
        if (looks_like_username) {
            auto addr = db.lookup_username(lower);
            if (addr) {
                std::string mn;
                if (prompt_secret(("Mnemonic for username '" + lower + "'").c_str(),
                                  mn, 256)
                    && mc::crypto::bip39_validate(mn)) {
                    auto kp_opt = mc::crypto::bip39_mnemonic_to_keypair(mn, "");
                    std::fill(mn.begin(), mn.end(), '\0');
                    if (kp_opt
                        && std::memcmp(kp_opt->address.data(),
                                       addr->data(), 20) == 0
                        && try_kp_for_login(*kp_opt)) {
                        mv.last_action = "login: '" + lower + "' (level "
                                       + std::to_string(mod.level) + ")";
                        mv.last_action_color = CP_OK;
                        return;
                    }
                }
            }
        }
    }

    // Path 1b ("PBKDF2 passphrase → keypair_from_seed") and Path 2
    // ("raw 64-hex priv → keypair_from_hex") were removed alongside
    // the SHA-256-rehashed keypair_from_* helpers themselves. Their
    // rehash step silently produced a DIFFERENT key than every
    // standard secp256k1 / EVM tool computed from the same bytes, so
    // the moderator level the operator typed against the chain didn't
    // correspond to any address an external recovery tool could
    // produce. The only login routes now are Path 1a (mnemonic) and
    // the username → mnemonic prompt above.

    // Deliberately vague — don't tell the operator whether the input
    // was malformed, parsed but unknown, or known-but-revoked. Same
    // failure reason for all three closes a fingerprinting side channel
    // someone testing keys against a public node could otherwise use.
    mv.last_action = "login: rejected";
    mv.last_action_color = CP_WARN;
}

void action_logout(ModeratorState& mod, ModView& mv) {
    if (!mod.logged_in) {
        mv.last_action = "logout: not logged in";
        mv.last_action_color = CP_WARN;
        return;
    }
    const std::string was = mod.addr_hex.substr(0, 12);
    wipe_session_key(mod);
    mv.last_action = "logout: cleared session for " + was + "…";
    mv.last_action_color = CP_OK;
}

// Path of the on-disk founder seed file. Plaintext BIP39 mnemonic.
// Living next to the chain data is intentional: an attacker with disk
// access already has the chain's full ledger, so the seed's marginal
// secrecy is bounded by FS permissions and full-disk encryption. A
// future iteration can wrap it under a password.
std::string founder_seed_path(const std::string& data_dir) {
    return data_dir + "/founder.seed";
}

void action_bootstrap_founder(mc::Database& db,
                              const std::string& data_dir,
                              ModView& mv) {
    // Bootstrap is one-shot: once a founder is recorded on chain there
    // is no second bootstrap window. The chain rejects future
    // self-grants regardless of what the TUI does, but check here too
    // so we can give a useful error.
    if (db.get_founder().has_value()) {
        mv.last_action = "bootstrap: founder already recorded on chain";
        mv.last_action_color = CP_WARN;
        return;
    }
    const auto seed_path = founder_seed_path(data_dir);
    if (fs::exists(seed_path)) {
        mv.last_action = "bootstrap: seed already saved — press L to log in"
                       " (GRANT applies in the next block)";
        mv.last_action_color = CP_WARN;
        return;
    }

    // Generate a fresh BIP39 mnemonic and derive the keypair. Like a
    // standard wallet first-launch: we generate, you write it down, you
    // decide whether to also save to disk for convenience.
    std::string mnemonic = mc::crypto::bip39_generate_12();
    if (mnemonic.empty()) {
        mv.last_action = "bootstrap: entropy source failed";
        mv.last_action_color = CP_WARN;
        return;
    }
    // Use the SAME derivation path as action_login (BIP32 m/44'/19779'/0'/0/0)
    // so the address the GRANT registers matches the address login resolves
    // for the same mnemonic. Previously bootstrap used keypair_from_seed
    // over the raw BIP39 seed[0:32] while login used bip39_mnemonic_to_keypair
    // → BIP32 derive, which produced two different addresses for the same
    // 12 words. The GRANT then landed on the wrong address and login
    // permanently said "GRANT not yet on chain".
    auto kp_opt = mc::crypto::bip39_mnemonic_to_keypair(mnemonic, "");
    if (!kp_opt) {
        mv.last_action = "bootstrap: BIP32 derivation failed";
        mv.last_action_color = CP_WARN;
        std::fill(mnemonic.begin(), mnemonic.end(), '\0');
        return;
    }
    auto kp = *kp_opt;

    // Render the seed on screen until the operator acknowledges. This
    // is the only chance to write the words down off-disk; if the
    // operator skips the save-to-disk step below they MUST have copied
    // it now or the founder key is lost.
    {
        erase();
        int max_y = getmaxy(stdscr);
        int max_x = getmaxx(stdscr);
        attron(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
        mvhline(0, 0, ' ', max_x);
        mvprintw(0, 2, " Founder seed phrase ");
        attroff(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
        attron(A_BOLD);
        mvprintw(3, 4, "Write these 12 words down on paper. This is the only");
        mvprintw(4, 4, "off-disk copy. Anyone with these words owns the founder");
        mvprintw(5, 4, "wallet — keep them somewhere safe.");
        attroff(A_BOLD);
        attron(COLOR_PAIR(CP_OK) | A_BOLD);
        mvprintw(8, 4, "%s", mnemonic.c_str());
        attroff(COLOR_PAIR(CP_OK) | A_BOLD);
        mvprintw(10, 4, "Press any key once you've written it down.");
        refresh();
        nodelay(stdscr, FALSE);
        getch();
        nodelay(stdscr, TRUE);
        erase();
        refresh();
    }

    // Ask whether to also persist to disk (default: yes, for ease).
    std::string save_choice;
    bool save_to_disk = true;
    if (prompt_string("Save seed to founder.seed on disk? (y/n)",
                      save_choice, 4)) {
        save_to_disk = !(save_choice == "n" || save_choice == "N" ||
                          save_choice == "no" || save_choice == "No");
    }

    // Optional username. The chain runs the well-formedness rules
    // (3..30 chars, [a-z0-9_]+, first char must be a letter). Empty
    // skips the username registration entirely.
    std::string username;
    prompt_string("Founder username (optional, blank to skip)",
                  username, 32);
    for (auto& c : username) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (save_to_disk) {
        std::ofstream sf(seed_path, std::ios::trunc);
        if (!sf.is_open()) {
            mv.last_action = "bootstrap: could not open " + seed_path + " for write";
            mv.last_action_color = CP_WARN;
            std::fill(mnemonic.begin(), mnemonic.end(), '\0');
            return;
        }
        sf << mnemonic << "\n";
        sf.close();
    }
    std::fill(mnemonic.begin(), mnemonic.end(), '\0');

    // Build the GRANT FOUNDER tx, self-signed.
    mc::ModeratorOpTx tx{};
    tx.op_code        = static_cast<uint8_t>(mc::ModOpCode::GRANT);
    tx.level          = static_cast<uint8_t>(mc::ModLevel::FOUNDER);
    tx.subject        = kp.address;
    tx.subject_pubkey = kp.public_key;
    tx.proposer       = kp.address;
    tx.proposer_pubkey = kp.public_key;
    // Nonce: 0 is correct for the first tx of a brand-new address.
    tx.nonce = db.get_nonce(kp.address);

    auto msg_bytes  = tx.sign_message();
    auto msg_hash   = mc::crypto::sha256(msg_bytes.data(), msg_bytes.size());
    tx.signature    = mc::crypto::sign_ecdsa(msg_hash, kp.private_key);

    // Sanity-check before letting the mempool get it.
    if (!tx.verify_signature()) {
        mv.last_action = "bootstrap: internal sign/verify mismatch";
        mv.last_action_color = CP_WARN;
        return;
    }

    // If the operator picked a username, build + sign a UsernameTx
    // alongside the GRANT. Both end up in the same block. The
    // UsernameTx uses nonce 1 because the GRANT consumes nonce 0.
    mc::UsernameTx un_tx{};
    bool want_username = !username.empty();
    if (want_username) {
        un_tx.name         = username;
        un_tx.owner        = kp.address;
        un_tx.owner_pubkey = kp.public_key;
        un_tx.nonce        = 1; // after the GRANT
        auto un_msg  = un_tx.sign_message();
        auto un_hash = mc::crypto::sha256(un_msg.data(), un_msg.size());
        un_tx.signature = mc::crypto::sign_ecdsa(un_hash, kp.private_key);
        if (!un_tx.verify_signature()) {
            mv.last_action = "bootstrap: username sign/verify mismatch";
            mv.last_action_color = CP_WARN;
            return;
        }
    }

    // Wipe the private key from kp now that all signatures are built.
    {
        volatile uint8_t* p = kp.private_key.data();
        for (size_t i = 0; i < kp.private_key.size(); ++i) p[i] = 0;
        kp.private_key.assign(kp.private_key.size(), 0);
    }

    // Drop into the mempool so the next block this node mines includes
    // the GRANT (and optionally the username).
    auto h = tx.tx_hash();
    if (!db.put_pending_tx(h, tx.serialize())) {
        mv.last_action = "bootstrap: failed to enqueue tx";
        mv.last_action_color = CP_WARN;
        return;
    }
    if (want_username) {
        auto un_h = un_tx.tx_hash();
        db.put_pending_tx(un_h, un_tx.serialize());
    }

    const std::string addr_hex = mc::crypto::to_checksum_hex(kp.address);
    std::string suffix;
    if (want_username) suffix = " + username '" + username + "'";
    mv.last_action = "bootstrap: GRANT queued for "
                   + addr_hex.substr(0, 14) + "…" + suffix
                   + "  (next block applies)";
    mv.last_action_color = CP_OK;
}

// ---- Founder-only: record-label management (M key) ------------------

void action_manage_labels(mc::Database& db, const ModeratorState& mod,
                          ModView& mv) {
    if (!mod.logged_in) {
        mv.last_action = "labels: not logged in";
        mv.last_action_color = CP_WARN;
        return;
    }
    if (db.get_mod_level(mod.kp.address)
            != static_cast<uint8_t>(mc::ModLevel::FOUNDER)) {
        mv.last_action = "labels: founder only";
        mv.last_action_color = CP_WARN;
        return;
    }

    // Submenu: D = define label, A = assign artist, ESC = back.
    erase();
    int max_y = getmaxy(stdscr);
    int max_x = getmaxx(stdscr);
    attron(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);
    mvhline(0, 0, ' ', max_x);
    mvprintw(0, 2, " Record-label management (founder) ");
    attroff(COLOR_PAIR(CP_PANEL_HDR) | A_BOLD);

    auto labels = db.list_labels();
    int r = 2;
    attron(COLOR_PAIR(CP_LABEL) | A_BOLD);
    mvprintw(r++, 2, "Existing labels (%zu)", labels.size());
    attroff(COLOR_PAIR(CP_LABEL) | A_BOLD);
    for (size_t i = 0; i < labels.size() && r < max_y - 6; ++i) {
        auto def = db.get_label(labels[i]);
        if (!def) continue;
        std::string line = " " + def->display_name + "  [";
        for (size_t s = 0; s < def->splits.size(); ++s) {
            if (s) line += ", ";
            line += mc::crypto::to_checksum_hex(def->splits[s].wallet)
                        .substr(0, 10) + "…:"
                  + std::to_string(def->splits[s].basis_points / 100) + "%";
        }
        line += "]";
        if ((int)line.size() > max_x - 4) line = line.substr(0, max_x - 5) + "…";
        attron(COLOR_PAIR(CP_VALUE));
        mvprintw(r++, 2, "%s", line.c_str());
        attroff(COLOR_PAIR(CP_VALUE));
    }

    attron(COLOR_PAIR(CP_FOOTER_LBL));
    mvhline(max_y - 1, 0, ' ', max_x);
    attroff(COLOR_PAIR(CP_FOOTER_LBL));
    attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
    mvprintw(max_y - 1, 1, " D ");
    attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
    mvprintw(max_y - 1, 4, "Define label   ");
    attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
    mvprintw(max_y - 1, 20, " A ");
    attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
    mvprintw(max_y - 1, 23, "Assign artist   ");
    attron(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
    mvprintw(max_y - 1, 40, " Q ");
    attroff(COLOR_PAIR(CP_FOOTER_KEY) | A_BOLD);
    mvprintw(max_y - 1, 43, "Back");
    refresh();

    nodelay(stdscr, FALSE);
    int key = getch();
    nodelay(stdscr, TRUE);

    if (key == 'q' || key == 'Q' || key == 27) return;

    std::string meta_json;

    if (key == 'd' || key == 'D') {
        std::string name;
        if (!prompt_string("Label name (1..64 chars)", name, 64)) return;
        // Collect splits interactively.
        std::ostringstream splits_js;
        splits_js << "[";
        int total_bp = 0;
        int count = 0;
        while (count < 16 && total_bp < 10000) {
            std::string addr_hex, bp_str;
            const std::string p =
                "Split " + std::to_string(count + 1) +
                " wallet (0x… or blank to finish)";
            if (!prompt_string(p.c_str(), addr_hex, 64)) break;
            if (addr_hex.empty()) break;
            Address w{};
            if (!mc::crypto::parse_address(addr_hex, w)) {
                mv.last_action = "labels: bad wallet address";
                mv.last_action_color = CP_WARN; return;
            }
            int remaining = 10000 - total_bp;
            const std::string p2 =
                "Basis points 1.." + std::to_string(remaining)
                                   + " (100 = 1%)";
            if (!prompt_string(p2.c_str(), bp_str, 5)) break;
            int bp = std::atoi(bp_str.c_str());
            if (bp <= 0 || bp > remaining) {
                mv.last_action = "labels: bad basis points";
                mv.last_action_color = CP_WARN; return;
            }
            if (count) splits_js << ",";
            splits_js << "{\"addr\":\""
                      << mc::crypto::to_checksum_hex(w)
                      << "\",\"bp\":" << bp << "}";
            total_bp += bp;
            ++count;
        }
        splits_js << "]";
        if (total_bp != 10000 || count == 0) {
            mv.last_action = "labels: splits must total 10000 bp (100%)";
            mv.last_action_color = CP_WARN; return;
        }
        std::ostringstream js;
        js << "{\"action\":\"label_define\",\"name\":\"" << name
           << "\",\"splits\":" << splits_js.str() << "}";
        meta_json = js.str();
    } else if (key == 'a' || key == 'A') {
        std::string artist_hex, label_name;
        if (!prompt_string("Artist address (0x…)", artist_hex, 64)) return;
        Address artist{};
        if (!mc::crypto::parse_address(artist_hex, artist)) {
            mv.last_action = "labels: bad artist address";
            mv.last_action_color = CP_WARN; return;
        }
        if (!prompt_string("Label name (blank to unassign)",
                           label_name, 64)) return;
        std::ostringstream js;
        js << "{\"action\":\"label_assign\",\"artist\":\""
           << mc::crypto::to_checksum_hex(artist)
           << "\",\"label\":\"" << label_name << "\"}";
        meta_json = js.str();
    } else {
        return;
    }

    // Build the founder-signed ModeratorOpTx.
    mc::ModeratorOpTx tx{};
    tx.op_code        = static_cast<uint8_t>(mc::ModOpCode::TAG_LABEL_EDIT);
    tx.proposer       = mod.kp.address;
    tx.proposer_pubkey = mod.kp.public_key;
    tx.nonce          = db.get_nonce(mod.kp.address);
    tx.meta_json      = meta_json;

    auto msg_bytes = tx.sign_message();
    auto msg_hash  = mc::crypto::sha256(msg_bytes.data(), msg_bytes.size());
    tx.signature   = mc::crypto::sign_ecdsa(msg_hash, mod.kp.private_key);
    if (!tx.verify_signature()) {
        mv.last_action = "labels: sign/verify mismatch";
        mv.last_action_color = CP_WARN; return;
    }
    auto h = tx.tx_hash();
    if (!db.put_pending_tx(h, tx.serialize())) {
        mv.last_action = "labels: failed to enqueue";
        mv.last_action_color = CP_WARN; return;
    }
    mv.last_action = "labels: " +
        std::string(key == 'd' || key == 'D' ? "define" : "assign")
        + " queued (next block applies)";
    mv.last_action_color = CP_OK;
}

} // namespace

void start_log_capture() {
    if (g_rb_cout) return;
    g_rb_cout   = new RingStreambuf(g_logs);
    g_rb_cerr   = new RingStreambuf(g_logs);
    g_prev_cout = std::cout.rdbuf(g_rb_cout);
    g_prev_cerr = std::cerr.rdbuf(g_rb_cerr);
}

void stop_log_capture() {
    if (g_prev_cout) std::cout.rdbuf(g_prev_cout);
    if (g_prev_cerr) std::cerr.rdbuf(g_prev_cerr);
    g_prev_cout = nullptr;
    g_prev_cerr = nullptr;
    delete g_rb_cout; g_rb_cout = nullptr;
    delete g_rb_cerr; g_rb_cerr = nullptr;
}

void run_tui(mc::api::HttpServer& /*http*/,
             mc::api::RatsApi& api,
             mc::Chain& chain,
             mc::Database& db,
             mc::store::SwarmIndex& swarm,
             mc::net::NetworkManager& net,
             mc::CandidateManager& candidates,
             const mc::crypto::KeyPair& node_keypair,
             const std::string& data_dir,
             std::atomic<bool>& keep_running) {
#ifdef _WIN32
    // Open INDEPENDENT kernel handles to the console. CONOUT$ / CONIN$
    // always resolve to the active console; the returned handles are
    // brand-new kernel object refs, immune to any later _dup2 trampling
    // of the CRT std fds.
    HANDLE conout = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE conin  = CreateFileA("CONIN$",  GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (conout != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, conout);
        SetStdHandle(STD_ERROR_HANDLE,  conout);
    }
    if (conin != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_INPUT_HANDLE,  conin);
    }
#endif

    initscr();
    if (stdscr == nullptr) {
        while (keep_running.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }

#ifdef _WIN32
    // PDCurses cached our independent conout/conin above. NOW point the
    // CRT's fd 1 / 2 at NUL so any stdout writes we couldn't catch via
    // rdbuf swap (DLL-local std::cout, raw printf, etc.) disappear
    // instead of trampling the TUI.
    int nul_fd = _open("NUL", _O_WRONLY);
    if (nul_fd >= 0) {
        std::fflush(stdout);
        std::fflush(stderr);
        _dup2(nul_fd, _fileno(stdout));
        _dup2(nul_fd, _fileno(stderr));
        _close(nul_fd);
        if (conout != INVALID_HANDLE_VALUE) {
            SetStdHandle(STD_OUTPUT_HANDLE, conout);
            SetStdHandle(STD_ERROR_HANDLE,  conout);
        }
    }
#endif
    raw();                       // catch Ctrl-C ourselves
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    setup_colors();
    start_log_capture();

    ModeratorState mod{};  // session starts logged out — press L to authenticate
    ModView mv;
    mv.dmca_files = scan_dmca_inbox(data_dir);
    mv.kyc_files  = scan_kyc_inbox(data_dir);

    int page = 1;
    auto last_redraw = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    const auto redraw_period = std::chrono::seconds(1);
    while (keep_running.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_redraw >= redraw_period) {
            erase();
            draw_header_bar(page);
            if (page == 2) {
                draw_logs_page();
                draw_footer_bar({
                    {"F1", "Main"},
                    {"F2", "Logs"},
                    {"Q",  "Quit"},
                });
            } else {
                draw_main_page(chain, db, swarm, net, node_keypair, mod, mv);
                if (mod.logged_in) {
                    draw_footer_bar({
                        {"F1", "Main"},
                        {"F2", "Logs"},
                        {"I",  "Inbox"},
                        {"K",  "KYC"},
                        {"H",  "Library"},
                        {"E",  "Release"},
                        {"V",  "Vote"},
                        {"M",  "Labels"},
                        {"R",  "Refresh"},
                        {"O",  "Logout"},
                        {"Q",  "Quit"},
                    });
                } else {
                    if (db.get_founder().has_value()) {
                        draw_footer_bar({
                            {"F1", "Main"},
                            {"F2", "Logs"},
                            {"L",  "Login"},
                            {"Q",  "Quit"},
                        });
                    } else {
                        // Pre-founder: B bootstraps. L is also shown
                        // because the operator may have just pressed B
                        // and now wants to log in once the GRANT mines
                        // — or they may have a founder.seed file from a
                        // previous bootstrap whose GRANT hasn't applied
                        // yet.
                        draw_footer_bar({
                            {"F1", "Main"},
                            {"F2", "Logs"},
                            {"B",  "Bootstrap"},
                            {"L",  "Login"},
                            {"Q",  "Quit"},
                        });
                    }
                }
            }
            refresh();
            last_redraw = now;
        }

        const int key = getch();
        if (key == ERR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Quit on Q / q / Ctrl-C / ESC.
        if (key == 'q' || key == 'Q' || key == 3 /*Ctrl-C*/ || key == 27 /*ESC*/) {
            keep_running.store(false, std::memory_order_relaxed);
            break;
        }
        if (key == KEY_F(1) || key == '1') { page = 1; last_redraw = std::chrono::steady_clock::now() - redraw_period; continue; }
        if (key == KEY_F(2) || key == '2') { page = 2; last_redraw = std::chrono::steady_clock::now() - redraw_period; continue; }

        if (page == 1) {
            // Login / logout are always available on F1 regardless of
            // auth state. Bootstrap is only meaningful when no founder
            // is recorded; everything else is gated on session auth.
            if (key == 'l' || key == 'L') {
                action_login(db, data_dir, mod, mv);
            } else if (key == 'o' || key == 'O') {
                action_logout(mod, mv);
            } else if ((key == 'b' || key == 'B') && !mod.logged_in
                       && !db.get_founder().has_value()) {
                action_bootstrap_founder(db, data_dir, mv);
                // The GRANT just landed in the mempool — wake the
                // producer thread so the first block mines immediately,
                // not 2 s later when the heartbeat poll re-checks.
                candidates.wake();
            } else if (mod.logged_in) {
                if (key == 'i' || key == 'I') {
                    mv.dmca_files = scan_dmca_inbox(data_dir);
                    mv.kyc_files  = scan_kyc_inbox(data_dir);
                    mv.last_action = "inbox: " + std::to_string(mv.dmca_files.size())
                                   + " DMCA · "
                                   + std::to_string(mv.kyc_files.size())
                                   + " KYC";
                    mv.last_action_color = CP_OK;
                } else if (key == 'h' || key == 'H') {
                    action_browse_library(api, db, mod, mv);
                } else if (key == 'k' || key == 'K') {
                    action_review_kyc(db, mod, mv, data_dir);
                    candidates.wake();
                } else if (key == 'e' || key == 'E') {
                    action_release_escrow(db, mod, mv);
                    candidates.wake();
                } else if (key == 'v' || key == 'V') {
                    action_view_proposals(db, mod, mv);
                    candidates.wake();
                } else if (key == 'm' || key == 'M') {
                    action_manage_labels(db, mod, mv);
                    candidates.wake();
                } else if (key == 'r' || key == 'R') {
                    mv.dmca_files = scan_dmca_inbox(data_dir);
                    mv.kyc_files  = scan_kyc_inbox(data_dir);
                    mv.last_action = "refreshed";
                    mv.last_action_color = CP_OK;
                }
            }
        }
        last_redraw = std::chrono::steady_clock::now() - redraw_period;
    }

    stop_log_capture();
    curs_set(1);
    endwin();
}

} // namespace mc::ui
