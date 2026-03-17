#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

#define MIC_DEVICE "hw:0,0"
#define SPK_DEVICE "hw:1,0"
#define RATE 48000
#define CHANNELS 2
#define FORMAT SND_PCM_FORMAT_S32_LE
#define PERIOD_SIZE 6000 

// Hàm thiết lập phần cứng (giữ nguyên sự đơn giản)
int setup_device(snd_pcm_t **handle, char *dev_name, snd_pcm_stream_t stream) {
    int err;
    snd_pcm_hw_params_t *params;
    unsigned int rate = RATE;
    snd_pcm_uframes_t frames = PERIOD_SIZE;
    int dir = 0;

    if ((err = snd_pcm_open(handle, dev_name, stream, 0)) < 0) {
        printf("Lỗi mở %s: %s\n", dev_name, snd_strerror(err));
        return err;
    }

    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(*handle, params);
    snd_pcm_hw_params_set_access(*handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(*handle, params, FORMAT);
    snd_pcm_hw_params_set_channels(*handle, params, CHANNELS);
    snd_pcm_hw_params_set_rate_near(*handle, params, &rate, &dir);
    
    // Đặt buffer lớn gấp 4 lần period để chống Underrun cho Loa
    snd_pcm_uframes_t buffer_size = PERIOD_SIZE * 4; 
    snd_pcm_hw_params_set_buffer_size_near(*handle, params, &buffer_size);
    snd_pcm_hw_params_set_period_size_near(*handle, params, &frames, &dir);

    if ((err = snd_pcm_hw_params(*handle, params)) < 0) {
        printf("Lỗi cấu hình HW params: %s\n", snd_strerror(err));
        return err;
    }
    snd_pcm_hw_params_free(params);

    if ((err = snd_pcm_prepare(*handle)) < 0) {
        printf("Lỗi prepare: %s\n", snd_strerror(err));
        return err;
    }
    return 0;
}

int main() {
    snd_pcm_t *mic_handle, *spk_handle;
    int buffer_bytes = PERIOD_SIZE * CHANNELS * 4; // 32-bit = 4 bytes
    char *buffer;

    printf("--- Audio Loopback với Pre-fill (Đã sửa lỗi DMA) ---\n");

    // --- SỬA LỖI 1: Cấp phát bộ nhớ căn chỉnh (Aligned Memory) ---
    // Bắt buộc đối với AXI DMA trên FPGA thay vì dùng malloc thông thường
    if (posix_memalign((void **)&buffer, 4096, buffer_bytes) != 0) {
        printf("Lỗi cấp phát bộ nhớ!\n");
        return 1;
    }

    // Thiết bị
    if (setup_device(&mic_handle, MIC_DEVICE, SND_PCM_STREAM_CAPTURE) < 0) return 1;
    if (setup_device(&spk_handle, SPK_DEVICE, SND_PCM_STREAM_PLAYBACK) < 0) return 1;

    // --- BƯỚC PRE-FILL CHO LOA ---
    printf("Đang nạp bộ đệm (Pre-filling)...\n");
    memset(buffer, 0, buffer_bytes); 
    snd_pcm_writei(spk_handle, buffer, PERIOD_SIZE);
    snd_pcm_writei(spk_handle, buffer, PERIOD_SIZE);

    printf("Bắt đầu chạy Loopback... Nhấn Ctrl+C để dừng.\n");

    // --- SỬA LỖI 2: Ra lệnh cho Mic bắt đầu thu ---
    // Cần mồi thủ công vì ta đã tốn thời gian ở bước Pre-fill
    snd_pcm_start(mic_handle);

    while (1) {
        // 1. Đọc từ Mic
        int pcm_read = snd_pcm_readi(mic_handle, buffer, PERIOD_SIZE);
        
        if (pcm_read < 0) {
            // Nếu bị lỗi (như tràn bộ đệm hoặc EIO), khôi phục và Start lại
            snd_pcm_prepare(mic_handle);
            snd_pcm_start(mic_handle);
            continue; 
        }

        // --- BƯỚC XỬ LÝ NHIỄU VÀ NHÂN BẢN KÊNH MONO THÀNH STEREO ---
        // Ép kiểu con trỏ char* thành mảng số nguyên 32-bit (do dùng định dạng S32_LE)
        int32_t *samples = (int32_t *)buffer;
        
        // Duyệt qua từng Frame (Mỗi frame pcm_read gồm 2 mẫu: Trái và Phải)
        for (int i = 0; i < pcm_read; i++) {
            // Chỉ số mảng: 
            // - Kênh Trái: i * 2
            // - Kênh Phải: i * 2 + 1
            
            // TRƯỜNG HỢP 1: Chân L/R của INMP441 nối xuống GND (Giọng nói ở kênh Trái, nhiễu ở kênh Phải)
            // Copy giọng nói từ kênh Trái đè lên kênh Phải
            samples[i * 2 + 1] = samples[i * 2]; 

            // TRƯỜNG HỢP 2: Nếu chân L/R của INMP441 nối lên 3.3V (Giọng nói ở Phải, nhiễu ở Trái)
            // Hãy comment dòng trên lại và bỏ comment dòng dưới này:
            // samples[i * 2] = samples[i * 2 + 1]; 
        }
        // -------------------------------------------------------------

        // 2. Ghi ra Loa
        int pcm_write = snd_pcm_writei(spk_handle, buffer, pcm_read);
        
        if (pcm_write < 0) {
            // Nếu Loa bị đói dữ liệu (Broken pipe)
            if (pcm_write == -EPIPE) {
                snd_pcm_prepare(spk_handle);
                // Bơm mồi lại ngay lập tức
                memset(buffer, 0, buffer_bytes);
                snd_pcm_writei(spk_handle, buffer, PERIOD_SIZE);
            } else {
                printf("Lỗi Loa: %s\n", snd_strerror(pcm_write));
            }
        }
    }

    free(buffer);
    snd_pcm_close(mic_handle);
    snd_pcm_close(spk_handle);
    return 0;
}