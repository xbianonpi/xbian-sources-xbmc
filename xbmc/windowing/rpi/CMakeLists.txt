set(SOURCES WinSystemRpi.cpp
            RPIUtils.cpp
            VideoSyncPi.cpp)

set(HEADERS WinSystemRpi.h
            RPIUtils.h
            VideoSyncPi.h)

if(TARGET ${APP_NAME_LC}::OpenGLES)
  list(APPEND SOURCES WinSystemRpiGLESContext.cpp)
  list(APPEND HEADERS WinSystemRpiGLESContext.h)
endif()

core_add_library(windowing_Rpi)
