JAVA_HOME?=/usr/lib/jvm/java-6-sun
JAVA_CFLAGS?=-I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux
JAVA_LDFLAGS?=-ljvm -L$(JAVA_HOME)/jre/lib/amd64/server

CFLAGS?=-Wall -pedantic
CFLAGS+=$(JAVA_CFLAGS)
LDFLAGS+=-lpthread $(JAVA_LDFLAGS)

CAPS_SUPPORT?=1

ifeq ($(CAPS_SUPPORT), 1)
CFLAGS+=-D CAPS_SUPPORT
LDFLAGS+=-Wl,-Bstatic -lcap -Wl,-Bdynamic
endif

all: semeru

%.o: %.c
	$(CC) $(CFLAGS) -c $^ -o $@

semeru: semeru.o
	$(CC) $^ $(LDFLAGS) -o $@
	strip $@

clean:
	rm -f *.o semeru

install: all
	mkdir -p $(DESTDIR)/usr/sbin/
	install -m 755 semeru $(DESTDIR)/usr/sbin/
