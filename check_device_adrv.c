#include <iio.h>
#include <stdio.h>

int main(void) {
    // 1. Tạo context (Local, Network, hoặc Auto)
    struct iio_context *ctx = iio_create_default_context();
    if (!ctx) {
        fprintf(stderr, "Unable to create IIO context\n");
        return -1;
    }

    // 2. Lấy số lượng thiết bị trong context
    unsigned int nb_devices = iio_context_get_devices_count(ctx);
    printf("Context has %u devices:\n", nb_devices);

    for (unsigned int i = 0; i < nb_devices; i++) {
        // Lấy con trỏ đến thiết bị
        struct iio_device *dev = iio_context_get_device(ctx, i);
        const char *dev_id = iio_device_get_id(dev);
        const char *dev_name = iio_device_get_name(dev);
        
        printf("Device %u: %s (Name: %s)\n", i, dev_id, dev_name ? dev_name : "N/A");

        // --- MỚI: Liệt kê các Device Attributes (Thuộc tính thiết bị) ---
        unsigned int nb_dev_attrs = iio_device_get_attrs_count(dev);
        for (unsigned int k = 0; k < nb_dev_attrs; k++) {
            const char *attr_name = iio_device_get_attr(dev, k);
            char val[1]; // Buffer để chứa giá trị đọc về
            
            // Đọc giá trị thuộc tính
            ssize_t res = iio_device_attr_read(dev, attr_name, val, sizeof(val));
            if (res > 0) {
                printf("\t[Dev Attr] %s: %s\n", attr_name, val);
            } else {
                printf("\t[Dev Attr] %s: <error reading>\n", attr_name);
            }
        }

        // 3. Lấy số lượng kênh của thiết bị đó
        unsigned int nb_channels = iio_device_get_channels_count(dev);
        
        for (unsigned int j = 0; j < nb_channels; j++) {
            struct iio_channel *chn = iio_device_get_channel(dev, j);
            const char *chn_id = iio_channel_get_id(chn);
            bool output = iio_channel_is_output(chn);
            
            printf("\tChannel: %s (%s)\n", chn_id, output ? "Output" : "Input");

            // --- MỚI: Liệt kê các Channel Attributes (Thuộc tính kênh) ---
            unsigned int nb_chn_attrs = iio_channel_get_attrs_count(chn);
            for (unsigned int k = 0; k < nb_chn_attrs; k++) {
                const char *attr_name = iio_channel_get_attr(chn, k);
                char val[1];
                
                // Đọc giá trị thuộc tính kênh
                ssize_t res = iio_channel_attr_read(chn, attr_name, val, sizeof(val));
                if (res > 0) {
                    printf("\t\t[Chn Attr] %s: %s\n", attr_name, val);
                }
            }
        }
    }

    // 4. Dọn dẹp bộ nhớ
    iio_context_destroy(ctx);
    return 0;
}