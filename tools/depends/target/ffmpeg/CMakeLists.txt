project(ffmpeg)

cmake_minimum_required(VERSION 3.12)

if(ENABLE_CCACHE AND CCACHE_PROGRAM)
  set(ffmpeg_conf "--cc=${CCACHE_PROGRAM} ${CMAKE_C_COMPILER}"
                  "--cxx=${CCACHE_PROGRAM} ${CMAKE_CXX_COMPILER}"
                )
else()
  set(ffmpeg_conf --cc=${CMAKE_C_COMPILER}
                  --cxx=${CMAKE_CXX_COMPILER}
                )
endif()

if(CROSSCOMPILING)
  set(pkgconf "PKG_CONFIG_LIBDIR=${DEPENDS_PATH}/lib/pkgconfig")
  list(APPEND ffmpeg_conf --pkg-config=${PKG_CONFIG_EXECUTABLE}
                          --pkg-config-flags=--static
                          --enable-cross-compile
                          --enable-pic
                          --ar=${CMAKE_AR}
                          --ranlib=${CMAKE_RANLIB}
                          --strip=${CMAKE_STRIP}
              )

  message(STATUS "CROSS: ${ffmpeg_conf}")
endif()

if(CPU MATCHES aarch64 OR CPU MATCHES arm64)
  set(CONFIGARCH --arch=${CPU})
else()
  set(CONFIGARCH --cpu=${CPU} --arch=${CMAKE_SYSTEM_ARCH})
endif()

list(APPEND ffmpeg_conf --disable-doc
                        --disable-devices
                        --disable-programs
                        --disable-sdl2
                        --disable-vulkan
                        --enable-gpl
                        --enable-postproc
                        --enable-runtime-cpudetect
                        --enable-pthreads
                        --extra-version="Kodi"
            )

message(STATUS "************** PLATFORM: ${PLATFORM} - CORE_PLATFORM_NAME: ${CORE_PLATFORM_NAME} - USE_PLATFORM: ${USE_PLATFORM}")
if(CORE_PLATFORM_NAME STREQUAL rbpi)
  message(STATUS "************** FFMPEG for Raspberry")
  string(CONCAT CMAKE_C_FLAGS ${CMAKE_C_FLAGS} " -I/opt/vc/include -I/opt/vc/include/interface/vcos/pthreads -I/opt/vc/include/interface/vmcs_host/linux")
  list(APPEND ffmpeg_conf --enable-rpi --disable-ffmpeg --enable-mmal --extra-version="")
elseif(PLATFORM MATCHES "gbm")
  if(USE_PLATFORM MATCHES "raspberry")
    message(STATUS "************** FFMPEG for Raspberry 4")
    list(APPEND ffmpeg_conf --enable-sand --enable-v4l2_m2m --enable-v4l2-request --enable-libdrm --enable-vout-drm --enable-libudev --disable-hwaccel=h264_v4l2request --disable-hwaccel=mpeg2_v4l2request --disable-hwaccel=vp8_v4l2request --disable-hwaccel=vp9_v4l2request)
    #get_cmake_property(_variableNames VARIABLES)
    #list (SORT _variableNames)
    #foreach (_variableName ${_variableNames})
    #  message(STATUS "${_variableName}=${${_variableName}}")
    #endforeach()
    if(ENABLE_MMAL)
      message(STATUS "************** FFMPEG with MMAL")
      string(CONCAT CMAKE_C_FLAGS ${CMAKE_C_FLAGS} " -I/opt/vc/include -I/opt/vc/include/interface/vcos/pthreads -I/opt/vc/include/interface/vmcs_host/linux")
      string(CONCAT CMAKE_EXE_LINKER_FLAGS ${CMAKE_EXE_LINKER_FLAGS} " -Wl,--copy-dt-needed-entries -L/opt/vc/lib -lvcsm")
      string(CONCAT CMAKE_MODULE_LINKER_FLAGS ${CMAKE_MODULE_LINKER_FLAGS} " -L/opt/vc/lib")
      list(APPEND ffmpeg_conf --enable-mmal --enable-ffmpeg)
    else()
      message(STATUS "************** FFMPEG without MMAL")
      list(APPEND ffmpeg_conf --disable-mmal --disable-ffmpeg)
    endif()
  else()
    message(STATUS "************** FFMPEG for Cubox-i")
    list(APPEND ffmpeg_conf --enable-hwaccels --enable-v4l2_m2m --enable-libdrm --disable-libudev --disable-v4l2-request --disable-ffmpeg)
  endif()
else()
  message(STATUS "************** FFMPEG for Other")
  list(APPEND ffmpeg_conf --enable-libdrm --enable-v4l2_m2m --enable-hwaccels --disable-ffmpeg --disable-mmal --disable-devices)
endif()

if(CMAKE_C_FLAGS)
  list(APPEND ffmpeg_conf --extra-cflags=${CMAKE_C_FLAGS})
endif()

if(CMAKE_CXX_FLAGS)
  list(APPEND ffmpeg_conf --extra-cxxflags=${CMAKE_CXX_FLAGS})
