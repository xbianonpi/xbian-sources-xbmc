add_subdirectory(drm)

set(SOURCES OptionalsReg.cpp
            WinSystemGbm.cpp
            VideoSyncGbm.cpp
            GBMUtils.cpp
            WinSystemGbmEGLContext.cpp
            GBMDPMSSupport.cpp)

set(HEADERS OptionalsReg.h
            WinSystemGbm.h
            VideoSyncGbm.h
            GBMUtils.h
            WinSystemGbmEGLContext.h
            GBMDPMSSupport.h)

if(TARGET OpenGL::GL)
  list(APPEND SOURCES WinSystemGbmGLContext.cpp)
  list(APPEND HEADERS WinSystemGbmGLContext.h)
endif()
if(TARGET OpenGL::GLES)
  list(APPEND SOURCES WinSystemGbmGLESContext.cpp)
  list(APPEND HEADERS WinSystemGbmGLESContext.h)
endif()

if (LIBVNCSERVER_FOUND)
  list(APPEND SOURCES VNCServer.cpp)
  list(APPEND HEADERS VNCServer.h)
endif()

core_add_library(windowing_gbm)
