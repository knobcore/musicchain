cmake_minimum_required(VERSION 3.16)
project(musicchain VERSION 0.1.0 LANGUAGES CXX C)

# Position-independent code globally — required on Linux so the static
# libwally + librats + secp256k1 archives can be linked into the shared
# libmusicchain.so. Without this the linker errors with R_X86_64_PC32
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

# libuv
if(PKG_CONFIG_FOUND)
  pkg_check_modules(UV REQUIRED libuv)
endif()

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

# HTTP/3 (msh3 + msquic + QuicTLS) is opt-in. The home node currently
# serves every verb over librats TCP RPC; the standalone msh3 listener was
# removed when the QUIC bridge was ripped out (see [[project-revert-to-
# plain-librats]]). Building msh3 also drags in QuicTLS, which needs perl
# + NASM + a working OpenSSL configure pipeline. Skip it by default — flip
# -DMC_WITH_H3=ON when restoring the h3_server. The h3_smoke target stays
# behind the same flag.
option(MC_WITH_H3 "Build the optional HTTP/3 surface (msh3 + msquic + h3_server)" OFF)
set(msh3_FOUND FALSE)
if(MC_WITH_H3)
  if(NOT EXISTS "${CMAKE_SOURCE_DIR}/deps/msh3/CMakeLists.txt")
    message(FATAL_ERROR "deps/msh3 not present but MC_WITH_H3=ON.")
  endif()

  # These must be set BEFORE add_subdirectory(deps/msh3) so msh3's own
  # `set(QUIC_TLS_LIB ... CACHE)` calls (without FORCE) see them already
  # cached and skip. We force quictls on every platform so the TLS backend
  # is the same whether you talk to a Linux peer or a Windows peer.
  set(QUIC_TLS_LIB       "quictls" CACHE STRING "" FORCE)
  set(QUIC_BUILD_SHARED  OFF        CACHE BOOL   "" FORCE)
  set(QUIC_BUILD_TOOLS   OFF        CACHE BOOL   "" FORCE)
  set(QUIC_BUILD_TEST    OFF        CACHE BOOL   "" FORCE)
  set(QUIC_BUILD_PERF    OFF        CACHE BOOL   "" FORCE)
  set(MSH3_TOOL          OFF        CACHE BOOL   "" FORCE)
  set(MSH3_TEST          OFF        CACHE BOOL   "" FORCE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_subdirectory(deps/msh3 EXCLUDE_FROM_ALL)
  set(msh3_FOUND TRUE)

  # After deps/msh3 adds its inner msquic, the `msquic_static` target exists
  # (with quictls statically linked). Use it everywhere msquic is needed.
  set(MC_MSQUIC_TARGET msquic_static)
  set(MC_MSQUIC_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/deps/msh3/msquic/src/inc")
endif()

# When MC_WITH_H3 is on, reuse the quictls static archives produced inside
# msh3's msquic build so the whole project shares one TLS source. When H3
# is off, fall back to the OpenSSL find_package result (vcpkg on Windows,
# system / prebuilt on Linux/Android) — librats only needs OpenSSL for
# Noise + STUN crypto + file_transfer, no QUIC dependency.
if(MC_WITH_H3)
  set(MC_QUICTLS_DIR "${CMAKE_BINARY_DIR}/_deps/opensslquic-build/quictls")
  add_library(mc_openssl_ssl    STATIC IMPORTED GLOBAL)
  add_library(mc_openssl_crypto STATIC IMPORTED GLOBAL)
  if(WIN32)
    set_target_properties(mc_openssl_ssl    PROPERTIES IMPORTED_LOCATION
      "${MC_QUICTLS_DIR}/release/lib/libssl.lib")
    set_target_properties(mc_openssl_crypto PROPERTIES IMPORTED_LOCATION
      "${MC_QUICTLS_DIR}/release/lib/libcrypto.lib")
    set(MC_QUICTLS_INCLUDE_DIR "${MC_QUICTLS_DIR}/release/include")
  else()
    set_target_properties(mc_openssl_ssl    PROPERTIES IMPORTED_LOCATION
      "${MC_QUICTLS_DIR}/lib/libssl.a")
    set_target_properties(mc_openssl_crypto PROPERTIES IMPORTED_LOCATION
      "${MC_QUICTLS_DIR}/lib/libcrypto.a")
    set(MC_QUICTLS_INCLUDE_DIR "${MC_QUICTLS_DIR}/include")
  endif()
  file(MAKE_DIRECTORY "${MC_QUICTLS_INCLUDE_DIR}")
  set_target_properties(mc_openssl_ssl    PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MC_QUICTLS_INCLUDE_DIR}")
  set_target_properties(mc_openssl_crypto PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MC_QUICTLS_INCLUDE_DIR}")
  set(MC_OPENSSL_SSL    mc_openssl_ssl)
  set(MC_OPENSSL_CRYPTO mc_openssl_crypto)
else()
  set(MC_OPENSSL_SSL    OpenSSL::SSL)
  set(MC_OPENSSL_CRYPTO OpenSSL::Crypto)
endif()

# libcurl (for registry announcer)
find_package(CURL QUIET)
if(NOT CURL_FOUND)
  find_path(CURL_INCLUDE_DIRS curl/curl.h)
  find_library(CURL_LIBRARIES NAMES libcurl curl)
  if(CURL_INCLUDE_DIRS AND CURL_LIBRARIES)
    set(CURL_FOUND TRUE)
  endif()
endif()

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
  ${UV_LIBRARIES}
  ${CURL_LIBRARIES}
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
# musicchain_static — no separate runtime .dll / .so to ship.
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

if(TARGET CURL::libcurl)
  list(REMOVE_ITEM MC_LINK_LIBS ${CURL_LIBRARIES})
  list(APPEND MC_LINK_LIBS CURL::libcurl)
endif()

if(MINIUPNPC_FOUND)
  list(APPEND MC_LINK_LIBS ${MINIUPNPC_LIBRARIES})
endif()

list(APPEND MC_LINK_LIBS mc_rats_quic)

# librats has been replaced by src/transport/mc_rats_quic.cpp which provides
# the same C API on top of msquic. The player still ships rats.dll separately
# until Phase 2c swaps the Dart FFI bindings to load mc_rats.dll instead.
list(APPEND MC_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/src/transport")

# ---- Windows: socket libs needed by rats_link / peer code ----
if(WIN32)
  list(APPEND MC_LINK_LIBS ws2_32 iphlpapi bcrypt)
endif()

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
  src/consensus/validator.cpp
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
  src/api/server.cpp
  src/api/jsonrpc_server.cpp
  src/api/routes.cpp
  src/api/rats_api.cpp
  src/capi/capi.cpp
)
if(msh3_FOUND)
  list(APPEND MC_SOURCES src/transport/h3_server.cpp src/transport/cert_util.cpp)
  list(APPEND MC_LINK_LIBS msh3)
endif()

# ---- librats (vanilla TCP transport) ----
#
# We build the *full* librats from deps/librats — DHT, GossipSub, ICE,
# file transfer, Noise encryption, the lot — using its stock TCP socket
# layer. The msquic/quictls QUIC bridge that used to live under
# `quic_socket.cpp` was removed on 2026-06-03 after persistent wire-
# alignment bugs (see [[project-revert-to-plain-librats]]); the
# msquic+msh3 deps still build because `src/transport/h3_server.cpp` uses
# them for a separate HTTP/3 listener, but librats no longer touches them.
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
add_library(musicchain SHARED ${MC_SOURCES})
target_include_directories(musicchain PUBLIC ${MC_INCLUDE_DIRS})
target_link_libraries(musicchain PUBLIC ${MC_LINK_LIBS})
target_compile_definitions(musicchain PRIVATE BUILDING_MUSICCHAIN_DLL)
set_target_properties(musicchain PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION 0
  OUTPUT_NAME musicchain
)

# Static library (linked into node executable)
add_library(musicchain_static STATIC ${MC_SOURCES})
target_include_directories(musicchain_static PUBLIC ${MC_INCLUDE_DIRS})
target_link_libraries(musicchain_static PUBLIC ${MC_LINK_LIBS})

# Node executable
add_executable(musicchain-node tools/node_main.cpp tools/node_tui.cpp)
target_include_directories(musicchain-node PRIVATE ${MC_INCLUDE_DIRS})
target_link_libraries(musicchain-node PRIVATE musicchain_static)

# ncurses (Linux) / PDCurses (Windows, via vcpkg) for the home node's
# interactive TUI. Both ship a Curses-compatible API so the same TUI
# source compiles on either; find_package(Curses) picks whichever is
# installed for the platform.
if (WIN32)
    find_package(unofficial-pdcurses CONFIG REQUIRED)
    target_link_libraries(musicchain-node
        PRIVATE unofficial::pdcurses::pdcurses)
    target_compile_definitions(musicchain-node PRIVATE MC_TUI_PDCURSES=1)
else()
    set(CURSES_NEED_NCURSES TRUE)
    find_package(Curses REQUIRED)
    target_include_directories(musicchain-node PRIVATE ${CURSES_INCLUDE_DIRS})
    target_link_libraries(musicchain-node PRIVATE ${CURSES_LIBRARIES})
endif()

endif() # NOT MC_MINI_NODE_ONLY

# Mini-node — VPS rendezvous binary, vanilla librats TCP transport.
#
# Pulls in a focused subset of the wallet stack (libwally BIP39+BIP32,
# secp256k1, OpenSSL signing, keccak256 address derivation, RelayRewardTx
# serialization) so the mini-node can hold its own EVM-shaped identity
# and let the full node credit it for served tunnel traffic via the
# RELAY_REWARD tx type.
add_executable(musicchain-mini-node
  tools/mini_node.cpp
  src/net/load_monitor.cpp
  src/crypto/hash.cpp
  src/crypto/keys.cpp
  src/crypto/keccak256.cpp
  src/crypto/signature.cpp
  src/crypto/wally_bip39.cpp
  src/core/block.cpp
  src/core/transaction.cpp)
target_link_libraries(musicchain-mini-node PRIVATE
  mc_rats_quic
  wallycore
  secp256k1
  ${MC_OPENSSL_SSL}
  ${MC_OPENSSL_CRYPTO})
target_include_directories(musicchain-mini-node PRIVATE
  ${OPENSSL_INCLUDE_DIR}
  ${CMAKE_SOURCE_DIR}/deps/libwally-core/include
  ${CMAKE_SOURCE_DIR}/deps/libwally-core/src/secp256k1/include)
if(WIN32)
  target_link_libraries(musicchain-mini-node PRIVATE ws2_32 iphlpapi bcrypt)
endif()
if(NOT MSVC)
  target_link_libraries(musicchain-mini-node PRIVATE Threads::Threads)
endif()
# Static-link libstdc++/libgcc so the deploy ELF has no C++ runtime ABI
# dependency on whatever glibc/g++ ships on the VPS. glibc itself stays
# dynamic — a fully -static build pulls in a getaddrinfo / NSS hairball.
if(NOT MSVC AND NOT APPLE)
  target_link_options(musicchain-mini-node PRIVATE
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

# h3_smoke — tiny HTTP/3 client used to verify H3Server end-to-end.
if(msh3_FOUND AND NOT MC_MINI_NODE_ONLY)
add_executable(h3_smoke tools/h3_smoke.cpp)
target_link_libraries(h3_smoke PRIVATE msh3)
if(WIN32)
  target_link_libraries(h3_smoke PRIVATE ws2_32)
endif()
endif()

if(NOT MC_MINI_NODE_ONLY)

# Copy static bootstrap file next to the node executable so it can seed it
add_custom_command(TARGET musicchain-node POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/assets/musicchain-bootstrap.txt"
    "$<TARGET_FILE_DIR:musicchain-node>/musicchain-bootstrap.txt"
  COMMENT "Copying bootstrap file to output directory"
)

# Install
include(GNUInstallDirs)
install(TARGETS musicchain musicchain-node
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES include/musicchain.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

endif() # NOT MC_MINI_NODE_ONLY

# Tests (optional)
enable_testing()
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
  add_subdirectory(tests EXCLUDE_FROM_ALL)
endif()
