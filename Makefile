
TARGET = player
SOURCE_FILES = ${wildcard ./*.c}
CC = gcc
LDFLAG = -lpthread
INCLUDE = -I./lib/include


$(TARGET):
	$(CC) -o $(SOURCE_FILES) $(LDFLAG)
