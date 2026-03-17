#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <iio.h>

#define SAMPLES_COUNT 512
#define RX_DEVICE "axi-adrv9002-rx-lpc"
#define PHY_DEVICE "adrv9002-phy"

volatile int keep_running = 1;

void signal_handler(int sig) {
    keep_running = 0;
}

int main(){
    // Connect via IIO network backend (localhost)
    struct iio_context *ctx = iio_create_network_context("localhost");
    if (!ctx) {
        fprintf(stderr, "Loi: Khong the ket noi IIO network (iiod dang chay?)\n");
        fprintf(stderr, "Thu: iiod -F\n");
        return -1;
    }

    // Tìm thiết bị nhận (RX)
    struct iio_device *rx_device = iio_context_find_device(ctx, RX_DEVICE);
    if (!rx_device) {
        fprintf(stderr, "Loi: Khong tim thay thiet bi %s.\n", RX_DEVICE);
        iio_context_destroy(ctx);
        return -1;
    }

    // Tìm 4 kênh (voltage0, voltage1, voltage2, voltage3)
    struct iio_channel *ch_i0 = iio_device_find_channel(rx_device, "voltage0_i", false);
    struct iio_channel *ch_q0 = iio_device_find_channel(rx_device, "voltage0_q", false);
    struct iio_channel *ch_i1 = iio_device_find_channel(rx_device, "voltage1_i", false);
    struct iio_channel *ch_q1 = iio_device_find_channel(rx_device, "voltage1_q", false);

    if (!ch_i0 || !ch_q0 || !ch_i1 || !ch_q1) {
        fprintf(stderr, "Loi: Khong tim thay cac kenh voltage.\n");
        iio_context_destroy(ctx);
        return -1;
    }

    // Kích hoạt 4 kênh
    iio_channel_enable(ch_i0);
    iio_channel_enable(ch_q0);
    iio_channel_enable(ch_i1);
    iio_channel_enable(ch_q1);

    // Set ADRV9002 to RX mode (only if not already in a mode)
    struct iio_device *phy_device = iio_context_find_device(ctx, PHY_DEVICE);
    if (phy_device) {
        printf("Canh bao: RX program khong thay doi ENSM mode (cho TX program quan ly)\n");
        printf("Dang tro mot luc khac de TX program set mode...\n");
        usleep(1000000);  // Wait 1 second for TX program to set mode
    }
    
    printf("Dang mo RX buffer (Device phai san sang TX+RX)...\n");

    // Tạo cyclic buffer cho RX
    struct iio_buffer *rx_buf = iio_device_create_buffer(rx_device, SAMPLES_COUNT, true);
    if (!rx_buf) {
        fprintf(stderr, "Loi: Khong the tao IIO RX buffer!\n");
        iio_context_destroy(ctx);
        return -1;
    }

    printf("Bat dau nhan du lieu tu ADRV9002 (RX)...\n");
    printf("Nhan Ctrl+C de dung chuong trinh\n\n");

    // Set up signal handler for graceful exit
    signal(SIGINT, signal_handler);

    // Statistics
    unsigned long samples_received = 0;
    unsigned long buffers_received = 0;
    int print_interval = 100;  // Print stats every 100 buffers

    while (keep_running) {
        // Lấy dữ liệu từ buffer
        ssize_t nbytes = iio_buffer_refill(rx_buf);
        if (nbytes < 0) {
            fprintf(stderr, "Loi: iio_buffer_refill that bai: %s\n", strerror((int)-nbytes));
            break;
        }

        buffers_received++;
        
        // In thống kê
        if (buffers_received % print_interval == 0) {
            printf("Buffer received: %lu (%.2f MB)\n", 
                   buffers_received, 
                   (float)(buffers_received * SAMPLES_COUNT * 4 * 2) / (1024.0 * 1024.0));
        }

        // Duyệt qua dữ liệu trong buffer
        int16_t *ptr = (int16_t *)iio_buffer_start(rx_buf);
        ptrdiff_t step = iio_buffer_step(rx_buf) / sizeof(int16_t);
        int16_t *end = (int16_t *)iio_buffer_end(rx_buf);

        // In một vài mẫu để kiểm chứng (mỗi 10000 buffer)
        if (buffers_received % 10000 == 0 && ptr < end) {
            printf("Sample data (first 4 frames):\n");
            for (int i = 0; i < 4 && ptr < end; i++) {
                printf("  Frame %d: I0=%6d, Q0=%6d, I1=%6d, Q1=%6d\n",
                       i,
                       ptr[0], ptr[1], ptr[2], ptr[3]);
                ptr += step;
            }
            printf("\n");
        }

        samples_received += SAMPLES_COUNT;
        
        // Optional: Add small delay to prevent CPU spinning
        usleep(100);
    }

    printf("\n\nDang dung chuong trinh...\n");
    printf("Tong cong: %lu buffers, %lu samples\n", buffers_received, samples_received);

    // Cleanup
    iio_buffer_destroy(rx_buf);
    iio_context_destroy(ctx);

    printf("Ket thuc.\n");
    return 0;
}