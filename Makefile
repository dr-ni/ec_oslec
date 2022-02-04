CC := gcc
LD := gcc

all: oec fifolib

oec: src/audio.c src/fifo.c src/pa_ringbuffer.c src/util.c src/oec.c
	$(CC) src/audio.c src/fifo.c src/pa_ringbuffer.c src/util.c src/oec.c -O3 -ldl -lm -Wl,-Bstatic -Wl,-Bdynamic -lrt -lpthread -lasound -o oec

fifolib: src/pcm_fifo.c
	$(CC) src/pcm_fifo.c -Wall -c -o pcm_fifo.o
	@echo LD $@
	$(LD) -I. -Wall -funroll-loops -ffast-math -fPIC -DPIC -O0 -g pcm_fifo.o -Wall -shared -o libasound_module_pcm_fifo.so

clean:
	@echo Cleaning...
	rm -vf *.o *.so	src/*.o ec

install:
	@echo Installing...
	mkdir -p /usr/lib/alsa-lib/
	install -m 644 libasound_module_pcm_fifo.so /usr/lib/alsa-lib/
	install -m 755 oec /usr/local/bin/
uninstall:
	@echo Uninstalling...
	rm /usr/lib/alsa-lib/libasound_module_pcm_fifo.so
	rm /usr/local/bin/oec
