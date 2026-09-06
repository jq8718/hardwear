#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "i2c_io.h"

#define I2C_SDA_GPIO     2
#define I2C_SCL_GPIO     3
#define PCA9535_INT_GPIO 28
#define PCA9535_ADDR     0x20
#define I2C_TIMEOUT_MS   100

static const char *TAG = "i2c_io";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool s_present = false;

esp_err_t i2c_io_init(void)
{
    // INT pin: input + internal pull-up so it never floats while PCA9535 is absent.
    gpio_set_direction(PCA9535_INT_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PCA9535_INT_GPIO, GPIO_PULLUP_ONLY);

    // SDA/SCL internal pull-up: PCA9535 is not soldered yet, so the bus floats
    // without these; keep the lines high so the probe sees a clean NACK.
    gpio_set_pull_mode(I2C_SDA_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(I2C_SCL_GPIO, GPIO_PULLUP_ONLY);

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_io_num = I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_probe(s_bus, PCA9535_ADDR, I2C_TIMEOUT_MS);
    if (err == ESP_OK) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = PCA9535_ADDR,
            .scl_speed_hz = 100000,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

        // Both ports as inputs: command 0x06 (Config Port0) then 0xFF 0xFF.
        uint8_t dir_cmd[3] = {0x06, 0xFF, 0xFF};
        ESP_ERROR_CHECK(i2c_master_transmit(s_dev, dir_cmd, sizeof(dir_cmd), I2C_TIMEOUT_MS));

        s_present = true;
        ESP_LOGI(TAG, "PCA9535 present at 0x20, ports configured as inputs");
    } else if (err == ESP_ERR_NOT_FOUND) {
        s_present = false;
        ESP_LOGW(TAG, "PCA9535 not found at 0x20, keys/SW disabled");
    } else {
        s_present = false;
        ESP_LOGW(TAG, "PCA9535 probe error: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

bool pca9535_present(void)
{
    return s_present;
}

esp_err_t pca9535_read_ports(uint8_t *port0, uint8_t *port1)
{
    if (!s_present || s_dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t reg = 0x00;  // Input Port0 register
    uint8_t buf[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    *port0 = buf[0];
    *port1 = buf[1];
    return ESP_OK;
}
