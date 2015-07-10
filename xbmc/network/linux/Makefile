SRCS=NetworkLinux.cpp \
     ZeroconfAvahi.cpp \
     ZeroconfBrowserAvahi.cpp \

LIB=network_linux.a

include ../../../Makefile.include
-include $(patsubst %.cpp,%.P,$(patsubst %.c,%.P,$(SRCS)))
CXXFLAGS += --std=c++11 -DHAVE_CHAR32_T -DHAVE_CHAR16_T
