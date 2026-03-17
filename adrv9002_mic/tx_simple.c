#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <alsa/asoundlib.h>
#include <iio.h>

#define MIC_DEVICE "hw:0,0"
#define TX_DEVICE "axi-adrv9002-tx-lpc"
#define BUFFER_SIZE 8192

volatile int running = 1;

void sig_handler(int sig) {
    running = 0;
}

int main() {
    printf("=== ADRV9002 TX (Mic -> ADRV9002) ===\n\n");

    // ========== INIT MIC ==========
    snd_pcm_t *mic;
    snd_pcm_hw_params_t *params;
    unsigned int rate = 48000;
    snd_pcm_uframes_t frames = 1024;

    snd_pcm_open(&mic, MIC_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    snd_pcm_hw_params_malloc(&params);
    snd_pcm_hw_params_any(mic, params);
    snd_pcm_hw_params_set_access(mic, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(mic, params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(mic, params, 2);
    snd_pcm_hw_params_set_rate_near(mic, params, &rate, 0);
    snd_pcm_hw_params_set_period_size_near(mic, params, &frames, 0);
    snd_pcm_hw_params(mic, params);
    snd_pcm_hw_params_free(params);
    snd_pcm_start(mic);

    // Allocate mic buffer
    int mic_buf_size = frames * 2 * 4;  // 2 channels, S32_LE
    char *mic_buf = (char *)malloc(mic_buf_size);

    // ========== INIT IIO (ADRV9002) ==========
    struct iio_context *ctx = iio_create_default_context();
    if (!ctx) {
        fprintf(stderr, "Error: Cannot create IIO context\n");
        return -1;
    }

    struct iio_device *tx_dev = iio_context_find_device(ctx, TX_DEVICE);
    if (!tx_dev) {
        fprintf(stderr, "Error: Cannot find TX device\n");
        return -1;
    }

    // Find and enable 4 channels
    struct iio_channel *ch[4] = {
        iio_device_find_channel(tx_dev, "voltage0", true),
        iio_device_find_channel(tx_dev, "voltage1", true),
        iio_device_find_channel(tx_dev, "voltage2", true),
        iio_device_find_channel(tx_dev, "voltage3", true)
    };

    for (int i = 0; i < 4; i++) {
        if (!ch[i]) {
            fprintf(stderr, "Error: Cannot find channel %d\n", i);
            return -1;
        }
        iio_channel_enable(ch[i]);
    }

    // Create cyclic buffer
    struct iio_buffer *tx_buf = iio_device_create_buffer(tx_dev, BUFFER_SIZE, true);
    if (!tx_buf) {
        fprintf(stderr, "Error: Cannot create TX buffer\n");
        return -1;
    }

    // Set TX mode
    struct iio_device *phy_dev = iio_context_find_device(ctx, "adrv9002-phy");
    if (phy_dev) {
        iio_device_attr_write(phy_dev, "ensm_mode", "tx");
        usleep(500000);
        printf("ADRV9002 set to TX mode\n");
    }

    printf("Starting TX (Mic -> ADRV9002)...\n");
    printf("Press Ctrl+C to stop\n\n");
    signal(SIGINT, sig_handler);

    iio_buffer_push(tx_buf);  // Initialize cyclic buffer

    int cycle = 0;
    int buf_idx = 0;

    // ========== MAIN LOOP ==========
    while (running) {
        // Read from mic
        int err = snd_pcm_readi(mic, mic_buf, frames);
        if (err < 0) {
            snd_pcm_recover(mic, err, 1);
            continue;
        }

        // Convert and write to IIO buffer
        int32_t *mic_data = (int32_t *)mic_buf;
        int16_t *buf_ptr = (int16_t *)iio_buffer_start(tx_buf);
        int buf_step = iio_buffer_step(tx_buf) / sizeof(int16_t);

        for (int i = 0; i < frames; i++) {
            int16_t left = (int16_t)(mic_data[i * 2] >> 16);
            int16_t right = (int16_t)(mic_data[i * 2 + 1] >> 16);

            size_t pos = buf_idx % BUFFER_SIZE;
            int16_t *dst = buf_ptr + pos * buf_step;

            dst[0] = left;   // I0
            dst[1] = right;  // Q0
            dst[2] = left;   // I1
            dst[3] = right;  // Q1

            buf_idx++;
        }

        if (cycle % 100 == 0) {
            printf("TX cycle %d\n", cycle);
        }
        cycle++;
    }

    // ========== CLEANUP ==========
    printf("\nShutting down...\n");
    iio_buffer_destroy(tx_buf);
    iio_context_destroy(ctx);
    snd_pcm_close(mic);
    free(mic_buf);
    printf("Done.\n");

    return 0;
}
