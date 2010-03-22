CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
AR = $(CROSS_COMPILE)ar

EXTRA_DEFS = $(OS_CFLAGS) -D_FILE_OFFSET_BITS=64 -DHAVE_ACCEPT4=1
CFLAGS=-g -pipe -Os -Wall -Wsign-compare -Wcast-align -Waggregate-return -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wmissing-noreturn -finline-functions -Wmissing-format-attribute -fwrapv -Iinclude $(EXTRA_DEFS)

HTTPD_OBJ = httpd.o http_parse.o http_buf.o webroot.o \
		io_sync.o io_sendfile.o \
		io_async.o io_dasync.o io_async_sendfile.o \
		nbio.o nbio-epoll.o nbio-poll.o \
		nbio-listener.o nbio-eventfd.o \
		vec.o hgang.o os.o \
		boyer-moore.o rbtree.o
HTTPD_SLIBS = ../libaio/src/libaio.a
HTTPD_LIBS = 

#MAKEROOT_OBJ = makeroot.o skunk_make.o hgang.o fobuf.o os.o
#MAKEROOT_SLIBS =
#MAKEROOT_LIBS =

BINS = httpd
ALL_OBJS = $(HTTPD_OBJ)
ALL_TARGETS = $(BINS)

TARGET = all

.PHONY: all clean dep root

all: dep $(BINS)

dep: Make.dep

Make.dep: Makefile *.c include/*.h
	$(CC) $(CFLAGS) -MM include/*.h *.h \
		$(patsubst %.o, %.c, $(ALL_OBJS)) > $@

%.o: Makefile %.c
	$(CC) $(CFLAGS) -c -o $@ $(patsubst %.o, %.c, $@)

httpd: $(HTTPD_OBJ) $(HTTPD_SLIBS)
	$(CC) $(HTTPD_LIBS) $(CFLAGS) -o $@ $^

#makeroot: $(MAKEROOT_OBJ) $(MAKEROOT_SLIBS)
#	$(CC) $(MAKEROOT_LIBS) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ALL_TARGETS) $(ALL_OBJS) Make.dep

root: webroot.h
webroot.h: makeroot MANIFEST ROOT
	./makeroot `cat ROOT` < MANIFEST

ifeq (Make.dep, $(wildcard Make.dep))
include Make.dep
endif
