/*
 * Vhf_FixS_Tx.c — Phiên bản Linux
 *
 * Thay đổi so với baremetal:
 *   1. Turbo Encode IP: Xil_Out64/Xil_In64 → volatile pointer qua /dev/mem
 *   2. Shared memory: shared_buffer_addr[READ_IDX] → shm->data[shm->read_idx]
 *   3. FixS_TxInit(): bỏ tính địa chỉ DDR, nhận con trỏ shm từ main
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <arm_neon.h>

#include "Vhf_FixS_Tx.h"
#include "shared_region.h"

/* ================================================================
 * Turbo Encode IP — thay thế Xil_Out64/Xil_In64
 *
 * Baremetal: Xil_Out64(0xA0004000, val)
 * Linux:    *cfig_reg = val  (qua /dev/mem + mmap)
 * ================================================================ */
#define TURBO_CFIG_PHYS_ADDR   0xA0004000
#define TURBO_INPUT_PHYS_ADDR  0xA0005000
#define TURBO_OUTPUT_PHYS_ADDR 0xA0006000
#define TURBO_MAP_SIZE         0x1000       /* 4KB per register page */

static volatile uint64_t *cfig_reg  = NULL;
static volatile uint64_t *input_reg = NULL;
static volatile uint64_t *output_reg = NULL;
static int devmem_fd = -1;

/* ================================================================
 * Shared memory — thay thế shared_buffer_addr[] + READ_IDX
 *
 * Baremetal: vld1q_s16((DATA*)(shared_buffer_addr[READ_IDX] + ...))
 * Linux:    shm_ptr->data[shm_ptr->read_idx][...]
 * ================================================================ */
static struct shared_region *shm_ptr = NULL;

/* ================================================================
 * Biến giữ nguyên từ baremetal
 * ================================================================ */
DATA DataI[160], DataQ[160];
DATA rx_status = 0;
DATA RxDataOut[432];

/************************ Constant Definitions ************************/
const DATA DummyPattern[480] = {8192,8192,-8192,8192,-8192,8192,-8192,-8192,-8192,8192,-8192,8192,8192,-8192,-8192};

const DATA dPreamblePattern_Tx[240] = {-8192,8192,8192,8192,-8192,8192,-8192,8192,-8192,8192,-8192,-8192,8192,8192,
                                       8192,-8192,8192,8192,-8192,8192,-8192,8192,-8192,8192,-8192,-8192,8192,8192};

const DATA dFrePattern_Tx[200] = {8192,8192,8192,-8192,8192,8192,-8192,8192,8192,-8192,8192,8192,-8192,8192,
                                  8192,8192,8192,-8192,8192,8192,-8192,8192,8192,-8192,8192,8192,-8192,8192};

const DATA dProbePattern_Tx[40] = {-8192,-8192,-8192,-8192,8192,8192,8192,8192,
                                   -8192,8192,8192,-8192,-8192,-8192,-8192,8192,
                                   8192,-8192,-8192,-8192,8192,8192,8192,8192,
                                   -8192,8192,8192,-8192,-8192,-8192,-8192,8192,
                                   8192,8192,8192,-8192,-8192,-8192,-8192,-8192};

const DATA PrefixPattern_Tx[4] = {-8192, -8192, -8192, 8192};

const DATA GuardPattern_Tx[4] = {8192, 8192, -8192, 8192};

const DATA RRC_Up4_FIXS_Tx[NHRC_UP4_FIXS] = {-16,   -7,    12,    20,    8,   -14,   -21,    -4,
                                             22,    -2,   -30,   -29,    5,    38,    33,   -10,
                                            -27,    30,    66,    29,  -57,  -104,   -48,    70,
                                             30,  -130,  -156,    70,  397,   448,   -61,  -946,
                                           -763,  1561,  4908,  7892, 9087,  7892,  4908,  1561,
                                          -1475,  -946,   -61,   448,  397,    70,  -156,  -130,
                                            123,    70,   -48,  -104,  -57,    29,    66,    30,
                                            -44,   -10,    33,    38,    5,   -29,   -30,    -2,
                                             20,    -4,   -21,   -14,    8,    20,    12,    -7};

