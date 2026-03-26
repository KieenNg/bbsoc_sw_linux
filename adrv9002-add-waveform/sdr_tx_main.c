#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <iio.h>

// =========================================================================
// 1. CẤU TRÚC NHÀ KHO (THREAD-SAFE RING BUFFER) DÀNH CHO CÁC BIT MELP
// =========================================================================
#define FIFO_SIZE 1024 // Chứa tối đa 1024 bit MELP (Dư sức chứa 240 bit)

typedef struct {
    uint8_t bits[FIFO_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t lock;
} BitFIFO_t;

BitFIFO_t tx_fifo;

// Hàm khởi tạo FIFO
void fifo_init(BitFIFO_t *fifo) {
    fifo->head = 0; fifo->tail = 0; fifo->count = 0;
    pthread_mutex_init(&fifo->lock, NULL);
}

// Hàm nhét dữ liệu vào FIFO (Thread 1 dùng)
void fifo_push(BitFIFO_t *fifo, uint8_t *data, int len) {
    pthread_mutex_lock(&fifo->lock);
    for (int i = 0; i < len; i++) {
        fifo->bits[fifo->head] = data[i];
        fifo->head = (fifo->head + 1) % FIFO_SIZE;
        fifo->count++;
    }
    pthread_mutex_unlock(&fifo->lock);
}

// Hàm múc dữ liệu ra khỏi FIFO (Thread 2 dùng)
// Trả về 1 nếu đủ dữ liệu (240 bit), trả về 0 nếu chưa đủ
int fifo_pop_if_enough(BitFIFO_t *fifo, uint8_t *out_data, int required_len) {
    int success = 0;
    pthread_mutex_lock(&fifo->lock);
    if (fifo->count >= required_len) {
        for (int i = 0; i < required_len; i++) {
            out_data[i] = fifo->bits[fifo->tail];
            fifo->tail = (fifo->tail + 1) % FIFO_SIZE;
            fifo->count--;
        }
        success = 1;
    }
    pthread_mutex_unlock(&fifo->lock);
    return success;
}

// =========================================================================
// 2. LUỒNG 1: THU ÂM VÀ NÉN MELP (Chạy mỗi 22.5ms)
// =========================================================================
void* thread_audio_tx(void* arg) {
    printf("[Audio Thread] Khoi dong...\n");
    // (BẠN PASTE CODE KHỞI TẠO ALSA VÀO ĐÂY: rate=8000, frames=180...)
    
    int16_t alsa_buffer[180 * 2]; // Khung 180 mẫu
    uint8_t melp_out_bits[60];    // 54 bit + 6 bit padding

    while(1) {
        // 1. Chờ lấy 180 mẫu âm thanh (Mất 22.5ms, CPU sẽ tự động ngủ ở đây)
        // err = snd_pcm_readi(mic, alsa_buffer, 180);
        // if(err < 0) { snd_pcm_prepare(mic); snd_pcm_start(mic); continue; }

        // 2. GỌI HÀM CỦA NO-OS: Nén âm thanh thành 60 bit
        // melp_encode_frame(alsa_buffer, melp_out_bits);

        // Giả lập tạo dữ liệu 60 bit cho template chạy thử
        memset(melp_out_bits, 1, 60); 

        // 3. Vứt 60 bit này vào Nhà Kho (FIFO)
        fifo_push(&tx_fifo, melp_out_bits, 60);
        
        // printf("[Audio Thread] Da day 60 bit vao FIFO (Tong: %d bit)\n", tx_fifo.count);
        usleep(22500); // Giả lập độ trễ ALSA 22.5ms để test
    }
    return NULL;
}

// =========================================================================
// 3. LUỒNG 2: MÃ HÓA TURBO, ĐIỀU CHẾ VÀ PHÁT ADRV (Chạy cực nhanh mỗi 2ms)
// =========================================================================
void* thread_rf_tx(void* arg) {
    printf("[RF Thread] Khoi dong...\n");
    // (BẠN PASTE CODE KHỞI TẠO LIBIIO VÀO ĐÂY: buf size = 160 frames...)
    
    uint8_t turbo_in_240[240];
    uint8_t turbo_out_720[720];
    int16_t iq_buffer[160 * 2]; // Bộ đệm chứa 160 mẫu I,Q đẩy ra ADRV (320 int16)

    // Biến quản lý mẻ I,Q cục bộ
    int iq_samples_ready = 0; 

    while(1) {
        // KIỂM TRA: Đã tiêu thụ hết I/Q chưa? Nếu hết, ta đi xin thêm bit từ FIFO
        if (iq_samples_ready < 160) {
            
            // Vào kho xem có đủ 240 bit (4 khung MELP) chưa?
            if (fifo_pop_if_enough(&tx_fifo, turbo_in_240, 240)) {
                
                // CÓ ĐỦ HÀNG! Bắt đầu chuỗi thuật toán No-OS của bạn:
                // printf("[RF Thread] Gom du 240 bit. Bat dau ma hoa Turbo...\n");
                
                // 1. turbo_encode(turbo_in_240, turbo_out_720);
                
                // 2. Vòng lặp băm 720 bit thành các khối 35 bit -> add 5 bit pad -> BPSK -> Upsample
                // (Paste hàm BPSK và Upsample x4 của bạn vào đây để tạo ra mảng I,Q)
                
                // Sau khối này, bạn sẽ có một mảng lớn I,Q sẵn sàng phát.
                // Ở template này, ta giả lập đã tạo xong các mẫu I,Q:
                iq_samples_ready = 10000; // Giả sử tạo ra 10000 mẫu I,Q từ 720 bit
            }
        }

        // BƯỚC ĐẨY RA ANTEN (Luôn phải chạy mỗi 2ms, không được chờ)
        if (iq_samples_ready >= 160) {
            // Có dữ liệu I/Q: Lấy 160 mẫu chép vào IIO Buffer và phát
            // iio_buffer_push(tx_buf);
            iq_samples_ready -= 160; 
        } else {
            // ĐÓI DỮ LIỆU: ALSA thu chưa kịp. 
            // KHÔNG ĐƯỢC CHẶN. Phải đẩy "Im lặng" (I=0, Q=0) ra ADRV để giữ sóng.
            memset(iq_buffer, 0, sizeof(iq_buffer));
            // iio_buffer_push(tx_buf); 
        }

        usleep(2000); // Giả lập hàm iio_buffer_push() ngốn đúng 2ms
    }
    return NULL;
}

// =========================================================================
// 4. HÀM MAIN: NGƯỜI NHẠC TRƯỞNG
// =========================================================================
int main() {
    printf("--- KHOI DONG HE THONG SDR (PTHREADS) ---\n");

    // Khởi tạo Nhà kho
    fifo_init(&tx_fifo);

    // Khai báo 2 luồng
    pthread_t thread_1, thread_2;

    // Sinh ra luồng Audio
    if (pthread_create(&thread_1, NULL, thread_audio_tx, NULL) != 0) {
        perror("Loi tao luong Audio"); return 1;
    }

    // Sinh ra luồng RF
    if (pthread_create(&thread_2, NULL, thread_rf_tx, NULL) != 0) {
        perror("Loi tao luong RF"); return 1;
    }

    // Lệnh này bắt hàm main() phải chờ, không cho nó kết thúc sớm để 2 luồng kia còn chạy
    pthread_join(thread_1, NULL);
    pthread_join(thread_2, NULL);

    return 0;
}