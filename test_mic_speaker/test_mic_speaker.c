#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>

#define MIC_DEVICE "hw:0,0"
#define SPK_DEVICE "hw:1,0"

int main(){
    snd_pcm_t *mic, *spk;
    snd_pcm_hw_params_t *params;

    int rate = 8000;
    // Yêu cầu 180, nhưng ALSA có thể sẽ đổi thành số khác
    snd_pcm_uframes_t frames = 180; 
    snd_pcm_uframes_t periods = 8; 

    snd_pcm_open(&mic, MIC_DEVICE, SND_PCM_STREAM_CAPTURE, 0);
    snd_pcm_open(&spk, SPK_DEVICE, SND_PCM_STREAM_PLAYBACK, 0);


    snd_pcm_hw_params_malloc(&params);

    // --- Cấu hình Mic ---
    snd_pcm_hw_params_any(mic, params);
    snd_pcm_hw_params_set_access(mic, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(mic, params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(mic, params, 2);
    snd_pcm_hw_params_set_rate_near(mic, params, &rate, 0);
    
    // Gửi yêu cầu frames và periods cho Mic
    snd_pcm_hw_params_set_period_size_near(mic, params, &frames, 0);
    snd_pcm_uframes_t hw_buf_size = frames * periods; 
    snd_pcm_hw_params_set_buffer_size_near(mic, params, &hw_buf_size); 
    snd_pcm_hw_params(mic, params); 

    // --- Cấu hình Loa ---
    snd_pcm_hw_params_any(spk, params);
    snd_pcm_hw_params_set_access(spk, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(spk, params, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(spk, params, 2);
    snd_pcm_hw_params_set_rate_near(spk, params, &rate, 0);
    
    // Gửi yêu cầu frames và periods cho Loa
    snd_pcm_hw_params_set_period_size_near(spk, params, &frames, 0);
    hw_buf_size = frames * periods; 
    snd_pcm_hw_params_set_buffer_size_near(spk, params, &hw_buf_size); 
    snd_pcm_hw_params(spk, params); 

    snd_pcm_hw_params_free(params);

    printf("Phan cung da CHOT period size thuc te la: %lu frames\n", frames);
    
    int buffer_size = frames * 2 * 4; 
    char *buffer = (char *) malloc(buffer_size); 

    printf("Bat dau chay loopback...\n");
    int err;
    
    memset(buffer, 0, buffer_size);
    for(int i = 0; i < 4; i++) {
        snd_pcm_writei(spk, buffer, frames);
    }
    
    snd_pcm_start(mic);

    while(1){
        err = snd_pcm_readi(mic, buffer, frames);
        if(err < 0){
            printf("Loi mic: %s\n", snd_strerror(err));
            snd_pcm_prepare(mic);
            snd_pcm_start(mic);
            continue;
        }

        err = snd_pcm_writei(spk, buffer, frames);
        if(err < 0){
            printf("Loi loa: %s\n", snd_strerror(err));
            snd_pcm_prepare(spk);
            memset(buffer, 0, buffer_size);
            for(int i = 0; i < 4; i++) {
                snd_pcm_writei(spk, buffer, frames);
            }
        }
    }
    
    snd_pcm_close(mic);
    snd_pcm_close(spk);
    free(buffer);

    return 0;
}