const DATA InfoTest_Tx[20] = {1,1,1,1,1,0,1,0,1,0,0,0,0,1,1,1,0,0,0,0};
const DATA dataBit_test_Tx[432] = {1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0};

/************************ Variable Definitions ************************/
struct FixSNew * sFixSTx;
DATA   dDataBit_Tx[432];

/***********************************************************************
 * Turbo IP init/cleanup — gọi từ FixS_TxInit() và cleanup
 ***********************************************************************/
static int turbo_ip_init(void)
{
    devmem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (devmem_fd < 0) {
        perror("open /dev/mem (cần chạy với sudo)");
        return -1;
    }

    cfig_reg = (volatile uint64_t *)mmap(NULL, TURBO_MAP_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem_fd, TURBO_CFIG_PHYS_ADDR);
    if (cfig_reg == MAP_FAILED) {
        perror("mmap cfig_reg");
        return -1;
    }

    input_reg = (volatile uint64_t *)mmap(NULL, TURBO_MAP_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem_fd, TURBO_INPUT_PHYS_ADDR);
    if (input_reg == MAP_FAILED) {
        perror("mmap input_reg");
        return -1;
    }

    output_reg = (volatile uint64_t *)mmap(NULL, TURBO_MAP_SIZE,
        PROT_READ | PROT_WRITE, MAP_SHARED, devmem_fd, TURBO_OUTPUT_PHYS_ADDR);
    if (output_reg == MAP_FAILED) {
        perror("mmap output_reg");
        return -1;
    }

    printf("[OK] Turbo Encode IP mapped via /dev/mem\n");
    return 0;
}

static void turbo_ip_cleanup(void)
{
    if (cfig_reg && cfig_reg != MAP_FAILED)
        munmap((void *)cfig_reg, TURBO_MAP_SIZE);
    if (input_reg && input_reg != MAP_FAILED)
        munmap((void *)input_reg, TURBO_MAP_SIZE);
    if (output_reg && output_reg != MAP_FAILED)
        munmap((void *)output_reg, TURBO_MAP_SIZE);
    if (devmem_fd >= 0)
        close(devmem_fd);
}

/***********************************************************************
 * FixS_Interleave — giữ nguyên
 ***********************************************************************/
void FixS_Interleave(DATA Input[], DATA Output[], DATA Nrow, DATA Ncol)
{
    DATA i,j;
    for(i=0;i<Nrow;i++)
        for(j=0;j<Ncol;j++)
        {
            Output[i*Ncol+j] = Input[j*Nrow+i];
        }
}

/***********************************************************************
 * FixS_TxInit — thay đổi: nhận shm pointer, init turbo IP, bỏ DDR addr
 *
 * Baremetal:
 *   shared_buffer_addr[i] = SHARED_DDR_BASEADDR + ...
 *
 * Linux:
 *   Nhận shm pointer từ tx_fixs_linux.c
 *   Mmap Turbo IP registers
 ***********************************************************************/
int FixS_TxInit(struct shared_region *shm)
{
    DATA i;
    int ret;

    /* Lưu con trỏ shared memory */
    shm_ptr = shm;

    /* Khởi tạo Turbo Encode IP qua /dev/mem */
    ret = turbo_ip_init();
    if (ret < 0) {
        fprintf(stderr, "turbo_ip_init() failed\n");
        return -1;
    }

    sFixSTx = (struct FixSNew*)malloc(1*sizeof(struct FixSNew));
    if (!sFixSTx) {
        fprintf(stderr, "malloc FixSNew failed\n");
        return -1;
    }

    // Hop Count Init
    sFixSTx->dHopCnt        = -12;
    sFixSTx->dCountData     = 0;
    sFixSTx->dCountBlockCTC = 0;

    // Reset buffers for filters
    for (i=0; i<NHRC_UP4_FIXS+2; i++)
    {
        sFixSTx->dBufferRRCVecI1[i] = 0;
        sFixSTx->dBufferRRCVecQ1[i] = 0;
    }

    // Reset Output
    for (i=0;i<HOPSIZE_FIXS*4;i++)
    {
        sFixSTx->dCorrI[i] = 0;
        sFixSTx->dCorrQ[i] = 0;
    }

    /* Baremetal: tính shared_buffer_addr[] → BỎ, dùng shm_ptr thay thế */

    printf("[OK] FixS_Tx initialized (Linux)\n");
    return 0;
}

