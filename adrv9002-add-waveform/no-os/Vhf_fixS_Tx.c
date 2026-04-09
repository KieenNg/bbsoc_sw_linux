#include "Vhf_FixS_Tx.h"

#define MELP_DATA_SIZE   54
#define BUFFER_DEPTH     128

#define WRITE_IDX    (*(volatile unsigned int *)(0xFFFC0000 + 0 * sizeof(unsigned int)))
#define READ_IDX     (*(volatile unsigned int *)(0xFFFC0000 + 1 * sizeof(unsigned int)))

#define DAC_DDR_BASEADDR      XPAR_DDR_MEM_BASEADDR + 0x40000000
#define MIC_DDR_BASEADDR      XPAR_DDR_MEM_BASEADDR + 0x50000000
#define SHARED_DDR_BASEADDR   XPAR_DDR_MEM_BASEADDR + 0x60000000

uintptr_t shared_buffer_addr[BUFFER_DEPTH];

DATA DataI[160], DataQ[160];
DATA rx_status = 0; // status fail
DATA RxDataOut[432];

// Addresses - define these according to your platform's memory map
#define CFIG_BUFFER_ADDR   0xA0004000
#define INPUT_BUFFER_ADDR  0xA0005000
#define OUTPUT_BUFFER_ADDR 0xA0006000

/************************ Constant Definitions ************************/
const DATA DummyPattern[480] = {8192,8192,-8192,8192,-8192,8192,-8192,-8192,-8192,8192,-8192,8192,8192,-8192,-8192}; // Truncated

const DATA dPreamblePattern_Tx[240] = {-8192,8192,8192,8192,-8192,8192,-8192,8192,-8192,8192,-8192,-8192,8192,8192, // Truncated
                                       8192,-8192,8192,8192,-8192,8192,-8192,8192,-8192,8192,-8192,-8192,8192,8192};

