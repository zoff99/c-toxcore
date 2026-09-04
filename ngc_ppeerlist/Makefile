TARGET = libngc_ppeerlist.a

all: $(TARGET)

libngc_ppeerlist.o: mid_roster.c
	$(CC) -c -o $@ $< -O2 -Wno-discarded-qualifiers -fPIC -I. -Wl,-Bstatic $(shell pkg-config --cflags --libs libsodium x264) -Wl,-Bdynamic $(shell pkg-config --cflags --libs opus vpx libavcodec libavutil) -pthread

$(TARGET): libngc_ppeerlist.o
	$(AR) rcs $@ $^

test:
	$(CC) -fsanitize=address -g -fno-omit-frame-pointer tox_ngc_tcp_test.c -I. ../amalgamation/libtoxcore.a -lsodium -lopus -lx265 -lx264 -lavcodec -lvpx -lavutil

clean:
	rm -f libngc_ppeerlist.a libngc_ppeerlist.o
