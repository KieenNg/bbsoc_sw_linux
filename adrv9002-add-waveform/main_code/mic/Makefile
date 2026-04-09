# =================================================
# MAKEFILE CHO MIC + MELP TREN LINUX
# =================================================

# 1. Ten file chay cuoi cung (Executable)
TARGET = mic_melp_app

# 2. Trinh bien dich (Dung gcc mac dinh cua PetaLinux hoac Cross-compiler)
CC = aarch64-xilinx-linux-gcc  -mcpu=cortex-a72.cortex-a53 -march=armv8-a+crc -fstack-protector-strong  -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security --sysroot=$(SDKTARGETSYSROOT)

# 3. Co bien dich: Hien thi canh bao (-Wall) va Toi uu hoa toc do (-O2)
CFLAGS = -Wall -O2

# 4. Cac thu muc chua file Header (.h)
INCLUDES = -I./melp

# 5. Cac thu vien Linux bat buoc phai Link vao
# -lasound  : Thu vien ALSA (Mic)
# -lrt      : Thu vien Shared Memory (POSIX Realtime)
# -lpthread : Thu vien Mutex / Da luong
# -lm       : Thu vien Toan hoc (Cuc ky quan trong cho MELP FFT/Trig)
LIBS = -lasound -lrt -lpthread -lm

# 6. Danh sach cac file Source (.c)
SRCS = app/mic_melp_os.c

SRCS += melp/coeff.c \
        melp/dsp_sub.c \
        melp/fec_code.c \
        melp/fft_lib.c \
        melp/fs_lib.c \
        melp/fsvq_cb.c \
        melp/global.c \
        melp/harm.c \
        melp/lpc_lib.c \
        melp/math_lib.c \
        melp/melp_ana.c \
        melp/melp_chn.c \
        melp/melp_sub.c \
        melp/melp_syn.c \
        melp/msvq_cb.c \
        melp/pit_lib.c \
        melp/vq_lib.c

OBJS = $(SRCS:.c=.o)

# =================================================
# CAC LUAT BIEN DICH (BUILD RULES)
# =================================================

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LIBS)
	@echo "=> Build thanh cong! Chay file bang lenh: ./$(TARGET)"

# Khau Compile: Bien file .c thanh file .o
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Lenh don dep (Xoa file rac khi go "make clean")
clean:
	rm -f $(OBJS) $(TARGET)
	@echo "=> Da don dep sach se!"