SRCS=VideoSyncGLX.cpp \
     VideoSyncCocoa.cpp \
     VideoSyncDRM.cpp \
     VideoSyncPi.cpp \
     VideoSyncIMX.cpp \

LIB=videosync.a

include ../../../Makefile.include
-include $(patsubst %.cpp,%.P,$(patsubst %.c,%.P,$(SRCS)))
