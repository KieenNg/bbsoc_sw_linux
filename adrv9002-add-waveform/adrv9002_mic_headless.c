//****************************************************************************//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xil_cache.h"
#include "xil_io.h"

#include "axi_dmac.h"

#include "parameters.h"

#include "melpe.h"
#include "global.h"
#include "dsp_sub.h"
#include "melp_sub.h"
#include "constant.h"
#include "math_lib.h"
#include "math.h"

#define MELP_DATA_SIZE 54
#define AUD_DATA_SIZE  180
#define BUFFER_DEPTH   128

#define WRITE_IDX  (*(volatile unsigned int *)(0xFFFC0000 + 0 * sizeof(unsigned int)))
#define READ_IDX   (*(volatile unsigned int *)(0xFFFC0000 + 1 * sizeof(unsigned int)))
#define START_FLAG (*(volatile unsigned int *)(0xFFFC0000 + 2 * sizeof(unsigned int)))
#define TX_READY   (*(volatile unsigned int *)(0xFFFC0000 + 3 * sizeof(unsigned int)))
#define MIC_READY  (*(volatile unsigned int *)(0xFFFC0000 + 4 * sizeof(unsigned int)))

#define DAC_DDR_BASEADDR    XPAR_DDR_MEM_BASEADDR + 0x40000000
#define MIC_DDR_BASEADDR    XPAR_DDR_MEM_BASEADDR + 0x50000000
#define SHARED_DDR_BASEADDR XPAR_DDR_MEM_BASEADDR + 0x60000000

uintptr_t shared_buffer_addr[BUFFER_DEPTH];

uintptr_t mic_buffer_addr[2] = {
    MIC_DDR_BASEADDR + 0 * 2 * AUD_DATA_SIZE * sizeof(uint32_t),
    MIC_DDR_BASEADDR + 1 * 2 * AUD_DATA_SIZE * sizeof(uint32_t),
};

void mic_process(uint32_t* input_data, DATA* output_data)
{
    /*
        MIC process
        input_data[180] -> melp_analysis() -> melp_encode [54]
    */
    DATA voice_in[AUD_DATA_SIZE];

    // extract voice L/R data (16 bit)
    for(int i = 0; i < AUD_DATA_SIZE; i = i + 1) {
        voice_in[i] = (DATA)input_data[2*i + 0];
    }

    analysis(voice_in, (ushort*)output_data);

    // for(int i = 0; i < MELP_DATA_SIZE; i = i + 1) {
    //  printf("%d", output_data[i]);
    // }

    return;
}

int main(void)
{
    int ret;
    struct axi_dmac *mic_dmac;
    int ping_idx = 0;
    int pong_idx = 1;

    for(int i = 0; i < BUFFER_DEPTH; i++) {
        shared_buffer_addr[i] = SHARED_DDR_BASEADDR + i * MELP_DATA_SIZE * sizeof(DATA);
    }

    // Init Hardware
    printf("Core1: Hello\n");

    struct axi_dmac_init mic_dmac_init = {
        "mic_dmac",
        MIC_DMA_BASEADDR,
        IRQ_DISABLED
    };

    ret = axi_dmac_init(&mic_dmac, &mic_dmac_init);
    if (ret) {
        printf("axi_dmac_init() failed with status %d\n", ret);
        return -1;
    }

    Xil_ICacheEnable();
    Xil_DCacheEnable();

    melp_ana_init();
    melp_syn_init();

    // Setup DMA
    struct axi_dma_transfer transfer_mic = {
        // Number of bytes to write/read
        // .size = 2 * 1200 * sizeof(uint32_t),
        .size = 2 * AUD_DATA_SIZE * sizeof(uint32_t),
        // Transfer done flag
        .transfer_done = 0,
        // Signal transfer mode
        .cyclic = NO,
        // Address of data source
        .src_addr = 0,
        // Address of data destination
        .dest_addr = 0
    };

    // Config Audio (aud_mclk = 24.576 MHz)
    Xil_Out32(MIC_I2S_BASEADDR + 0x20, 24);         // setup divider clock 24.576 MHz = 3.072 * (2 * 4)
    Xil_Out32(MIC_I2S_BASEADDR + 0x08, 0x1);        // enable mic i2s

    printf("Core1: Config Mic Done\n");

    // Test mic
    MIC_READY = 1;
    while(!(TX_READY && MIC_READY)) {}
    START_FLAG = 1;

    // config and start mic dma
    transfer_mic.dest_addr = mic_buffer_addr[ping_idx];
    axi_dmac_write(mic_dmac, AXI_DMAC_REG_IRQ_PENDING, 3); // clear irq
    axi_dmac_transfer_start(mic_dmac, &transfer_mic);

    while(1) {
        // for(int i = 0; i < 16; i++){
            // wait mic complete
            axi_dmac_transfer_wait_completion(mic_dmac, 50000);
            Xil_DCacheInvalidateRange(mic_buffer_addr[ping_idx], 2 * AUD_DATA_SIZE * sizeof(uint32_t));

            // config and start mic dma
            transfer_mic.dest_addr = mic_buffer_addr[pong_idx];
            axi_dmac_write(mic_dmac, AXI_DMAC_REG_IRQ_PENDING, 3); // clear irq
            axi_dmac_transfer_start(mic_dmac, &transfer_mic);

            // melp process
            mic_process((uint32_t*)mic_buffer_addr[ping_idx], (DATA*)shared_buffer_addr[WRITE_IDX]);

            // circular buffer
            WRITE_IDX = WRITE_IDX + 1;
            if(WRITE_IDX == BUFFER_DEPTH) { WRITE_IDX = 0; }

            // swap idx
            ping_idx = ping_idx ^ 1;
            pong_idx = pong_idx ^ 1;
        // }
    }

    printf("Core1: Bye\n");
}