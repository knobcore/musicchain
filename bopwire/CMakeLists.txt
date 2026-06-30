cmake_minimum_required(VERSION 3.16)
project(bopwire VERSION 0.1.0 LANGUAGES CXX C)

# Position-independent code globally — required on Linux so the static
# libwally + librats + secp256k1 archives can be linked into the shared
# libbopwire.so. Without this the linker errors with R_X86_64_PC32
# "can not be used when making a shared object" deep inside libwally's
# secp256k1 internals. Harmless on Windows (MSVC ignores -fPIC; PE/COFF
# is position-independent by construction).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# When set, only the mini-node + mc_rats_quic targets are configured. Skips
# the heavy media/chain deps (ffmpeg, chromaprint, leveldb, ...) that the
# full home-node needs but the VPS rendezvous binary does not. Used by the
# Linux mini-node cross-build in WSL.
option(MC_MINI_NODE_ONLY "Configure only the mini-node target" OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
if(MSVC)
  add_compile_options(/W4 /WX- /O2)
else()
  # NB: do NOT enable -Wpedantic / -Werror at the directory level — these flags
  # leak into add_subdirectory(deps/msquic) and msquic's vendored quictls would
  # fail to build with our project's tighter warning policy. Per-target warning
  # flags can be added back individually if/when desired.
  add_compile_options(-Wall -Wextra)
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
  else()
    add_compile_options(-g)
  endif()
  find_package(Threads REQUIRED)
endif()

# ---- Find dependencies ----

find_package(OpenSSL REQUIRED)

# pkg-config fallback helper
find_package(PkgConfig QUIET)

if(NOT MC_MINI_NODE_ONLY)

# Chromaprint
if(PKG_CONFIG_FOUND)
  pkg_check_modules(CHROMAPRINT libchromaprint)
endif()
if(NOT CHROMAPRINT_FOUND)
  find_path(CHROMAPRINT_INCLUDE_DIRS chromaprint.h)
  find_library(CHROMAPRINT_LIBRARIES NAMES chromaprint)
  if(CHROMAPRINT_INCLUDE_DIRS AND CHROMAPRINT_LIBRARIES)
    set(CHROMAPRINT_FOUND TRUE)
  endif()
endif()
if(NOT CHROMAPRINT_FOUND)
  message(FATAL_ERROR "Chromaprint not found. Install libchromaprint-dev or build from source.")
endif()

# FFmpeg
if(PKG_CONFIG_FOUND)
  pkg_check_modules(AVCODEC  REQUIRED libavcodec)
  pkg_check_modules(AVFORMAT REQUIRED libavformat)
  pkg_check_modules(AVUTIL   REQUIRED libavutil)
  pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
endif()

# Ogg / Vorbis / Opus
if(PKG_CONFIG_FOUND)
  pkg_check_modules(OGG      REQUIRED ogg)
  pkg_check_modules(VORBIS   REQUIRED vorbis)
  pkg_check_modules(VORBISFILE REQUIRED vorbisfile)
  pkg_check_modules(OPUS     REQUIRED opus)
  pkg_check_modules(OPUSFILE REQUIRED opusfile)
endif()

# LevelDB
find_package(leveldb QUIET)
if(NOT leveldb_FOUND AND PKG_CONFIG_FOUND)
  pkg_check_modules(LEVELDB leveldb)
endif()
if(NOT LEVELDB_FOUND AND NOT leveldb_FOUND)
  find_path(LEVELDB_INCLUDE_DIRS leveldb/db.h)
  find_library(LEVELDB_LIBRARIES NAMES leveldb)
  if(LEVELDB_INCLUDE_DIRS AND LEVELDB_LIBRARIES)
    set(LEVELDB_FOUND TRUE)
  else()
    message(FATAL_ERROR "LevelDB not found. Install libleveldb-dev.")
  endif()
endif()

# libuv removed — its only consumer was the legacy libuv TCP mesh
# (network/peer.cpp), deleted in the Model 1 cleanup. The live transport
# is librats (QUIC), which has no libuv dependency.

# libmicrohttpd was removed — HTTP/3 now goes through msh3 (see
# src/transport/h3_server.cpp) tunneled inside the librats QUIC mesh on
# UDP/443, so there is no second TCP listener to find here.

# miniupnpc (for UPnP NAT port mapping)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(MINIUPNPC miniupnpc)
endif()
if(NOT MINIUPNPC_FOUND)
  find_path(MINIUPNPC_INCLUDE_DIRS miniupnpc/miniupnpc.h
    HINTS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
          "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
          "C:/vcpkg/installed/x64-windows/include")
  find_library(MINIUPNPC_LIBRARY NAMES miniupnpc
    HINTS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
          "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
          "C:/vcpkg/installed/x64-windows/lib")
  if(MINIUPNPC_INCLUDE_DIRS AND MINIUPNPC_LIBRARY)
    set(MINIUPNPC_FOUND TRUE)
    set(MINIUPNPC_LIBRARIES ${MINIUPNPC_LIBRARY})
  else()
    message(WARNING "miniupnpc not found — UPnP port mapping disabled")
  endif()
endif()

endif() # NOT MC_MINI_NODE_ONLY

# HTTP/3 / QUIC removed entirely — librats is plain TCP and the standalone
# msh3 HTTP/3 listener was retired. No msh3/msquic/QuicTLS dependency.
set(msh3_FOUND FALSE)

# librats needs OpenSSL only for Noise + STUN crypto + file_transfer (no QUIC).
set(MC_OPENSSL_SSL    OpenSSL::SSL)
set(MC_OPENSSL_CRYPTO OpenSSL::Crypto)

# libcurl removed — its only consumer was the HttpGossip registry
# announcer (network/registry_announcer.cpp), deleted in the Model 1
# cleanup. Block sync now runs over librats (BlockPropagator).

# nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(NLOHMANN_JSON nlohmann_json)
  endif()
  if(NOT NLOHMANN_JSON_FOUND)
    find_path(NLOHMANN_JSON_INCLUDE_DIRS nlohmann/json.hpp
      HINTS /usr/include /usr/local/include)
    if(NLOHMANN_JSON_INCLUDE_DIRS)
      set(NLOHMANN_JSON_FOUND TRUE)
    else()
      message(FATAL_ERROR "nlohmann/json not found. Install nlohmann-json3-dev.")
    endif()
  endif()
endif()

# ---- Linker search path for vcpkg (required for bare lib names from pkg-config) ----
# pkg-config returns bare names like "chromaprint" via ${CHROMAPRINT_LIBRARIES},
# so the linker needs the vcpkg lib dir on its search path. Honor both the
# in-tree manifest layout (build/vcpkg_installed) and an out-of-tree
# VCPKG_INSTALLED_DIR (e.g. shared across projects).
if(DEFINED VCPKG_TARGET_TRIPLET)
  if(EXISTS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib")
    link_directories("${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib")
  endif()
  if(DEFINED VCPKG_INSTALLED_DIR AND
     EXISTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    link_directories("${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
  endif()
endif()

# ---- Collect all include dirs & libs ----

set(MC_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
  ${OPENSSL_INCLUDE_DIR}
  ${CHROMAPRINT_INCLUDE_DIRS}
  ${AVCODEC_INCLUDE_DIRS}
  ${AVFORMAT_INCLUDE_DIRS}
  ${AVUTIL_INCLUDE_DIRS}
  ${SWRESAMPLE_INCLUDE_DIRS}
  ${OGG_INCLUDE_DIRS}
  ${VORBIS_INCLUDE_DIRS}
  ${VORBISFILE_INCLUDE_DIRS}
  ${OPUS_INCLUDE_DIRS}
  ${OPUSFILE_INCLUDE_DIRS}
  ${LEVELDB_INCLUDE_DIRS}
  ${UV_INCLUDE_DIRS}
  ${NLOHMANN_JSON_INCLUDE_DIRS}
  ${CURL_INCLUDE_DIRS}
)
if(MINIUPNPC_FOUND OR miniupnpc_FOUND)
  list(APPEND MC_INCLUDE_DIRS ${MINIUPNPC_INCLUDE_DIRS})
endif()

set(MC_LINK_LIBS
  ${MC_OPENSSL_SSL}
  ${MC_OPENSSL_CRYPTO}
  ${CHROMAPRINT_LIBRARIES}
  ${AVCODEC_LIBRARIES}
  ${AVFORMAT_LIBRARIES}
  ${AVUTIL_LIBRARIES}
  ${SWRESAMPLE_LIBRARIES}
  ${OGG_LIBRARIES}
  ${VORBIS_LIBRARIES}
  ${VORBISFILE_LIBRARIES}
  ${OPUS_LIBRARIES}
  ${OPUSFILE_LIBRARIES}
  ${LEVELDB_LIBRARIES}
)

if(NOT MSVC)
  list(APPEND MC_LINK_LIBS Threads::Threads)
endif()

# cpp-httplib for the JSON-RPC HTTP shim (eth_chainId / eth_blockNumber /
# eth_getBalance / etc on 127.0.0.1:8545). Single-header, vendored at
# deps/cpp-httplib so we don't depend on the distro packaging that
# variously ships a CONFIG target (vcpkg, Arch), a -dev split (Debian),
# or nothing at all. The vendored copy is mit-licensed and pinned at
# yhirose/cpp-httplib v0.15.3.
list(APPEND MC_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/deps/cpp-httplib)

# libwally-core: Bitcoin/Liquid wallet primitives. Vendored under
# deps/libwally-core (git clone + secp256k1-zkp submodule). We force a
# static build so the wallycore archive gets folded into
# bopwire_static — no separate runtime .dll / .so to ship.
set(BUILD_SHARED_LIBS                OFF CACHE BOOL "" FORCE)
set(WALLYCORE_ENABLE_TESTS           OFF CACHE BOOL "" FORCE)
set(WALLYCORE_INSTALL                OFF CACHE BOOL "" FORCE)
set(WALLYCORE_BUILD_ELEMENTS         OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS            OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK        OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES         OFF CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL                OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/libwally-core EXCLUDE_FROM_ALL)
list(APPEND MC_LINK_LIBS wallycore)
list(APPEND MC_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/deps/libwally-core/include)

if(TARGET nlohmann_json::nlohmann_json)
  list(APPEND MC_LINK_LIBS nlohmann_json::nlohmann_json)
endif()

if(TARGET leveldb::leveldb)
  list(REMOVE_ITEM MC_LINK_LIBS ${LEVELDB_LIBRARIES})
  list(APPEND MC_LINK_LIBS leveldb::leveldb)
endif()

if(MINIUPNPC_FOUND)
  list(APPEND MC_LINK_LIBS ${MINIUPNPC_LIBRARIES})
endif()

list(APPEND MC_LINK_LIBS mc_rats_quic)

# NOTE (historical): src/transport/mc_rats_quic.cpp was a custom msquic-based
# reimplementation of the librats C API. That QUIC bridge was removed; the
# real transport is now librats (frozen v0.2.0), and `mc_rats_quic` is just a
# CMake ALIAS for the `rats` target (see the add_library(mc_rats_quic ALIAS
# rats) below) so older link references keep resolving. The include dir is
# retained for the few headers (e.g. cert_util.h) still shared with the
# opt-in HTTP/3 surface.
list(APPEND MC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src/transport")

# ---- Windows: socket libs needed by rats_link / peer code ----
# advapi32: RegGetValueA in src/util/hw_fingerprint.cpp (MachineGuid etc.).
if(WIN32)
  list(APPEND MC_LINK_LIBS ws2_32 iphlpapi bcrypt advapi32)
endif()

# ---- CRoaring (Roaring bitmaps) for the wallet LibraryStore (DB2) -----
# Compiled as an isolated C static lib so the amalgamation keeps its own
# SIMD-autodetect flags and doesn't inherit the project's C++ strictness.
# PIC so it links into the bopwire SHARED lib.
add_library(roaring STATIC deps/croaring/roaring.c)
target_include_directories(roaring PUBLIC ${CMAKE_SOURCE_DIR}/deps/croaring)
set_target_properties(roaring PROPERTIES POSITION_INDEPENDENT_CODE ON)
list(APPEND MC_LINK_LIBS roaring)
list(APPEND MC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/deps/croaring")

# ---- Library sources ----

set(MC_SOURCES
  src/core/block.cpp
  src/core/transaction.cpp
  src/core/merkle.cpp
  src/core/chain.cpp
  src/crypto/hash.cpp
  src/crypto/keys.cpp
  src/crypto/signature.cpp
  src/crypto/ecies.cpp
  # Hand-rolled BIP39 stays in the tree as a reference but is no longer
  # linked. wally_bip39.cpp implements the same public API on top of
  # the vendored libwally-core archive under deps/libwally-core.
  # src/crypto/bip39.cpp
  # src/crypto/bip39_wordlist_en.cpp
  src/crypto/wally_bip39.cpp
  src/crypto/keccak256.cpp
  src/audio/ogg_validator.cpp
  src/audio/ogg_decoder.cpp
  src/audio/fingerprint.cpp
  src/audio/multi_decoder.cpp
  src/consensus/candidate.cpp
  src/consensus/slashing.cpp
  src/sync/block_propagator.cpp
  src/sync/deep_audit.cpp
  src/net/load_monitor.cpp
  src/net/relay_credit_tracker.cpp
  src/tokens/ledger.cpp
  src/tokens/mint.cpp
  src/network/manager.cpp       # thin shim now — no TCP mesh
  src/network/rats_link.cpp
  src/storage/database.cpp
  src/store/swarm.cpp
  src/store/library_store.cpp
  src/api/server.cpp
  src/api/jsonrpc_server.cpp
  src/api/routes.cpp
  src/api/rats_api.cpp
  src/util/hw_fingerprint.cpp
  src/capi/capi.cpp
)
# (HTTP/3 surface removed — no msh3/h3_server; librats is plain TCP)

# ---- librats (vanilla TCP transport) ----
#
# We build the *full* librats from deps/librats — DHT, GossipSub, ICE,
# file transfer, Noise encryption, the lot — using its stock TCP socket
# layer. QUIC is gone entirely: the old quic_socket.cpp bridge and the
# msh3/msquic HTTP/3 deps were removed. librats is plain TCP, end to end.
#
# The output is renamed `mc_rats` so the Dart FFI side keeps loading
# 'mc_rats.dll' / 'libmc_rats.so' unchanged. Home node and mini-node link
# against it as a shared library (and depend on the DLL/.so at runtime).
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RATS_SHARED_LIBRARY ON  CACHE BOOL "" FORCE)
set(RATS_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS       ON  CACHE BOOL "" FORCE)   # librats_c.cpp / rats_* C API
set(RATS_INSTALL        OFF CACHE BOOL "" FORCE)
set(RATS_VERSION_OVERRIDE "1.0.0" CACHE STRING "" FORCE)
add_subdirectory(deps/librats EXCLUDE_FROM_ALL)
# Rename the output so it lands next to the Dart FFI loader as mc_rats.dll /
# libmc_rats.so.
set_target_properties(rats PROPERTIES OUTPUT_NAME mc_rats)
# `mc_rats` alias for the rest of the project — code that used to link against
# our old mc_rats_quic STATIC target now links the librats shared library.
add_library(mc_rats ALIAS rats)
# Older code paths may still reference mc_rats_quic by name — alias too.
add_library(mc_rats_quic ALIAS rats)

if(NOT MC_MINI_NODE_ONLY)

# Shared library (C API for Flutter FFI)
add_library(bopwire SHARED ${MC_SOURCES})
target_include_directories(bopwire PUBLIC ${MC_INCLUDE_DIRS})
target_link_libraries(bopwire PUBLIC ${MC_LINK_LIBS})
target_compile_definitions(bopwire PRIVATE BUILDING_BOPWIRE_DLL)
set_target_properties(bopwire PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  OUTPUT_NAME bopwire
)

# Static library (linked into node executable)
add_library(bopwire_static STATIC ${MC_SOURCES})
target_include_directories(bopwire_static PUBLIC ${MC_INCLUDE_DIRS})
target_link_libraries(bopwire_static PUBLIC ${MC_LINK_LIBS})

# Node executable
add_executable(bopwire-node tools/node_main.cpp tools/node_tui.cpp)
target_include_directories(bopwire-node PRIVATE ${MC_INCLUDE_DIRS})
target_link_libraries(bopwire-node PRIVATE bopwire_static)

# ncurses (Linux) / PDCurses (Windows, via vcpkg) for the home node's
# interactive TUI. Both ship a Curses-compatible API so the same TUI
# source compiles on either; find_package(Curses) picks whichever is
# installed for the platform.
if (WIN32)
    find_package(unofficial-pdcurses CONFIG REQUIRED)
    target_link_libraries(bopwire-node
        PRIVATE unofficial::pdcurses::pdcurses)
    target_compile_definitions(bopwire-node PRIVATE MC_TUI_PDCURSES=1)
else()
    set(CURSES_NEED_NCURSES TRUE)
    find_package(Curses REQUIRED)
    target_include_directories(bopwire-node PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(bopwire-node PRIVATE ${CURSES_LIBRARIES})
endif()

endif() # NOT MC_MINI_NODE_ONLY

# Mini-node — VPS rendezvous binary, vanilla librats TCP transport.
#
# Pulls in a focused subset of the wallet stack (libwally BIP39+BIP32,
# secp256k1, OpenSSL signing, keccak256 address derivation, RelayRewardTx
# serialization) so the mini-node can hold its own EVM-shaped identity
# and let the full node credit it for served tunnel traffic via the
# RELAY_REWARD tx type.
add_executable(bopwire-mini-node
  tools/mini_node.cpp
  src/net/load_monitor.cpp
  src/crypto/hash.cpp
  src/crypto/keys.cpp
  src/crypto/keccak256.cpp
  src/crypto/signature.cpp
  src/crypto/wally_bip39.cpp
  src/core/block.cpp
  src/core/transaction.cpp)
target_link_libraries(bopwire-mini-node PRIVATE
  mc_rats_quic
  wallycore
  secp256k1
  ${MC_OPENSSL_SSL}
  ${MC_OPENSSL_CRYPTO})
target_include_directories(bopwire-mini-node PRIVATE
  ${OPENSSL_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/deps/libwally-core/include
  ${CMAKE_SOURCE_DIR}/deps/libwally-core/src/secp256k1/include)
if(WIN32)
  target_link_libraries(bopwire-mini-node PRIVATE ws2_32 iphlpapi bcrypt)
endif()
if(NOT MSVC)
  target_link_libraries(bopwire-mini-node PRIVATE Threads::Threads)
endif()
# Static-link libstdc++/libgcc so the deploy ELF has no C++ runtime ABI
# dependency on whatever glibc/g++ ships on the VPS. glibc itself stays
# dynamic — a fully -static build pulls in a getaddrinfo / NSS hairball.
if(NOT MSVC AND NOT APPLE)
  target_link_options(bopwire-mini-node PRIVATE
    -static-libstdc++ -static-libgcc)
endif()

# library_smoke_cli — Android-friendly CLI that does routes.get + songs.list
# over the same librats relay path the player uses. Pure-C++ + librats; no
# Dart/Flutter, so we can adb push to the phone and run from `adb shell` on
# cellular to validate the live network end-to-end.
add_executable(library_smoke_cli tools/library_smoke_cli.cpp)
target_link_libraries(library_smoke_cli PRIVATE mc_rats_quic)
if(WIN32)
  target_link_libraries(library_smoke_cli PRIVATE ws2_32 iphlpapi bcrypt)
endif()
if(NOT MSVC)
  target_link_libraries(library_smoke_cli PRIVATE Threads::Threads)
endif()
if(NOT MSVC AND NOT APPLE AND NOT ANDROID)
  target_link_options(library_smoke_cli PRIVATE
    -static-libstdc++ -static-libgcc)
endif()

# (h3_smoke removed — HTTP/3 surface gone)

# bopwire-web-gateway — browser-facing HTTPS/JSON gateway for the web player
# (webplayer/). A headless librats peer (mc_rats_quic) that joins the mini mesh,
# serves the Discover feed + audio over plain HTTP (Caddy terminates TLS), and
# runs play sessions so web plays mint the artist/seeder/mini reward. Source
# lives under webplayer/gateway/ so the web player is self-contained.
add_executable(bopwire-web-gateway
  webplayer/gateway/src/main.cpp
  webplayer/gateway/src/rats_link.cpp
  webplayer/gateway/src/streamer.cpp)
target_compile_features(bopwire-web-gateway PRIVATE cxx_std_17)
target_include_directories(bopwire-web-gateway PRIVATE
  ${CMAKE_SOURCE_DIR}/deps/librats/src      # librats_c.h, json.hpp
  ${CMAKE_SOURCE_DIR}/deps/cpp-httplib)      # httplib.h (header-only, plain HTTP)
# Link the real librats target directly (NOT the mc_rats_quic alias — QUIC was
# scrapped and that alias is going away). `rats` is what deps/librats defines.
target_link_libraries(bopwire-web-gateway PRIVATE rats)
if(WIN32)
  target_link_libraries(bopwire-web-gateway PRIVATE ws2_32 iphlpapi bcrypt)
endif()
if(NOT MSVC)
  target_link_libraries(bopwire-web-gateway PRIVATE Threads::Threads)
endif()
if(NOT MSVC AND NOT APPLE AND NOT ANDROID)
  target_link_options(bopwire-web-gateway PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(NOT MC_MINI_NODE_ONLY)

# Copy static bootstrap file next to the node executable so it can seed it
add_custom_command(TARGET bopwire-node POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/assets/bopwire-bootstrap.txt"
    "$<TARGET_FILE_DIR:bopwire-node>/bopwire-bootstrap.txt"
  COMMENT "Copying bootstrap file to output directory"
)

# Install
include(GNUInstallDirs)
install(TARGETS bopwire bopwire-node
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/bopwire.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

endif() # NOT MC_MINI_NODE_ONLY

# Tests (optional)
enable_testing()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests EXCLUDE_FROM_ALL)
endif()
