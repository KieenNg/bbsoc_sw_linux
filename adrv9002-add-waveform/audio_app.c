#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <alsa/asoundlib.h>
#include "sdr_shm.h"

#define MIC_DEVICE "hw:0,0" // Tùy chỉnh lại theo mạch của bạn

int main() {
    printf("[Audio App] Khoi dong va tao Shared Memory...\n");

    // 1. TẠO SHARED MEMORY VÀ ÉP KIỂU
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666); //SHM_NAME là tên, O_CREAT để tạo mới nếu chưa tồn tại, O_RDWR để đọc/ghi, 0666 là quyền truy cập
    if (shm_fd < 0) { perror("Loi tao Shared Memory"); return 1; }
    ftruncate(shm_fd, sizeof(SharedFIFO_t));//mở rộng kích thước của shared memory để đủ chứa cấu trúc SharedFIFO_t
    SharedFIFO_t *fifo = mmap(0, sizeof(SharedFIFO_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    //ánh xạ vùng nhớ ảo này vào thẳng không gian RAM. MAP_SHARED cho phép các tiến trình các cũng có thể truy cập

    // 2. KHỞI TẠO Ổ KHÓA ĐA TIẾN TRÌNH (Chỉ chạy 1 lần ở app này)
    fifo->head = 0; fifo->tail = 0; fifo->count = 0;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED); // Lệnh chốt hạ cho đa tiến trình!
    pthread_mutex_init(&fifo->lock, &attr);

    // 3. CẤU HÌNH ALSA (8000Hz, 180 frames)
    snd_pcm_t *mic;
    snd_pcm_hw_params_t *params;
    int rate = 8000;
    snd_pcm_uframes_t frames = 180;
    snd_pcm_uframes_t periods = 8;
    
    snd_pcm_open(&mic, MIC_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(mic, params);
    snd_pcm_hw_params_set_access(mic, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(mic, params, SND_PCM_FORMAT_S16_LE); // Dùng 16-bit cho MELP
    snd_pcm_hw_params_set_channels(mic, params, 1); // Đa số MELP dùng Mono (1 kênh)
    snd_pcm_hw_params_set_rate_near(mic, params, &rate, 0);
    snd_pcm_hw_params_set_period_size_near(mic, params, &frames, 0);
    snd_pcm_uframes_t hw_buf_size = frames * periods;
    snd_pcm_hw_params_set_buffer_size_near(mic, params, &hw_buf_size);
    snd_pcm_hw_params(mic, params);
    snd_pcm_hw_params_free(params);

    int16_t alsa_buffer[180];  // 180 mẫu Mono
    uint8_t melp_out_bits[60]; // 60 bit đầu ra của MELP

    snd_pcm_start(mic);
    printf("[Audio App] Bat dau thu am...\n");

    while (1) {
        // A. Thu âm (Blocking 22.5ms)
        int err = snd_pcm_readi(mic, alsa_buffer, frames);
        if (err < 0) {
            snd_pcm_prepare(mic); snd_pcm_start(mic); continue;
        }

        // B. GỌI HÀM NÉN MELP TỪ NO-OS
        // melp_encode_frame(alsa_buffer, melp_out_bits);
        memset(melp_out_bits, 1, 60); // Giả lập có 60 bit

        // C. CẤT VÀO KHO
        pthread_mutex_lock(&fifo->lock);
        for (int i = 0; i < 60; i++) {
            fifo->bits[fifo->head] = melp_out_bits[i];
            fifo->head = (fifo->head + 1) % FIFO_SIZE;
            fifo->count++;
        }
        pthread_mutex_unlock(&fifo->lock);
    }
    return 0;
}