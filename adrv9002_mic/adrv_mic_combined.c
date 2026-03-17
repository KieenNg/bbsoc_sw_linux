#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include <iio.h>

#define MIC_DEVICE "hw:0,0"
#define ADRV9002_TX_DEV "axi-adrv9002-tx-lpc"
#define SAMPLES_COUNT 8192  // Cyclic buffer size

int main(){
    // ==========================================
    // 1. KHỞI TẠO ALSA (MIC)
    // ==========================================
    snd_pcm_t *mic;
    snd_pcm_hw_params_t *params;
    
    int rate = 8000;
    snd_pcm_uframes_t frames = 180;  // Read 180 frames per cycle
    int buffer_size = frames * 2 * 4;  // frame x 2 channel x 4 bytes
    char *alsa_buffer = (char *) malloc(buffer_size);
    
    if (!alsa_buffer) {
        printf("Loi: Khong the cap phat ALSA buffer!\n");
        return -1;
    }

    // Mở thiết bị mic
    int ret = snd_pcm_open(&mic, MIC_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        printf("Loi: Khong the mo mic: %s\n", snd_strerror(ret));
        free(alsa_buffer);
        return -1;
    }

    // Cấu hình phần cứng
    ret = snd_pcm_hw_params_malloc(&params);
    if (ret < 0) {
        printf("Loi: Khong the cap phat params: %s\n", snd_strerror(ret));
        free(alsa_buffer);
        snd_pcm_close(mic);
        return -1;
    }

    snd_pcm_hw_params_any(mic, params);
    snd_pcm_hw_params_set_access(mic, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(mic, params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(mic, params, 2);
    snd_pcm_hw_params_set_rate_near(mic, params, &rate, 0);
    snd_pcm_hw_params_set_period_size_near(mic, params, &frames, 0);
    snd_pcm_hw_params(mic, params);
    snd_pcm_hw_params_free(params);
    
    snd_pcm_start(mic);

    // ==========================================
    // 2. KHỞI TẠO LIBIIO (ADRV9002 TX)
    // ==========================================
    struct iio_context *ctx = iio_create_network_context("localhost");
    if (!ctx) {
        printf("Loi: Khong the ket noi IIO network (iiod dang chay?)\n");
        printf("Thu: iiod -F\n");
        free(alsa_buffer);
        snd_pcm_close(mic);
        return -1;
    }

    struct iio_device *tx_device = iio_context_find_device(ctx, ADRV9002_TX_DEV);
    if (!tx_device) {
        printf("Loi: Khong tim thay thiet bi %s.\n", ADRV9002_TX_DEV);
        iio_context_destroy(ctx);
        free(alsa_buffer);
        snd_pcm_close(mic);
        return -1;
    }

    // Tìm và bật 4 kênh (I0, Q0, I1, Q1)
    struct iio_channel *ch_i0 = iio_device_find_channel(tx_device, "voltage0", true);
    struct iio_channel *ch_q0 = iio_device_find_channel(tx_device, "voltage1", true);
    struct iio_channel *ch_i1 = iio_device_find_channel(tx_device, "voltage2", true);
    struct iio_channel *ch_q1 = iio_device_find_channel(tx_device, "voltage3", true);
    
    if (!ch_i0 || !ch_q0 || !ch_i1 || !ch_q1) {
        printf("Loi: Khong tim thay cac kenh voltage.\n");
        iio_context_destroy(ctx);
        free(alsa_buffer);
        snd_pcm_close(mic);
        return -1;
    }
    
    iio_channel_enable(ch_i0);
    iio_channel_enable(ch_q0);
    iio_channel_enable(ch_i1);
    iio_channel_enable(ch_q1);

    // Tạo CYCLIC buffer
    struct iio_buffer *tx_buf = iio_device_create_buffer(tx_device, SAMPLES_COUNT, true);
    if (!tx_buf) {
        printf("Loi: Khong the tao IIO buffer!\n");
        iio_context_destroy(ctx);
        free(alsa_buffer);
        snd_pcm_close(mic);
        return -1;
    }

    // Set ADRV9002 to TX mode
    struct iio_device *phy_device = iio_context_find_device(ctx, "adrv9002-phy");
    if (phy_device) {
        printf("Dang ep ADRV9002 sang che do TX...\n");
        iio_device_attr_write(phy_device, "ensm_mode", "tx");
        usleep(500000);  // Wait 500ms for mode transition
        printf("ADRV9002 da chuyen sang TX mode.\n");
    }

    printf("Bat dau truyen giong noi tu Mic qua ADRV9002...\n");
    printf("An enter de dung chuong trinh\n");

    // ==========================================
    // 3. MAIN LOOP: Capture từ Mic, ghi vào ADRV9002
    // ==========================================
    int cycle = 0;
    int err;
    int global_buf_index = 0;

    // Khởi tạo cyclic buffer
    iio_buffer_push(tx_buf);

    while(1) {
        // Print progress
        if (cycle % 100 == 0) {
            printf("cycle %d\n", cycle);
        }

        // 1. Đọc từ Mic
        err = snd_pcm_readi(mic, alsa_buffer, frames);
        if (err < 0) {
            printf("Loi mic: %s\n", snd_strerror(err));
            if (snd_pcm_recover(mic, err, 1) < 0) {
                printf("Loi: Khong the phuc hoi tu loi mic\n");
                break;
            }
            continue;
        }

        // 2. Chuyển đổi và ghi vào cyclic buffer
        int32_t *mic_ptr = (int32_t *)alsa_buffer;
        
        // Ghi 1024 mẫu từ mic vào buffer
        for (int i = 0; i < frames; i++) {
            // Đọc 2 channel từ mic (L, R)
            int32_t mic_left  = *mic_ptr++;
            int32_t mic_right = *mic_ptr++;

            // Dịch 16-bit để lấy phần MSB
            int16_t tx_left  = (int16_t)(mic_left >> 16);
            int16_t tx_right = (int16_t)(mic_right >> 16);

            // Tính vị trí trong cyclic buffer
            size_t buf_pos = global_buf_index % SAMPLES_COUNT;
            
            // Lấy con trỏ tới vị trí hiện tại trong buffer
            int16_t *buf_start = (int16_t *)iio_buffer_start(tx_buf);
            ptrdiff_t buf_step = iio_buffer_step(tx_buf) / sizeof(int16_t);
            int16_t *frame_ptr = buf_start + buf_pos * buf_step;
            
            // Ghi vào 4 kênh ADRV9002 (I0, Q0, I1, Q1)
            frame_ptr[0] = tx_left;   // I0
            frame_ptr[1] = tx_right;  // Q0
            frame_ptr[2] = tx_left;   // I1
            frame_ptr[3] = tx_right;  // Q1
            
            global_buf_index++;
        }

        cycle++;
        usleep(1000);  // Small delay
    }

    // ==========================================
    // 4. CLEANUP
    // ==========================================
    printf("Dang dung chuong trinh...\n");
    if (tx_buf)
        iio_buffer_destroy(tx_buf);
    if (ctx)
        iio_context_destroy(ctx);
    if (mic)
        snd_pcm_close(mic);
    if (alsa_buffer)
        free(alsa_buffer);

    printf("Ket thuc.\n");
    return 0;
}
