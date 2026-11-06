CC ?= gcc
SRC = ../src
CFLAGS = -std=c11 -g -O1 -Wall -Wextra -Wno-unused-parameter -I$(SRC) -I. -fsanitize=address,undefined -fno-sanitize-recover=undefined
OBJS = test_host.c sim_flash.c $(SRC)/uf2_fs.c $(SRC)/scsi.c $(SRC)/app_state.c

test_host: $(OBJS) $(SRC)/*.h sim_flash.h
	$(CC) $(CFLAGS) $(OBJS) -o $@

run: test_host
	python3 ../tools/make_test_app.py out
	python3 ../tools/bin2uf2.py out/app.bin -o out/app.uf2
	ASAN_OPTIONS=detect_leaks=0 ./test_host out/app.uf2 out/app.bin out/uf2.img
	python3 check_fat.py out/uf2.img

clean:
	rm -rf test_host out
