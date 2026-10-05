cmake_minimum_required(VERSION 3.16)
project(faxmodem VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(FAXMODEM_HARDENING "Compile and link with the usual hardening flags" ON)
option(FAXMODEM_LTO "Link-time optimisation in release builds" ON)
option(FAXMODEM_WERROR "Treat warnings as errors" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SPANDSP REQUIRED IMPORTED_TARGET spandsp)
pkg_check_modules(TIFF REQUIRED IMPORTED_TARGET libtiff-4)
pkg_check_modules(PJPROJECT REQUIRED IMPORTED_TARGET libpjproject)

find_package(Threads REQUIRED)
find_package(OpenSSL)

# libpjproject.pc lists only the pj* libraries; the codecs and SRTP that
# pjproject bundles sit beside them as static archives and still have to be
# linked in explicitly.
set(PJ_BUNDLED_LIBS "")
foreach(_pj_extra g7221codec gsmcodec ilbccodec resample speex srtp webrtc yuv)
  foreach(_pj_dir ${PJPROJECT_LIBRARY_DIRS} ${PJPROJECT_STATIC_LIBRARY_DIRS})
    file(GLOB _pj_found "${_pj_dir}/lib${_pj_extra}*.a")
    if(_pj_found)
      list(APPEND PJ_BUNDLED_LIBS ${_pj_found})
      break()
    endif()
  endforeach()
endforeach()
if(PJ_BUNDLED_LIBS)
  message(STATUS "pjproject bundled libraries: ${PJ_BUNDLED_LIBS}")
endif()

# Stamp what went into this binary, so a running service can say so in its log.
# SOURCE_DATE_EPOCH is honoured for reproducible builds.
if(DEFINED ENV{SOURCE_DATE_EPOCH})
  set(FAXMODEM_BUILD_DATE "epoch:$ENV{SOURCE_DATE_EPOCH}")
else()
  string(TIMESTAMP FAXMODEM_BUILD_DATE "%Y-%m-%dT%H:%M:%SZ" UTC)
endif()
configure_file(include/faxmodem/build_info.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/faxmodem/build_info.h" @ONLY)

add_executable(faxmodem
  src/main.c
  src/config.c
  src/log.c
  src/fax.c
  src/sip.c
  src/spool.c
  src/tiff_probe.c
  src/util.c
  src/v34_codec.c
  src/v34_dsp.c
  src/v34_info.c
  src/v34_cc.c
  src/v34hdx.c
)

target_include_directories(faxmodem PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")

# spandsp's T.30 engine and fax front end, vendored from 0.0.6 and patched for
# V.34 (T.30 Annex F), and its V.17 receiver, fixed for fixed point builds;
# they override the installed library's copies. See third_party/spandsp/README.md.
set(T30_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/spandsp")
set(T30_SOURCES "${T30_DIR}/t30.c" "${T30_DIR}/t30_api.c" "${T30_DIR}/t30_logging.c" "${T30_DIR}/fax.c"
    "${T30_DIR}/v17rx.c")
target_sources(faxmodem PRIVATE ${T30_SOURCES})
# They include spandsp's headers one by one, never <spandsp.h>, which is the
# only place the installed library says whether it was built fixed point - and
# the modem structures inside fax_state_t are laid out differently each way.
# So read it from there, and tell the vendored files the same.
set(T30_DEFS HAVE_CONFIG_H)
find_file(SPANDSP_MAIN_HEADER spandsp.h PATHS ${SPANDSP_INCLUDE_DIRS} NO_DEFAULT_PATH)
if(NOT SPANDSP_MAIN_HEADER)
  message(FATAL_ERROR "cannot find the installed spandsp.h in ${SPANDSP_INCLUDE_DIRS}")
endif()
file(STRINGS "${SPANDSP_MAIN_HEADER}" _spandsp_fixed REGEX "^#define SPANDSP_USE_FIXED_POINT")
if(_spandsp_fixed)
  list(APPEND T30_DEFS SPANDSP_USE_FIXED_POINT=1)
  message(STATUS "spandsp: fixed point build")
else()
  message(STATUS "spandsp: floating point build")
endif()
# -fwrapv: spandsp does phase arithmetic in int32_t and relies on it wrapping,
# which C leaves undefined; clang at -O2 then broke V.17's short training.
set_source_files_properties(${T30_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;-fwrapv"
    INCLUDE_DIRECTORIES "${T30_DIR}"
    COMPILE_DEFINITIONS "${T30_DEFS}")
target_link_libraries(faxmodem PRIVATE
  PkgConfig::PJPROJECT
  ${PJ_BUNDLED_LIBS}
  PkgConfig::SPANDSP
  PkgConfig::TIFF
  Threads::Threads
)

if(OPENSSL_FOUND)
  target_link_libraries(faxmodem PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

# powf/lrintf, for the selftest's simulated line. Part of libc on macOS.
if(UNIX AND NOT APPLE)
  target_link_libraries(faxmodem PRIVATE m)
endif()

target_compile_options(faxmodem PRIVATE
  -Wall -Wextra -Wno-unused-parameter -Wshadow
  $<$<BOOL:${FAXMODEM_WERROR}>:-Werror>
)

# -O2 rather than -O3: the modem work is spandsp's, and -O2 is the setting it
# is actually tested with.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

if(FAXMODEM_HARDENING)
  target_compile_options(faxmodem PRIVATE
    -fstack-protector-strong
    -fno-strict-aliasing          # spandsp and pjsip both type-pun
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>
  )
  if(NOT APPLE)
    # PIE plus full RELRO; the macOS toolchain does both by default.
    target_compile_options(faxmodem PRIVATE -fPIE)
    target_link_options(faxmodem PRIVATE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)
  endif()
endif()

if(FAXMODEM_LTO AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_why)
  if(_ipo_ok)
    set_property(TARGET faxmodem PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "link-time optimisation enabled")
  else()
    message(STATUS "link-time optimisation unavailable: ${_ipo_why}")
  endif()
endif()

if(APPLE)
  # pjproject's audio device layer pulls these in even though we run headless.
  target_link_libraries(faxmodem PRIVATE
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework AudioUnit"
  )
endif()

install(TARGETS faxmodem RUNTIME DESTINATION bin)
install(FILES examples/faxmodem.conf DESTINATION share/faxmodem/examples)
install(FILES examples/example.job DESTINATION share/faxmodem/examples)
install(FILES examples/faxmodem.service DESTINATION share/faxmodem/examples)
install(PROGRAMS scripts/make-test-page.sh DESTINATION bin RENAME faxmodem-make-test-page)

# Unit tests for the parts of V.34 that have to agree with the far end bit for
# bit, with no signal processing in between: `ctest`, or build/v34test.
enable_testing()
add_executable(v34test tests/v34_test.c src/v34_codec.c src/v34_info.c src/v34_dsp.c src/v34_cc.c src/log.c
               src/util.c)
target_include_directories(v34test PRIVATE src include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(v34test PRIVATE -Wall -Wextra -Wno-unused-parameter -Wshadow)
target_link_libraries(v34test PRIVATE Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(v34test PRIVATE m)
endif()
add_test(NAME v34 COMMAND v34test)

# Two half-duplex V.34 modems back to back over a simulated line: start-up,
# the control channel both ways, pages on the primary channel.
add_executable(v34hdxtest tests/v34hdx_test.c src/v34hdx.c src/v34_cc.c src/v34_codec.c src/v34_info.c
               src/v34_dsp.c src/log.c src/util.c)
target_include_directories(v34hdxtest PRIVATE src include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(v34hdxtest PRIVATE -Wall -Wextra -Wno-unused-parameter -Wshadow)
target_link_libraries(v34hdxtest PRIVATE PkgConfig::SPANDSP PkgConfig::TIFF Threads::Threads)
if(UNIX AND NOT APPLE)
  target_link_libraries(v34hdxtest PRIVATE m)
endif()
add_test(NAME v34hdx COMMAND v34hdxtest)