/***********************************************************************
 * FixS_TxCleanup — mới, gọi khi thoát chương trình
 ***********************************************************************/
void FixS_TxCleanup(void)
{
    turbo_ip_cleanup();
    if (sFixSTx) {
        free(sFixSTx);
        sFixSTx = NULL;
    }
    printf("[OK] FixS_Tx cleanup done\n");
}

/***********************************************************************
 * FixS_Modulator — giữ nguyên
 ***********************************************************************/
void FixS_Modulator(DATA dInputBit[], DATA dOutput[], DATA NumBit)
{
    DATA i = 0;
    int16x8_t v_pos = vdupq_n_s16(8192);
    int16x8_t v_neg = vdupq_n_s16(-8192);
    for (i = 0; i < NumBit; i += 8) {
        int16x8_t v_input = vld1q_s16(&dInputBit[i]);
        uint16x8_t mask = vceqq_s16(v_input, vdupq_n_s16(1));
        int16x8_t v_result = vbslq_s16(mask, v_pos, v_neg);
        vst1q_s16(&dOutput[i], v_result);
    }
}

/***********************************************************************
 * FixS_TxSendDummy — giữ nguyên
 ***********************************************************************/
void FixS_TxSendDummy()
{
    DATA i,j;
    j = sFixSTx->dHopCnt+12;
    for(i=0;i<HOPSIZE_FIXS;i+=8)
    {
        int16x8_t temp = vld1q_s16(&DummyPattern[i+j*HOPSIZE_FIXS]);
        vst1q_s16(&sFixSTx->dVecI[i], temp);
    }
}

/***********************************************************************
 * FixS_TxSendHeader — giữ nguyên
 ***********************************************************************/
void FixS_TxSendHeader()
{
    DATA i,j;
    if(sFixSTx->dHopCnt < 84)
    {
        j = sFixSTx->dHopCnt%HOP_CORR_PRE;
        for(i=0;i<HOPSIZE_FIXS;i+=8)
        {
            int16x8_t temp = vld1q_s16(&dPreamblePattern_Tx[i+j*HOPSIZE_FIXS]);
            vst1q_s16(&sFixSTx->dVecI[i], temp);
        }
    }
    else
    {
        j = (sFixSTx->dHopCnt-84)%HOP_CORR_FRE;
        for(i=0;i<HOPSIZE_FIXS;i+=8)
        {
            int16x8_t temp = vld1q_s16(&dFrePattern_Tx[i+j*HOPSIZE_FIXS]);
            vst1q_s16(&sFixSTx->dVecI[i], temp);
        }
    }
}

/***********************************************************************
 * unpack_60bits_from_msb — giữ nguyên
 ***********************************************************************/
void unpack_60bits_from_msb(uint64_t packed, DATA *output, DATA bits_to_extract) {
    DATA i;
    for (i = 0; i < bits_to_extract; ++i) {
        output[i] = (packed >> (63 - i)) & 0x1;
    }
}

/***********************************************************************
 * FixS_TxSendInfo — thay đổi: Turbo IP dùng volatile pointer
 *
 * Baremetal: Xil_Out64(CFIG_BUFFER_ADDR, 0x36)
 * Linux:    *cfig_reg = 0x36
 ***********************************************************************/
