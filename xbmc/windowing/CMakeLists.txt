set(SOURCES GraphicContext.cpp
            OSScreenSaver.cpp
            Resolution.cpp
            WindowSystemFactory.cpp
            WinSystemHeadless.cpp
            WinSystem.cpp)

set(HEADERS GraphicContext.h
            OSScreenSaver.h
            Resolution.h
            WinEvents.h
            WindowSystemFactory.h
            WinSystemHeadless.h
            WinSystem.h
            XBMC_events.h
            VideoSync.h)

core_add_library(windowing)
