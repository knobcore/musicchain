cmake_minimum_required(VERSION 3.22)
project(bopwire_android C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- bopwire root --------------------------------------------------
# From android/app/src/main/cpp/ → bopwire root is 7 dirs up.
set(MC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../bopwire")

# ---- libwally-core (static, vendored) --------------------------------
#
# libwally bundles its own secp256k1-zkp. Building both libwally's
# secp256k1 and a standalone secp256k1 here creates a target-name clash
# (both register a `secp256k1` target), so we let libwally own the
# secp256k1 build and link bopwire against libwally's copy.
#
# These flags match the desktop / home-node build (bopwire/CMakeLists.txt
# lines 290+): static-only, no tests, the modules wally needs and nothing
# else. Keeping them in sync means the same mnemonic derives the same
# secp256k1 key on every platform.
set(BUILD_SHARED_LIBS         OFF CACHE BOOL "" FORCE)
set(WALLYCORE_ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(WALLYCORE_INSTALL         OFF CACHE BOOL "" FORCE)
set(WALLYCORE_BUILD_ELEMENTS  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH        ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY    ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ELLSWIFT    OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_GENERATOR   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RANGEPROOF  ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SURJECTIONPROOF ON CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_WHITELIST   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_MUSIG       OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDSA_ADAPTOR OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDSA_S2C   ON  CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_BPPP        OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK           OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS    OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS         OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES            OFF CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL                   OFF CACHE BOOL "" FORCE)

add_subdirectory("${MC_ROOT}/deps/libwally-core" libwally_build EXCLUDE_FROM_ALL)

# ---- Build the bopwire shared library -----------------------------
add_library(bopwire SHARED
    bopwire_android.c
    "${MC_ROOT}/src/crypto/keccak256_c.c")
target_include_directories(bopwire PRIVATE
    "${MC_ROOT}/include"
    "${MC_ROOT}/src/crypto"
    "${MC_ROOT}/deps/libwally-core/include"
    "${MC_ROOT}/deps/libwally-core/src/secp256k1/include")
target_link_libraries(bopwire PRIVATE wallycore secp256k1 log)
target_compile_definitions(bopwire PRIVATE BOPWIRE_BUILD=1)

# ---- librats shared library (loaded via Dart FFI at runtime) ---------
set(LIBRATS_DIR "${MC_ROOT}/deps/librats")
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS       ON  CACHE BOOL "" FORCE)
set(RATS_SHARED_LIBRARY ON  CACHE BOOL "" FORCE)
set(RATS_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(RATS_VERSION_OVERRIDE "1.0.0" CACHE STRING "" FORCE)
add_subdirectory("${LIBRATS_DIR}" librats_build)

# The librats CMake target is named `rats`, whose default output is
# `librats.so`. But the Dart FFI loads `libmc_rats.so` (see rats_client.dart
# _loadLibrary). Without this rename the build produced librats.so while the app
# loaded a STALE prebuilt jniLibs/libmc_rats.so — so none of the freshly-built
# transport ever reached the device. Force the output name so the compiled
# librats IS what gets loaded. (The bopwire root CMakeLists does the same for
# the desktop/node builds; the player builds librats directly so must repeat it.)
set_target_properties(rats PROPERTIES OUTPUT_NAME mc_rats)

# ---- chromaprint JNI bridge ------------------------------------------
#
# Lets Kotlin call chromaprint_new/start/feed/finish/get_fingerprint
# directly without bouncing PCM through MethodChannel + Dart FFI.
# Drops one full PCM round-trip + a Dart-side memcpy per fingerprinted
# song, so an album scan finishes ~2-3× faster.
set(CHROMAPRINT_HEADER_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../chromaprint-1.5.1/src/include")

add_library(chromaprint_jni SHARED chromaprint_jni.cpp)
target_include_directories(chromaprint_jni PRIVATE ${CHROMAPRINT_HEADER_DIR})

# Import the prebuilt chromaprint .so as a SHARED IMPORTED target so
# the linker can resolve chromaprint_* symbols and the runtime loader
# picks the version in jniLibs.
add_library(chromaprint_prebuilt SHARED IMPORTED)
set_target_properties(chromaprint_prebuilt PROPERTIES
    IMPORTED_LOCATION
        "${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libchromaprint.so")
target_link_libraries(chromaprint_jni PRIVATE chromaprint_prebuilt log)