endif()

if(CMAKE_EXE_LINKER_FLAGS)
  list(APPEND ffmpeg_conf --extra-ldflags=${CMAKE_EXE_LINKER_FLAGS})
endif()

if(ENABLE_NEON)
  list(APPEND ffmpeg_conf --enable-neon)
endif()

if(CMAKE_BUILD_TYPE STREQUAL Release)
  list(APPEND ffmpeg_conf --disable-debug)
endif()

if(CORE_SYSTEM_NAME STREQUAL linux OR CORE_SYSTEM_NAME STREQUAL freebsd)
  list(APPEND ffmpeg_conf --enable-pic
                          --target-os=linux
              )
  if(CORE_PLATFORM_NAME STREQUAL rbpi)
    list(APPEND ffmpeg_conf --cpu=${CPU} --disable-vaapi --disable-vdpau)
  else()
    if(ENABLE_VAAPI)
      list(APPEND ffmpeg_conf --enable-vaapi)
    else()
      list(APPEND ffmpeg_conf --disable-vaapi)
    endif()
    if(ENABLE_VDPAU)
      list(APPEND ffmpeg_conf --enable-vdpau)
    else()
      list(APPEND ffmpeg_conf --disable-vdpau)
    endif()
  endif()
elseif(CORE_SYSTEM_NAME STREQUAL android)
  list(APPEND ffmpeg_conf --target-os=android
                          --extra-libs=-liconv
                          --disable-linux-perf
              )
  if(CPU MATCHES arm64)
    set(CONFIGARCH --arch=aarch64)
    list(APPEND ffmpeg_conf --cpu=cortex-a53)
  elseif(CPU MATCHES arm)
    list(APPEND ffmpeg_conf --cpu=cortex-a9)
  elseif(CPU MATCHES x86_64)
    list(APPEND ffmpeg_conf --cpu=x86_64)
    list(APPEND ffmpeg_conf --extra-cflags=-mno-stackrealign)
  else()
    list(APPEND ffmpeg_conf --cpu=i686 --disable-mmx --disable-asm)
    list(APPEND ffmpeg_conf --extra-cflags=-mno-stackrealign)
  endif()
elseif(CORE_SYSTEM_NAME STREQUAL darwin_embedded)
  list(APPEND ffmpeg_conf --disable-crystalhd
                          --enable-videotoolbox
                          --disable-filter=yadif_videotoolbox
                          --target-os=darwin
              )
elseif(CORE_SYSTEM_NAME STREQUAL osx)
  list(APPEND ffmpeg_conf --disable-crystalhd
                          --enable-videotoolbox
                          --target-os=darwin
                          --disable-securetransport
              )
endif()

if(CPU MATCHES arm)
  list(APPEND ffmpeg_conf --disable-armv5te --disable-armv6t2)
elseif(CPU MATCHES mips)
  list(APPEND ffmpeg_conf --disable-mips32r2 --disable-mipsdsp --disable-mipsdspr2)
endif()

find_package(GnuTLS)
if(GNUTLS_FOUND)
  list(APPEND ffmpeg_conf --enable-gnutls)
endif()

if(CPU MATCHES x86 OR CPU MATCHES x86_64)
  find_package(NASM REQUIRED)
  list(APPEND ffmpeg_conf --x86asmexe=${NASM_EXECUTABLE})
endif()

if(ENABLE_DAV1D)
  list(APPEND ffmpeg_conf --enable-libdav1d)
  set(pkgconf_path "PKG_CONFIG_PATH=${PKG_CONFIG_PATH}")
else()
  list(APPEND ffmpeg_conf --disable-libdav1d)
endif()

if(EXTRA_FLAGS)
  string(REPLACE " " ";" EXTRA_FLAGS ${EXTRA_FLAGS})
  list(APPEND ffmpeg_conf ${EXTRA_FLAGS})
endif()

list(APPEND ffmpeg_conf ${CONFIGARCH})

list(APPEND ffmpeg_conf --enable-libspeex)

message(STATUS "FFMPEG_CONF: ${ffmpeg_conf}")

set(MAKE_COMMAND $(MAKE))
if(CMAKE_GENERATOR STREQUAL Ninja)
  set(MAKE_COMMAND make)
  include(ProcessorCount)
  ProcessorCount(N)
  if(NOT N EQUAL 0)
    set(MAKE_COMMAND make -j${N})
  endif()
endif()

include(ExternalProject)
externalproject_add(ffmpeg
                    SOURCE_DIR ${CMAKE_SOURCE_DIR}
                    CONFIGURE_COMMAND ${pkgconf} ${pkgconf_path} <SOURCE_DIR>/configure
                      --prefix=${CMAKE_INSTALL_PREFIX}
                      --extra-version="kodi-${FFMPEG_VER}"
                      ${ffmpeg_conf}
                    BUILD_COMMAND ${MAKE_COMMAND})

install(CODE "Message(Done)")

# Quell warnings
set(BUILD_SHARED_LIBS)
set(XBMC_BUILD_DIR)
set(KODI_BUILD_DIR)