const DATA dFrePattern_Tx[200] = {8192,8192,8192,-8192,8192,8192,-8192,8192,8192,-8192,8192,8192,-8192,8192, // Truncated
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
const DATA dataBit_test_Tx[432] = {1, 1, 0, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 1, 0}; // Truncated

/************************ Variable Definitions ************************/
struct FixSNew * sFixSTx;
DATA   dDataBit_Tx[432];   // 20 blocks CTC

/***********************************************************************
/** @brief  FixS_Interleave
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
/** @brief  FixS_TxInit
 ***********************************************************************/
void FixS_TxInit()
{
    DATA i;
    sFixSTx = (struct FixSNew*)malloc(1*sizeof(struct FixSNew));
    
    // Hop Count Init
    sFixSTx->dHopCnt        = -12; // Send early 12 Hop for Dummy -> That why use -12
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
    
    for(int i = 0; i < BUFFER_DEPTH; i++) {
        shared_buffer_addr[i] = SHARED_DDR_BASEADDR + i * MELP_DATA_SIZE * sizeof(DATA);
    }
}

/***********************************************************************
/** @brief  FixS_Modulator()
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
/** @brief  FixS_TxSendDummy()
 ***********************************************************************/
void FixS_TxSendDummy()
{
    DATA i,j;
    // Transmit Preamble
    j = sFixSTx->dHopCnt+12;
    for(i=0;i<HOPSIZE_FIXS;i+=8)
    {
        int16x8_t temp = vld1q_s16(&DummyPattern[i+j*HOPSIZE_FIXS]); 
        vst1q_s16(&sFixSTx->dVecI[i], temp); 
    }
}

/***********************************************************************
/** @brief  FixS_TxSendHeader()
 ***********************************************************************/
void FixS_TxSendHeader()
{
    DATA i,j;
    // Transmit Preamble
    if(sFixSTx->dHopCnt < 84) 
    {
        j = sFixSTx->dHopCnt%HOP_CORR_PRE;
        for(i=0;i<HOPSIZE_FIXS;i+=8)
        {
            int16x8_t temp = vld1q_s16(&dPreamblePattern_Tx[i+j*HOPSIZE_FIXS]); 
            vst1q_s16(&sFixSTx->dVecI[i], temp); 
        }
    }
    // Transmit Fre Sync
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
/** @brief  unpack_60bits_from_msb
 ***********************************************************************/
void unpack_60bits_from_msb(uint64_t packed, DATA *output, DATA bits_to_extract) {
    DATA i;
    for (i = 0; i < bits_to_extract; ++i) {
        output[i] = (packed >> (63 - i)) & 0x1;
    }
}

/***********************************************************************
/** @brief  FixS_TxSendInfo
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
        
        /* Step 1: Send configuration to IP core */
        Xil_Out64(CFIG_BUFFER_ADDR, 0x36); 
        
        /* Step 2: Write input data to input buffer */
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
            Xil_Out64(INPUT_BUFFER_ADDR, packed);
        }
        
        /* Step 3: Read processed data from output buffer into CPU memory */
        DATA bit_index = 0;
        block = 0;
        
        while (bit_index < 1296)
        {
            uint64_t packed = Xil_In64(OUTPUT_BUFFER_ADDR);
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
        if((sFixSTx->dHopCnt&0x0001) == 1) // Info Data (Odd hop)
        {
            // Prefix
            i = 0;
            int16x4_t temp_prefix = vld1_s16(&PrefixPattern_Tx[i]); 
            vst1_s16(&sFixSTx->dVecI[i], temp_prefix); 
            
            // Info
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
            // Guard
            i = 36;
            int16x4_t temp_guard = vld1_s16(&GuardPattern_Tx[i-36]); 
            vst1_s16(&sFixSTx->dVecI[i], temp_guard); 
        }
        else // Even hop
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
/** @brief  FixS_TxSendData
 ***********************************************************************/
void FixS_TxSendData()
{
    DATA i;

    if(((sFixSTx->dHopCnt - 179) % 80 == 0) && (sFixSTx->dCountBlockCTC < 20))
    {
        for (int i = 0; i < NUM_CTC_IN; i += 8) {
            // int16x8_t temp_vec1 = vld1q_s16(&dataBit_test_Tx[i]);
            int16x8_t temp_vec1 = vld1q_s16((DATA*)(shared_buffer_addr[READ_IDX] + i * sizeof(DATA)));
            vst1q_s16(&dDataBit_Tx[i], temp_vec1);     // Store 8 elements
        }

        READ_IDX = READ_IDX + 8;
        if(READ_IDX == BUFFER_DEPTH) { READ_IDX = 0; }

        // CTC
        // turboEncode((DATA*)dDataBit_Tx,NUM_CTC_IN,(DATA*)sFixSTx->dDataCTCInRecvI,NUM_CTC_IN);

        /***** Connect with TurboEncode IP core *****/

        /* Step 1: Send configuration to IP core */
        Xil_Out64(CFIG_BUFFER_ADDR, 0x36); // Block size (54 bytes = 432 bits)

        /* Step 2: Write input data to input buffer */
        // Padding to take enough 64 elements
        DATA enc_data[64*7] = {0};
        for(i = 0; i < 432; i+=8)
        {
            int16x8_t temp_vec2 = vld1q_s16(&dDataBit_Tx[i]);
            vst1q_s16(&enc_data[i], temp_vec2); // Luu 8 phan tu vao mang
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

            // Send each block
            Xil_Out64(INPUT_BUFFER_ADDR, packed);
        }

        /* Step 3: Read processed data from output buffer into CPU memory */
        DATA bit_index = 0;

        while (bit_index < 1296) {
            uint64_t packed = Xil_In64(OUTPUT_BUFFER_ADDR);

            // Extract stop_bit from bit position 0
            uint8_t stop_bit = packed & 0x1;

            // Determine how many bits to extract (max 60, less if nearing the end)
            DATA bits_remaining = 1296 - bit_index;
            DATA bits_to_extract = (bits_remaining >= 60) ? 60 : bits_remaining;

            // Unpack bits 63:4 and store into the output_data buffer
            unpack_60bits_from_msb(packed, &sFixSTx->dDataCTCInRecvI[bit_index], bits_to_extract);
            bit_index += bits_to_extract;

            // Stop reading if stop bit is set
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
            vst1q_s16(&sFixSTx->dBufferCorrVecI[sFixSTx->dCountData], temp);  // Store 8 elements
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
        if((sFixSTx->dHopCnt & 0x0001) == 0) // Info Data (Even hop)
        {
            // Prefix
            for(i=0; i<PRESIZE_FIXS; i+=4)
            {
                int16x4_t temp = vld1_s16(&PrefixPattern_Tx[i]);
                vst1_s16(&sFixSTx->dVecI[i], temp); // store 4 phan tu
            }

            // Info
            for(i=4; i<36; i+=8)
            {
                int16x8_t temp = vld1q_s16(&sFixSTx->dBufferCorrVecI[i-4]);
                vst1q_s16(&sFixSTx->dVecI[i], temp); // store 8 phan tu
            }

            // Update Buffer
            for(i = 0; i<sFixSTx->dCountData-32; i++)
            {
                sFixSTx->dBufferCorrVecI[i] = sFixSTx->dBufferCorrVecI[i+32];
            }
            sFixSTx->dCountData = sFixSTx->dCountData - 32;

            // Guard
            for(i=36; i<HOPSIZE_FIXS; i+=4)
            {
                int16x4_t temp = vld1_s16(&GuardPattern_Tx[i-36]);
                vst1_s16(&sFixSTx->dVecI[i], temp); // store 4 phan tu
            }

        }
        else // Odd hops
        {
            for(i=0; i<HOPSIZE_FIXS; i+=8)
            {
                int16x8_t temp = vld1q_s16(&dProbePattern_Tx[i]);
                vst1q_s16(&sFixSTx->dVecI[i], temp); // store 8 phan tu
            }
        }
    }
}

/***********************************************************************
/** @brief  FixS_Tx
 ***********************************************************************/
void FixS_Tx(uint32_t* tx_data)
{
    DATA    i;
    DATA    dITempVec4[160];
    DATA    dQTempVec4[160];

    // printf("Core0: hopcnt = %d\n", sFixSTx->dHopCnt);

    // Tx process -----------------------
    if(sFixSTx->dHopCnt == 179) // Reset for Data Segment
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

    fir(sFixSTx->dCorrI, (DATA*)RRC_Up4_FIXS_Tx, dITempVec4, sFixSTx->dBufferRRCVecI1, 160, NHRC_UP4_FIXS); // NHRC_UP4_FIXS
    fir(sFixSTx->dCorrQ, (DATA*)RRC_Up4_FIXS_Tx, dQTempVec4, sFixSTx->dBufferRRCVecQ1, 160, NHRC_UP4_FIXS);

    for (i=0; i<HOPSIZE_FIXS; i++)
    {
        sFixSTx->dCorrI[i] = 0;
        sFixSTx->dCorrQ[i] = 0;
    }

    for(i = 0; i < 160; i++)
    {
        tx_data[2*i]     = ((uint32_t)(uint16_t)(dQTempVec4[i]) << 16) | ((uint16_t)(dITempVec4[i])); // IQ data of ...
        tx_data[2*i + 1] = ((uint32_t)(uint16_t)(dQTempVec4[i]) << 16) | ((uint16_t)(dITempVec4[i])); // IQ data of ...
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