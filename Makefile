CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
AR = $(CROSS_COMPILE)ar

ifdef WIN32
SUFFIX=.exe
OS_SPECIFIC=os_win32.o
OS_CFLAGS=
else
SUFFIX=
OS_SPECIFIC=os_posix.o
OS_CFLAGS= -DHAVE_ASSERT_H
endif

EXTRA_DEFS = $(OS_CFLAGS) -D_FILE_OFFSET_BITS=64
CFLAGS=-g -pipe -O0 -Wall -Wsign-compare -Wcast-align -Waggregate-return -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wmissing-noreturn -finline-functions -Wmissing-format-attribute -fwrapv -Iinclude $(EXTRA_DEFS)

# Static libraries
BINS = httpd
HTTPD_OBJ = httpd.o http_conn.o vec.o \
		nbio.o nbio-epoll.o nbio-poll.o \
		nbio-listener.o \
		hgang.o os.o
HTTPD_SLIBS = ../libaio/src/libaio.a
HTTPD_LIBS = 

ALL_OBJS = $(HTTPD_OBJ)
ALL_TARGETS = $(BINS)

TARGET = all

.PHONY: all clean dep

all: dep $(BINS)

dep: Make.dep

Make.dep: Makefile *.c include/*.h
	$(CC) $(CFLAGS) -MM $(patsubst %.o, %.c, $(ALL_OBJS)) > $@

%.o: Makefile %.c
	$(CC) $(CFLAGS) -c -o $@ $(patsubst %.o, %.c, $@)

httpd$(SUFFIX): $(HTTPD_OBJ) $(HTTPD_SLIBS)
	$(CC) $(HTTPD_LIBS) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ALL_TARGETS) $(ALL_OBJS) Make.dep

ifeq (Make.dep, $(wildcard Make.dep))
include Make.dep
endif
