# `just app hello` runs this with the firmware's toolchain in CC and the folder
# to install on the phone in OUT. Static: nothing depends on the image's libraries.

CC ?= cc
OUT ?= out

all:
	mkdir -p $(OUT)
	$(CC) -O2 -Wall -Wshadow -Wvla -Wimplicit-fallthrough -Wstrict-prototypes -static -o $(OUT)/hello hello.c
	cp app.json $(OUT)/
