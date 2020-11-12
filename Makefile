
TARGET = player
SOURCE_FILES = ${wildcard ./*.c}
CC = gcc
RM = rm -rf
LDFLAG = -lpthread -lavformat -lswscale -lavcodec -lavutil -lswresample -lasound -lm -lva -lX11 -lssl -lcrypto -lz -lva-x11 -lva-drm -lva-x11 -lvdpau -lSDL2
LDFLAG += -L./lib/lib
INCLUDE = -I./lib/include


$(TARGET):
	$(CC) -o $@ $(SOURCE_FILES) $(LDFLAG)

clean:
	$(RM) $(TARGET)
