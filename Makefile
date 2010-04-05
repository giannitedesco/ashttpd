CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
AR = $(CROSS_COMPILE)ar

EXTRA_DEFS = -D_FILE_OFFSET_BITS=64 -DHAVE_ACCEPT4=1
CFLAGS=-g -pipe -Os -Wall -Wsign-compare -Wcast-align -Waggregate-return -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations -Wmissing-noreturn -finline-functions -Wmissing-format-attribute -fwrapv -Iinclude $(EXTRA_DEFS)

HTTPD_BIN = httpd
HTTPD_SLIBS = ../libaio/src/libaio.a
HTTPD_LIBS = 
HTTPD_OBJ = httpd.o \
		http_parse.o \
		http_req.o \
		http_buf.o \
		webroot.o \
		io_sync.o \
		io_sendfile.o \
		io_async.o \
		io_dasync.o \
		io_async_sendfile.o \
		nbio.o \
		nbio-epoll.o \
		nbio-poll.o \
		nbio-listener.o \
		nbio-eventfd.o \
		rbtree.o \
		hgang.o \
		vec.o \
		os.o

#MAKEROOT_OBJ = makeroot.o skunk_make.o hgang.o fobuf.o os.o
#MAKEROOT_SLIBS =
#MAKEROOT_LIBS =

HTTPRAPE_BIN = httprape
HTTPRAPE_SLIBS =
HTTPRAPE_LIBS = 
HTTPRAPE_OBJ = httprape.o \
		markov.o \
		http_parse.o \
		http_resp.o \
		http_buf.o \
		nbio.o \
		nbio-epoll.o \
		nbio-poll.o \
		nbio-connecter.o \
		hgang.o \
		vec.o \
		os.o

BINS = $(HTTPD_BIN) $(HTTPRAPE_BIN)
ALL_OBJS = $(HTTPD_OBJ)
ALL_TARGETS = $(BINS)

TARGET = all

.PHONY: all clean dep root walk

all: dep $(BINS)

dep: Make.dep

Make.dep: Makefile *.c include/*.h
	$(CC) $(CFLAGS) -MM include/*.h *.h \
		$(patsubst %.o, %.c, $(ALL_OBJS)) > $@

%.o: Makefile %.c
	$(CC) $(CFLAGS) -c -o $@ $(patsubst %.o, %.c, $@)

$(HTTPD_BIN): $(HTTPD_OBJ) $(HTTPD_SLIBS)
	$(CC) $(HTTPD_LIBS) $(CFLAGS) -o $@ $^

$(HTTPRAPE_BIN): $(HTTPRAPE_OBJ) $(HTTPRAPE_SLIBS)
	$(CC) $(HTTPRAPE_LIBS) $(CFLAGS) -o $@ $^

#makeroot: $(MAKEROOT_OBJ) $(MAKEROOT_SLIBS)
#	$(CC) $(MAKEROOT_LIBS) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ALL_TARGETS) $(ALL_OBJS) Make.dep

webroot.h: makeroot MANIFEST ROOT
	./makeroot `cat ROOT` < MANIFEST
root: webroot.h

markov.c: mkmarkov WALK
	./mkmarkov < WALK
walk: markov.c

ifeq (Make.dep, $(wildcard Make.dep))
include Make.dep
endif
