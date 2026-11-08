# `just app hello` runs this with the firmware's toolchain in CC and the folder
# to install on the phone in OUT. Static: nothing depends on the image's libraries.

CC ?= cc
OUT ?= out

all:
	mkdir -p $(OUT)
	$(CC) -O2 -std=gnu17 -D_FORTIFY_SOURCE=3 -Wall -Wextra -Wno-missing-field-initializers -Wshadow -Wvla -Wimplicit-fallthrough -Wstrict-prototypes -Wmissing-prototypes -static -o $(OUT)/hello hello.c
	cp app.json $(OUT)/
