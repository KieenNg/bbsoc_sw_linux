# =================================================
# MAKEFILE CHO TX FIXS TREN LINUX
# =================================================

# 1. Ten file chay cuoi cung (Executable)
TARGET = tx_fixs_app

# 2. Trinh bien dich (Giong voi mic_melp)
CC = aarch64-xilinx-linux-gcc  -mcpu=cortex-a72.cortex-a53 -march=armv8-a+crc -fstack-protector-strong  -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security --sysroot=$(SDKTARGETSYSROOT)

# 3. Co bien dich
CFLAGS = -Wall -O2

# 4. Cac thu muc chua file Header (.h)
#    - ./app    : shared_region.h
#    - ./fixs   : Vhf_FixS_Tx.h va cac header L1/modulation
INCLUDES = -I./app -I./fixs

# 5. Cac thu vien Linux bat buoc phai Link vao
# -liio     : Thu vien IIO (ADRV9002 TX)
# -lrt      : Thu vien Shared Memory (POSIX Realtime)
# -lpthread : Thu vien Mutex / Da luong
# -lm       : Thu vien Toan hoc (Modulation, sin/cos)
LIBS = -liio -lrt -lpthread -lm

# 6. Danh sach cac file Source (.c)
#    Chinh sua duong dan cho dung voi project cua ban
SRCS = app/tx_fixs_linux.c

# Thu vien FixS_Tx (modulation + MELP decode)
# TODO: Them cac file .c cua FixS_Tx vao day
SRCS += fixs/Vhf_FixS_Tx.c

# Neu FixS_Tx can MELP decode (melp_syn), them cac file MELP:
# SRCS += melp/melp_syn.c \
#         melp/melp_sub.c \
#         melp/dsp_sub.c \
#         melp/fft_lib.c \
#         melp/lpc_lib.c \
#         melp/math_lib.c \
#         melp/global.c \
#         melp/coeff.c \
#         melp/fsvq_cb.c \
#         melp/msvq_cb.c \
#         melp/vq_lib.c \
#         melp/fs_lib.c \
#         melp/harm.c \
#         melp/pit_lib.c \
#         melp/fec_code.c \
#         melp/melp_chn.c

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