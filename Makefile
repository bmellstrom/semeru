all: jexec

CFLAGS=-Wall -I/usr/lib/jvm/java-6-sun/include -I/usr/lib/jvm/java-6-sun/include/linux
LDFLAGS=-ldl -lpthread
ifdef CAPS_SUPPORT
CFLAGS+=-D CAPS_SUPPORT
LDFLAGS+=-lcap
endif

%.o: %.c
	$(CC) $(CFLAGS) -c $^ -o $@

jexec: jexec.o
	$(CC) $(LDFLAGS) $^ -o $@

clean:
	rm -f *.o jexec
