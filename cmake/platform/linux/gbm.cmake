list(APPEND PLATFORM_REQUIRED_DEPS GBM LibDRM>=2.4.95 LibInput Xkbcommon UDEV LibDisplayInfo)

option(ENABLE_MMAL "Enable MMAL support?" ON)
if(PLATFORM MATCHES "raspberry" OR ENABLE_MMAL)
  list(APPEND PLATFORM_REQUIRED_DEPS MMAL)
  message(STATUS "### Building with MMAL ###")
endif()

if(USE_PLATFORM STREQUAL "raspberry-pi4")
  list(APPEND ARCH_DEFINES -DTARGET_RASPBERRY_PI4)
  message(STATUS "### Building for Raspberry Pi2-5 (32bit) ###")
endif()

if(USE_PLATFORM STREQUAL "raspberry-pi5")
  list(APPEND ARCH_DEFINES -DTARGET_RASPBERRY_PI5)
  message(STATUS "### Building for Raspberry Pi3-5 (64bit) ###")
endif()

list(APPEND PLATFORM_OPTIONAL_DEPS VAAPI)

if(APP_RENDER_SYSTEM STREQUAL "gl")
  list(APPEND PLATFORM_REQUIRED_DEPS OpenGl EGL)
elseif(APP_RENDER_SYSTEM STREQUAL "gles")
  list(APPEND PLATFORM_REQUIRED_DEPS OpenGLES EGL)
endif()
