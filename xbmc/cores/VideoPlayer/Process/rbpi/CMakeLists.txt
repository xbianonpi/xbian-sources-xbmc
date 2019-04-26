if(PLATFORM MATCHES raspberry OR ENABLE_MMAL) # ** KM 28.03
  set(SOURCES ProcessInfoPi.cpp)

  set(HEADERS ProcessInfoPi.h)

  core_add_library(processPi)
endif()
