/*****************************************************************************//**
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 ********************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xil_cache.h"
#include "xil_io.h"

#include "no_os_error.h"
#include "no_os_util.h"
#include "no_os_spi.h"

#include "axi_adc_core.h"
#include "axi_dac_core.h"
#include "axi_dmac.h"

#include "parameters.h"

#include "adrv9002.h"
#include "adi_adrv9001.h"
#include "adi_adrv9001_arm.h"
#include "adi_adrv9001_radio.h"
#include "adi_adrv9001_auxdac.h"
#include "adi_adrv9001_tx.h"
#include "adi_adrv9001_rx.h"
#include "adi_adrv9001_profileutil.h"
#include "Navassa_LVDS_profile.h"

#include "Vhf_FixS_Tx.h"

#define TX_DATA_SIZE     160

#define WRITE_IDX  (*(volatile unsigned int *)(0xFFFC0000 + 0 * sizeof(unsigned int)))
#define READ_IDX   (*(volatile unsigned int *)(0xFFFC0000 + 1 * sizeof(unsigned int)))
#define START_FLAG (*(volatile unsigned int *)(0xFFFC0000 + 2 * sizeof(unsigned int)))
#define TX_READY   (*(volatile unsigned int *)(0xFFFC0000 + 3 * sizeof(unsigned int)))
#define MIC_READY  (*(volatile unsigned int *)(0xFFFC0000 + 4 * sizeof(unsigned int)))

#define DAC_DDR_BASEADDR        XPAR_DDR_MEM_BASEADDR + 0x40000000
#define MIC_DDR_BASEADDR        XPAR_DDR_MEM_BASEADDR + 0x50000000
#define SHARED_DDR_BASEADDR     XPAR_DDR_MEM_BASEADDR + 0x60000000

uintptr_t tx_buffer_addr[2] = {
    DAC_DDR_BASEADDR,
    DAC_DDR_BASEADDR + 2 * TX_DATA_SIZE * sizeof(uint32_t)
};

uint64_t sampling_freq;

int get_sampling_frequency(struct axi_adc *dev, uint32_t chan,
                           uint64_t *sampling_freq_hz)
{
    if (!dev || !sampling_freq_hz)
        return -EINVAL;

    *sampling_freq_hz = sampling_freq;
    return 0;
}

static struct adi_adrv9001_SpiSettings spiSettings = {
    .msbFirst = 1,
    .enSpiStreaming = 0,
    .autoIncAddrUp = 1,
    .fourWireMode = 1,
    .cmosPadDrvStrength = ADI_ADRV9001_CMOSPAD_DRV_STRONG,
};

struct adi_adrv9001_SpiSettings *adrv9002_spi_settings_get(void)
{
    return &spiSettings;
}

enum adi_adrv9001_SsiType adrv9002_ssi_type_detect(struct adrv9002_rf_phy *phy)
{
    enum adi_adrv9001_SsiType ssi, ssi2;
    char *ssi_str[3] = {
        "[SSI Disabled]",
        "CMOS",
        "LVDS"
    };

    ssi = adrv9002_axi_ssi_type_get(phy);

    ssi2 = phy->curr_profile->rx.rxChannelCfg[0].profile.rxSsiConfig.ssiType;

    if (ssi != ssi2) {
        printf("SSI mismatch: detected %s in HDL and %s in profile.\n", ssi_str[ssi],
               ssi_str[ssi2]);
        return ADI_ADRV9001_SSI_TYPE_DISABLE;
    }

    return ssi;
}

static struct adi_adrv9001_GainControlCfg agc_defaults = {
    .peakWaitTime = 4,
    .maxGainIndex = ADI_ADRV9001_RX_GAIN_INDEX_MAX,
    .minGainIndex = ADI_ADRV9001_RX_GAIN_INDEX_MIN,
    .gainUpdateCounter = 11520,
    .attackDelay_us = 10,
    .lowThreshPreventGainInc = false,
    .slowLoopSettlingDelay = 16,
    .changeGainIfThreshHigh = 3,
    .agcMode = 1,
    .resetOnRxon = false,
    .resetOnRxonGainIndex = ADI_ADRV9001_RX_GAIN_INDEX_MAX,
    .enableSyncPulseForGainCounter = false,
    .enableFastRecoveryLoop = false,
    .power = {
        .powerEnableMeasurement = true,
        .underRangeHighPowerThresh = 10,
        .underRangeLowPowerThresh = 4,
        .underRangeHighPowerGainStepRecovery = 2,
        .underRangeLowPowerGainStepRecovery = 4,
        .powerMeasurementDuration = 10,
        .powerMeasurementDelay = 2,
        .rxTddPowerMeasDuration = 0,
        .rxTddPowerMeasDelay = 0,
        .overRangeHighPowerThresh = 0,
        .overRangeLowPowerThresh = 7,
        .overRangeHighPowerGainStepAttack = 4,
        .overRangeLowPowerGainStepAttack = 4,
        .feedback_inner_high_inner_low = ADI_ADRV9001_GPIO_PIN_CRUMB_UNASSIGNED,
        .feedback_apd_high_apd_low = ADI_ADRV9001_GPIO_PIN_CRUMB_UNASSIGNED,
    },
    .peak = {
        .agcUnderRangeLowInterval = 50,
        .agcUnderRangeMidInterval = 2,
        .agcUnderRangeHighInterval = 4,
        .apdHighThresh = 21,
        .apdLowThresh = 12,
        .apdUpperThreshPeakExceededCount = 6,
        .apdLowerThreshPeakExceededCount = 3,
        .apdGainStepAttack = 2,
        .apdGainStepRecovery = 0,
        .enableHbOverload = true,
        .hbOverloadDurationCount = 1,
        .hbOverloadThreshCount = 1,
        .hbHighThresh = 13044,
        .hbUnderRangeLowThresh = 5826,
        .hbUnderRangeMidThresh = 8230,
        .hbUnderRangeHighThresh = 7335,
        .hbUpperThreshPeakExceededCount = 6,
        .hbUnderRangeHighThreshExceededCount = 3,
        .hbGainStepHighRecovery = 2,
        .hbGainStepLowRecovery = 6,
        .hbGainStepMidRecovery = 4,
        .hbGainStepAttack = 2,
        .hbOverloadPowerMode = 0,
        .hbUnderRangeMidThreshExceededCount = 3,
        .hbUnderRangeLowThreshExceededCount = 3,
        .feedback_apd_low_hb_low = ADI_ADRV9001_GPIO_PIN_CRUMB_UNASSIGNED,
        .feedback_apd_high_hb_high = ADI_ADRV9001_GPIO_PIN_CRUMB_UNASSIGNED,
    },
};

int main(void)
{
    int ret;
    struct adi_common_ApiVersion api_version;
    struct adi_adrv9001_ArmVersion arm_version;
    struct adi_adrv9001_SiliconVersion silicon_version;
    struct adi_adrv9001_Device adrv9001_device = {0};
    struct adrv9002_chip_info chip = {0};
    struct adrv9002_rf_phy phy = {0};

    unsigned int c;
    uint32_t read_value;
    int ping_idx = 0;
    int pong_idx = 1;

    WRITE_IDX = 0;
    READ_IDX = 0;
    START_FLAG = 0;
    TX_READY = 0;
    MIC_READY = 0;

    // Init hardware
    printf("Core0: Hello\n");

    struct axi_adc_init rx1_adc_init = {
        .name = "axi-adrv9002-rx-lpc",
        .base = RX1_ADC_BASEADDR,
        .num_channels = ADRV9001_I_Q_CHANNELS,
    };

    struct axi_dac_channel tx1_dac_channels[2];
    tx1_dac_channels[0].sel = AXI_DAC_DATA_SEL_DMA;
    tx1_dac_channels[1].sel = AXI_DAC_DATA_SEL_DMA;

    struct axi_dac_init tx1_dac_init = {
        .name = "axi-adrv9002-tx-lpc",
        .base = TX1_DAC_BASEADDR,
        .num_channels = ADRV9001_I_Q_CHANNELS,
        .channels = tx1_dac_channels,
        .rate = 3
    };

    struct axi_dmac_init rx1_dmac_init = {
        "rx_dmac",
        RX1_DMA_BASEADDR,
        IRQ_DISABLED
    };

    struct axi_dmac_init tx1_dmac_init = {
        "tx_dmac",
        TX1_DMA_BASEADDR,
        IRQ_DISABLED
    };

    Xil_ICacheEnable();
    Xil_DCacheEnable();

    phy.rx2tx2 = true;
    phy.adrv9001 = &adrv9001_device;

    /* ADRV9002 */
    chip.cmos_profile = "Navassa_CMOS_profile.json";
    chip.lvd_profile = "Navassa_LVDS_profile.json";
    chip.name = "adrv9002-phy";
    chip.n_tx = ADRV9002_CHANN_MAX;

    phy.chip = &chip;

    ret = adi_adrv9001_profileutil_Parse(phy.adrv9001, &phy.profile,
                                         (char *)json_profile, strlen(json_profile));
    if (ret)
        goto error;

    phy.curr_profile = &phy.profile;

    sampling_freq = phy.curr_profile->rx.rxChannelCfg[0].profile.rxOutputRate_Hz;

    /* Initialize the ADC/DAC cores */
    ret = axi_adc_init_begin(&phy.rx1_adc, &rx1_adc_init);
    if (ret) {
        printf("axi_adc_init_begin() failed with status %d\n", ret);
        goto error;
    }

    ret = axi_dac_init_begin(&phy.tx1_dac, &tx1_dac_init);
    if (ret) {
        printf("axi_dac_init_begin() failed with status %d\n", ret);
        goto error;
    }

    phy.ssi_type = adrv9002_ssi_type_detect(&phy);
    if (phy.ssi_type == ADI_ADRV9001_SSI_TYPE_DISABLE)
        goto error;

    /* Initialize AGC */
    for (c = 0; c < ADRV9002_CHANN_MAX; c++) {
        phy.rx_channels[c].agc = agc_defaults;
    }

    ret = adrv9002_setup(&phy);
    if (ret)
        return ret;

    adi_adrv9001_ApiVersion_Get(phy.adrv9001, &api_version);
    adi_adrv9001_arm_Version(phy.adrv9001, &arm_version);
    adi_adrv9001_SiliconVersion_Get(phy.adrv9001, &silicon_version);

    printf("%s Rev %d.%d, Firmware %u.%u.%u.%u API version: %u.%u.%u successfully initialized\n",
           "ADRV9002", silicon_version.major, silicon_version.minor,
           arm_version.majorVer, arm_version.minorVer,
           arm_version.maintVer, arm_version.rcVer, api_version.major,
           api_version.minor, api_version.patch);

    /* Post AXI DAC/ADC setup, digital interface tuning */
    ret = adrv9002_post_setup(&phy);
    if (ret) {
        printf("adrv9002_post_setup() failed with status %d\n", ret);
        goto error;
    }

    /* Finalize the ADC/DAC cores initialization */
    ret = axi_adc_init_finish(phy.rx1_adc);
    if (ret) {
        printf("axi_adc_init_finish() failed with status %d\n", ret);
        goto error;
    }

    ret = axi_dac_init_finish(phy.tx1_dac);
    if (ret) {
        printf("axi_dac_init_finish() failed with status %d\n", ret);
        goto error;
    }
    phy.tx1_dac->clock_hz = phy.curr_profile->tx.txProfile[0].txInputRate_Hz;

    /* Initialize the AXI DMA Controller cores */
    ret = axi_dmac_init(&phy.tx1_dmac, &tx1_dmac_init);
    if (ret) {
        printf("axi_dmac_init() failed with status %d\n", ret);
        goto error;
    }

    ret = axi_dmac_init(&phy.rx1_dmac, &rx1_dmac_init);
    if (ret) {
        printf("axi_dmac_init() failed with status %d\n", ret);
        goto error;
    }

    FixS_TxInit();

    // Config ADRV9001
    adi_adrv9001_AuxDac_Configure(phy.adrv9001, ADI_ADRV9001_AUXDAC3, 1);
    adi_adrv9001_AuxDac_Code_Set(phy.adrv9001, ADI_ADRV9001_AUXDAC3, 0);

    adi_adrv9001_Tx_Attenuation_Set(phy.adrv9001, ADI_CHANNEL_1, 10000); // loopback
    adi_adrv9001_Tx_Attenuation_Set(phy.adrv9001, ADI_CHANNEL_2, 0);     // twoboard

    // Setup DMA
    axi_dac_set_dataset(phy.tx1_dac, -1, AXI_DAC_DATA_SEL_DMA);

    struct axi_dma_transfer transfer_tx = {
        // Number of bytes to write/read
        .size = 2 * TX_DATA_SIZE * sizeof(uint32_t),
        // Transfer done flag
        .transfer_done = 0,
        // Signal transfer mode
        .cyclic = NO,
        // Address of data source
        .src_addr = 0,
        // Address of data destination
        .dest_addr = 0
    };

    // Config L1
    Xil_Out32(TX_CFG_BASEADDR + 0x00, 0); // GaindB TX
    do {
        read_value = Xil_In32(TX_CFG_BASEADDR + 0x7C);
    } while (read_value != 0);

    Xil_Out32(RX_CFG_BASEADDR + 0x00, 0); // GaindB RX
    do {
        read_value = Xil_In32(RX_CFG_BASEADDR + 0x7C);
    } while (read_value != 0);

    printf("Core0: Config L1 Done\n");

    // Transmit
    TX_READY = 1;
    while(!(TX_READY && MIC_READY)) {};

    while(!START_FLAG) {};
    no_os_mdelay(400);

    FixS_Tx(tx_buffer_addr[ping_idx]);
    Xil_DCacheFlushRange(tx_buffer_addr[ping_idx], 2 * TX_DATA_SIZE * sizeof(uint32_t));

    while(1) {
        // config and start tx dma
        transfer_tx.src_addr = tx_buffer_addr[ping_idx];
        axi_dmac_write(phy.tx1_dmac, AXI_DMAC_REG_IRQ_PENDING, 3); // clear irq
        axi_dmac_transfer_start(phy.tx1_dmac, &transfer_tx);

        FixS_Tx(tx_buffer_addr[pong_idx]);
        Xil_DCacheFlushRange(tx_buffer_addr[pong_idx], 2 * TX_DATA_SIZE * sizeof(uint32_t));

        ping_idx = ping_idx ^ 1;
        pong_idx = pong_idx ^ 1;

        // wait tx complete
        axi_dmac_transfer_wait_completion(phy.tx1_dmac, 50000);
    }
    printf("Core0: Bye\n");

error:
    adi_adrv9001_HwClose(phy.adrv9001);
    axi_adc_remove(phy.rx1_adc);
    axi_dac_remove(phy.tx1_dac);
    axi_adc_remove(phy.rx2_adc);
    axi_dac_remove(phy.tx2_dac);
    axi_dmac_remove(phy.rx1_dmac);
    axi_dmac_remove(phy.tx1_dmac);
    axi_dmac_remove(phy.rx2_dmac);
    axi_dmac_remove(phy.tx2_dmac);
    return ret;
}