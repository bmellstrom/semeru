all: jexec

CFLAGS=-Wall -I/usr/lib/jvm/java-6-sun/include -I/usr/lib/jvm/java-6-sun/include/linux
LDFLAGS=-lcap -ldl -lpthread

%.o: %.c
	gcc $(CFLAGS) -c $^ -o $@

jexec: jexec.o
	gcc $(LDFLAGS) $^ -o $@

clean:
	rm -f *.o jexec
