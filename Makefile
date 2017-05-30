TARGET := fixGB
DEBUG   = 0
AUDIO_FLOAT = 1

OBJECTS :=
OBJECTS +=alhelpers.o
OBJECTS +=apu.o
OBJECTS +=audio.o
OBJECTS +=cpu.o
OBJECTS +=input.o
OBJECTS +=main.o
OBJECTS +=mbc.o
OBJECTS +=mem.o
OBJECTS +=ppu.o

FLAGS    += -Wall -Wextra -msse -mfpmath=sse -ffast-math
FLAGS    += -Werror=implicit-function-declaration
DEFINES  += -DFREEGLUT_STATIC
INCLUDES += -I.

ifeq ($(AUDIO_FLOAT),1)
FLAGS += -DAUDIO_FLOAT=1
endif

ifeq ($(DEBUG),1)
FLAGS += -O0 -g
else
FLAGS   += -O3
LDFLAGS += -s
endif

CFLAGS += $(FLAGS) $(DEFINES) $(INCLUDES)

LDFLAGS += $(CFLAGS) -lglut -lopenal -lGL -lGLU -lm

all: $(TARGET)
$(TARGET): $(OBJECTS)
	$(CC) $^ -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS)

clean:
	rm -f $(TARGET) $(OBJECTS)


.PHONY: clean test