void FixS_TxSendInfo()
{
    DATA i,j;

    if(sFixSTx->dHopCnt == 94)
    {
        j = -1;
        for(i=0;i<400;i+=4)
        {
            j = i / 20;
            int16x4_t info_vec = vdup_n_s16(InfoTest_Tx[j]);
            vst1_s16(&dDataBit_Tx[i], info_vec);
        }

        int16x8_t one_vec = vdupq_n_s16(1);
        for(i=400;i<432;i+=8)
        {
            int16x8_t idx_vec = {i, i+1, i+2, i+3, i+4, i+5, i+6, i+7};
            int16x8_t result = vandq_s16(idx_vec, one_vec);
            vst1q_s16(&dDataBit_Tx[i], result);
        }

        /* Turbo Encode — Linux: dùng volatile pointer thay Xil_Out64/In64 */

        /* Step 1: Send configuration */
        *cfig_reg = 0x36;

        /* Step 2: Write input data */
        DATA full_data[64*7] = {0};
        for(i = 0; i < 432; i+=8)
        {
            int16x8_t temp_vec = vld1q_s16(&dDataBit_Tx[i]);
            vst1q_s16(&full_data[i], temp_vec);
        }

        DATA block, idx;
        for (block = 0; block < 7; block++) {
            uint64_t packed = 0;

            for (int i = 0; i < 64; i++) {
                idx = block * 64 + i;
                if (full_data[idx] == 1) {
                    packed |= ((uint64_t)(1) << (63-i));
                }
            }
            *input_reg = packed;    /* Baremetal: Xil_Out64(INPUT_BUFFER_ADDR, packed) */
        }

        /* Step 3: Read processed data */
        DATA bit_index = 0;
        block = 0;

        while (bit_index < 1296)
        {
            uint64_t packed = *output_reg;  /* Baremetal: Xil_In64(OUTPUT_BUFFER_ADDR) */
            uint8_t stop_bit = packed & 0x1;

            DATA bits_remaining = 1296 - bit_index;
            DATA bits_to_extract = (bits_remaining >= 60) ? 60 : bits_remaining;

            unpack_60bits_from_msb(packed, &sFixSTx->dInterleave[bit_index], bits_to_extract);
            bit_index += bits_to_extract;

            if (stop_bit == 1) {
                break;
            }
        }

        // Modulator
        FixS_Modulator(sFixSTx->dInterleave,sFixSTx->dDataCTCInRecvI,NUM_CTC_OUT);

        // 1st MP
        for(i=0;i<HOPSIZE_FIXS;i+=8)
        {
            int16x8_t temp = vld1q_s16(&dProbePattern_Tx[i]);
            vst1q_s16(&sFixSTx->dVecI[i], temp);
        }
    }
    else
    {
        if((sFixSTx->dHopCnt&0x0001) == 1)
        {
            i = 0;
            int16x4_t temp_prefix = vld1_s16(&PrefixPattern_Tx[i]);
            vst1_s16(&sFixSTx->dVecI[i], temp_prefix);

            for(i=4;i<36;i++)
            {
                sFixSTx->dVecI[i] = sFixSTx->dDataCTCInRecvI[sFixSTx->dCountData];
                if(sFixSTx->dCountData < 1295)
                {
                    sFixSTx->dCountData++;
                }
                else
                {
                    sFixSTx->dCountData = 1295;
                }
            }

            i = 36;
            int16x4_t temp_guard = vld1_s16(&GuardPattern_Tx[i-36]);
            vst1_s16(&sFixSTx->dVecI[i], temp_guard);
        }
        else
        {
            for(i=0;i<HOPSIZE_FIXS;i+=8)
            {
                int16x8_t temp = vld1q_s16(&dProbePattern_Tx[i]);
                vst1q_s16(&sFixSTx->dVecI[i], temp);
            }
        }
    }
}

/***********************************************************************
 * FixS_TxSendData — thay đổi: Turbo IP + shared memory
 *
 * Baremetal:
 *   vld1q_s16((DATA*)(shared_buffer_addr[READ_IDX] + i * sizeof(DATA)))
 *   READ_IDX = READ_IDX + 8;
 *
 * Linux:
 *   memcpy từ shm_ptr->data[shm_ptr->read_idx]
 *   shm_ptr->read_idx = (shm_ptr->read_idx + 1) % BUFFER_DEPTH
 ***********************************************************************/
