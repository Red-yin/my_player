
TARGET = player
SOURCE_FILES = ${wildcard ./*.c}
CC = gcc
RM = rm -rf
LDFLAG = -lpthread -lSDL2 -lasound -lavformat -lavcodec -lavutil
INCLUDE = -I./lib/include


$(TARGET):
	$(CC) -o $@ $(SOURCE_FILES) $(LDFLAG)

clean:
	$(RM) $(TARGET)
