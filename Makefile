
TARGET = player
SOURCE_FILES = ${wildcard ./*.c}
CC = gcc
RM = rm -rf
LDFLAG = -lpthread -lavformat -lswscale -lavcodec -lavutil -lswresample -lasound -lm -lva -lX11 -lssl -lcrypto -lz -lva-x11 -lva-drm -lva-x11 -lvdpau -lSDL2
LDFLAG += -L./lib/lib
INCLUDE = -I./lib/include
MACRO = 

OBJECTS = $(patsubst %.c,%.o, $(SOURCE_FILES))
#OBJECTS := $(foreach item,$(SOURCE_FILES),$(CC) -c $(item) -o $(patsubst %.c,%.o, $(item)) $(INCLUDE) $(MACRO))
#gcc -E  hello.c  -o hello.i
#PRECOMPILE_FILES := $(foreach item,$(SOURCE_FILES),$(CC) -E $(item) -o $(patsubst %.c,%.i, $(item)) $(INCLUDE) $(MACRO))
#COMPILE_FILES := $(foreach item,$(SOURCE_FILES),$(CC) -S $(item) -o $(patsubst %.c,%.s, $(item)) $(INCLUDE) $(MACRO))
#COMPILE_FILES = $(patsubst %.c,%.s, $(SOURCE_FILES))

$(TARGET): $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAG)

#gcc  -c  hello.s  -o  hello.o
$(OBJECTS):$(SOURCE_FILES)
	$(CC) -c $^ $(INCLUDE)

#gcc  -S  hello.i   -o  hello.s
#$(COMPILE_FILES):$(PRECOMPILE_FILES)
	#$(CC) -S $^


clean:
	$(RM) $(TARGET) $(OBJECTS)
