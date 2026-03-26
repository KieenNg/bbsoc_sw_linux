#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <iio.h>
#include "sdr_shm.h"

int main() {
    printf("[RF App] Ket noi vao Shared Memory...\n");

    // 1. CHỈ MỞ KHO (KHÔNG TẠO MỚI)
    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) { 
        printf("Loi: Khong thay Shared Memory! Chay audio_app truoc.\n"); 
        return 1; 
    }
    SharedFIFO_t *fifo = mmap(0, sizeof(SharedFIFO_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    // 2. KHỞI TẠO LIBIIO CHO ADRV9002 (Giả lập cấu trúc)
    // struct iio_context *ctx = iio_create_default_context();
    // struct iio_device *tx_dev = iio_context_find_device(ctx, "axi-adrv9002-tx-lpc");
    // struct iio_buffer *tx_buf = iio_device_create_buffer(tx_dev, 160, false);
    
    uint8_t turbo_in_240[240];
    uint8_t turbo_out_720[720];
    int16_t iq_buffer[320]; // 160 I + 160 Q

    printf("[RF App] Bat dau vong lap 2ms...\n");

    while (1) {
        int co_du_hang = 0;

        // A. NGÓ VÀO KHO HÚT 240 BIT
        pthread_mutex_lock(&fifo->lock);
        if (fifo->count >= 240) {
            for (int i = 0; i < 240; i++) {
                turbo_in_240[i] = fifo->bits[fifo->tail];
                fifo->tail = (fifo->tail + 1) % FIFO_SIZE;
                fifo->count--;
            }
            co_du_hang = 1;
        }
        pthread_mutex_unlock(&fifo->lock);

        // B. XỬ LÝ VÀ PHÁT SÓNG
        if (co_du_hang) {
            // [NHÉT HÀM NO-OS CỦA BẠN VÀO ĐÂY]
            // turbo_encode(turbo_in_240, turbo_out_720);
            // bpsk_modulate_and_upsample(turbo_out_720, iq_buffer);
            
            // iio_buffer_push(tx_buf); 
        } else {
            // KHO CHƯA ĐỦ 240 BIT -> PHÁT SÓNG MANG TRỐNG ĐỂ GIỮ KẾT NỐI
            memset(iq_buffer, 0, sizeof(iq_buffer));
            // iio_buffer_push(tx_buf);
        }

        // C. NHỊP TIM CỦA HỆ THỐNG RF (2ms)
        usleep(2000); 
    }
    return 0;
}