void FixS_TxSendData()
{
    DATA i;

    if(((sFixSTx->dHopCnt - 179) % 80 == 0) && (sFixSTx->dCountBlockCTC < 20))
    {
        /* ============================================================
         * Đọc MELP data từ shared memory
         *
         * Baremetal:
         *   vld1q_s16((DATA*)(shared_buffer_addr[READ_IDX] + i * sizeof(DATA)));
         *   READ_IDX = READ_IDX + 8;
         *
         * Linux:
         *   Đọc từ shm_ptr->data[read_idx], tăng read_idx
         *   Baremetal đọc 8 slot mỗi lần (READ_IDX += 8)
         *   nhưng mỗi slot = MELP_DATA_SIZE = 54 uint16_t
         *   Cần đọc đủ NUM_CTC_IN (432) bit = 8 slot x 54
         * ============================================================ */
        int slot;
        int bit_offset = 0;
        for (slot = 0; slot < 8; slot++) {
            /* Chờ nếu ring buffer rỗng */
            while (shm_ptr->read_idx == shm_ptr->write_idx) {
                usleep(100);
            }

            /* Copy 54 uint16_t từ shared memory vào dDataBit_Tx */
            for (i = 0; i < MELP_DATA_SIZE && bit_offset < NUM_CTC_IN; i++) {
                dDataBit_Tx[bit_offset] = (DATA)shm_ptr->data[shm_ptr->read_idx][i];
                bit_offset++;
            }

            /* Tăng read_idx */
            __sync_synchronize();
            shm_ptr->read_idx = (shm_ptr->read_idx + 1) % BUFFER_DEPTH;
        }

        /* ============================================================
         * Turbo Encode — Linux: volatile pointer thay Xil_Out64/In64
         * ============================================================ */

        /* Step 1: Send configuration */
        *cfig_reg = 0x36;

        /* Step 2: Write input data */
        DATA enc_data[64*7] = {0};
        for(i = 0; i < 432; i+=8)
        {
            int16x8_t temp_vec2 = vld1q_s16(&dDataBit_Tx[i]);
            vst1q_s16(&enc_data[i], temp_vec2);
        }

        DATA step, idx;
        for (step = 0; step < 7; step++) {
            uint64_t packed = 0;

            for (i = 0; i < 64; i++) {
                idx = step * 64 + i;
                if (enc_data[idx] == 1) {
                    packed |= ((uint64_t)(1) << (63-i));
                }
            }
            *input_reg = packed;    /* Baremetal: Xil_Out64(INPUT_BUFFER_ADDR, packed) */
        }

        /* Step 3: Read processed data */
        DATA bit_index = 0;

        while (bit_index < 1296) {
            uint64_t packed = *output_reg;  /* Baremetal: Xil_In64(OUTPUT_BUFFER_ADDR) */

            uint8_t stop_bit = packed & 0x1;

            DATA bits_remaining = 1296 - bit_index;
            DATA bits_to_extract = (bits_remaining >= 60) ? 60 : bits_remaining;

            unpack_60bits_from_msb(packed, &sFixSTx->dDataCTCInRecvI[bit_index], bits_to_extract);
            bit_index += bits_to_extract;

            if (stop_bit == 1) {
                break;
            }
        }

        sFixSTx->dCountBlockCTC++;

        // Interleave
        FixS_Interleave(sFixSTx->dDataCTCInRecvI, sFixSTx->dInterleave, NUMROW_FIXS, NUMCOL_FIXS);

        // Modulator
        FixS_Modulator(sFixSTx->dInterleave, sFixSTx->dDataCTCInRecvI, NUM_CTC_OUT);

        // Saving to Data Buffer
        for(i = 0; i < NUM_CTC_OUT; i+=8)
        {
            int16x8_t temp = vld1q_s16(&sFixSTx->dDataCTCInRecvI[i]);
            vst1q_s16(&sFixSTx->dBufferCorrVecI[sFixSTx->dCountData], temp);
            sFixSTx->dCountData += 8;
        }

        // MP
        for(i=0; i<HOPSIZE_FIXS; i+=8)
        {
            int16x8_t temp = vld1q_s16(&dProbePattern_Tx[i]);
            vst1q_s16(&sFixSTx->dVecI[i], temp);
        }
    }
    else
    {
        if((sFixSTx->dHopCnt & 0x0001) == 0)
        {
            for(i=0; i<PRESIZE_FIXS; i+=4)
            {
                int16x4_t temp = vld1_s16(&PrefixPattern_Tx[i]);
                vst1_s16(&sFixSTx->dVecI[i], temp);
            }

            for(i=4; i<36; i+=8)
            {
                int16x8_t temp = vld1q_s16(&sFixSTx->dBufferCorrVecI[i-4]);
                vst1q_s16(&sFixSTx->dVecI[i], temp);
            }

            for(i = 0; i<sFixSTx->dCountData-32; i++)
            {
                sFixSTx->dBufferCorrVecI[i] = sFixSTx->dBufferCorrVecI[i+32];
            }
            sFixSTx->dCountData = sFixSTx->dCountData - 32;

            for(i=36; i<HOPSIZE_FIXS; i+=4)
            {
                int16x4_t temp = vld1_s16(&GuardPattern_Tx[i-36]);
                vst1_s16(&sFixSTx->dVecI[i], temp);
            }
        }
        else
        {
            for(i=0; i<HOPSIZE_FIXS; i+=8)
            {
                int16x8_t temp = vld1q_s16(&dProbePattern_Tx[i]);
                vst1q_s16(&sFixSTx->dVecI[i], temp);
            }
        }
    }
}

