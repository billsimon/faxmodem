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
)

target_include_directories(faxmodem PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
