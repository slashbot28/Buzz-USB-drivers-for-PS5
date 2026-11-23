PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= 192.168.1.20

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

TARGET := buzz_bridge.elf

CFLAGS  := -D__PROSPERO__ -Wall -Wextra -O2 -fno-stack-protector
LDLIBS  := -lkernel_web -lSceLibcInternal

.PHONY: all clean send

all: $(TARGET)

$(TARGET): buzz_bridge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
	$(STRIP) --strip-debug $@

send: $(TARGET)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p 9021 $(TARGET)

clean:
	rm -f $(TARGET)