/***********************************************************************
 * FixS_Tx — thay đổi: output format cho IIO
 *
 * Baremetal: ghi uint32_t tx_data[] (packed I/Q 16+16, nhân đôi)
 * Linux:    ghi int16_t tx_data[] (interleaved I, Q, I, Q, ...)
 *           → tx_fixs_linux.c push qua IIO buffer
 ***********************************************************************/
void FixS_Tx(int16_t* tx_data)
{
    DATA    i;
    DATA    dITempVec4[160];
    DATA    dQTempVec4[160];

    // Tx process
    if(sFixSTx->dHopCnt == 179)
    {
        sFixSTx->dCountData = 0;
    }

    // Send Dummy
    if(sFixSTx->dHopCnt < 0)
    {
        FixS_TxSendDummy();
    }
    // Send Preamble + Frequency Sync
    else if(sFixSTx->dHopCnt < 94)
    {
        FixS_TxSendHeader();
    }
    // Send Info
    else if(sFixSTx->dHopCnt < 179)
    {
        FixS_TxSendInfo();
    }
    // Send Data
    else
    {
        FixS_TxSendData();
    }

    // Fir Up 4
    for (i=0; i<HOPSIZE_FIXS*4; i++)
    {
        sFixSTx->dCorrI[i] = 0;
        sFixSTx->dCorrQ[i] = 0;
    }

    for (i=0; i<HOPSIZE_FIXS; i++)
    {
        sFixSTx->dCorrI[i*4] = sFixSTx->dVecI[i];
    }

    fir(sFixSTx->dCorrI, (DATA*)RRC_Up4_FIXS_Tx, dITempVec4, sFixSTx->dBufferRRCVecI1, 160, NHRC_UP4_FIXS);
    fir(sFixSTx->dCorrQ, (DATA*)RRC_Up4_FIXS_Tx, dQTempVec4, sFixSTx->dBufferRRCVecQ1, 160, NHRC_UP4_FIXS);

    for (i=0; i<HOPSIZE_FIXS; i++)
    {
        sFixSTx->dCorrI[i] = 0;
        sFixSTx->dCorrQ[i] = 0;
    }

    /* ============================================================
     * Output format — thay đổi cho IIO
     *
     * Baremetal (uint32_t, packed, nhân đôi cho 2 kênh DAC):
     *   tx_data[2*i]   = (Q<<16) | I
     *   tx_data[2*i+1] = (Q<<16) | I
     *
     * Linux (int16_t, interleaved I/Q cho IIO buffer):
     *   tx_data[2*i]   = I
     *   tx_data[2*i+1] = Q
     * ============================================================ */
    for(i = 0; i < 160; i++)
    {
        tx_data[2*i]     = dITempVec4[i];  /* I */
        tx_data[2*i + 1] = dQTempVec4[i];  /* Q */
    }

    /***********************************************************************/
    sFixSTx->dHopCnt++;

    // Reset to transmit next frame
    if(sFixSTx->dHopCnt >= HOP_PER_FRAME_FIXS)
    {
        sFixSTx->dHopCnt = 0;
        sFixSTx->dCountData = 0;
        sFixSTx->dCountBlockCTC = 0;
    }
}