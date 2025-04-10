#include "ds3231.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/unistd.h>

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/msc_host.h"
#include "usb/msc_host_vfs.h"
#include "ffconf.h"
#include "errno.h"
#include "driver/gpio.h"
#include "DS3231.h"
#include "adc_read.h"

static const char *TAG = "DS3231";

/* Global handle for I2C bus and DS3231 device */
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t ds3231_handle;

/* Convert BCD to decimal */
uint8_t bcd_to_dec(uint8_t val)
{
    return ((val / 16) * 10) + (val % 16);
}

/* Convert decimal to BCD */
uint8_t dec_to_bcd(uint8_t val)
{
    return ((val / 10) * 16) + (val % 10);
}

/* Function to initialize I2C master */
// esp_err_t i2c_master_init()
// {
//     i2c_master_bus_config_t i2c_config = {
//         .clk_source = I2C_CLK_SRC_DEFAULT,
//         .i2c_port = I2C_MASTER_NUM,
//         .scl_io_num = I2C_MASTER_SCL_IO,
//         .sda_io_num = I2C_MASTER_SDA_IO,
//         .glitch_ignore_cnt = 7,
//     };

//     // Initialize I2C bus
//     esp_err_t ret = i2c_new_master_bus(&i2c_config, &bus_handle);
//     if (ret != ESP_OK)
//     {
//         ESP_LOGE(TAG, "Failed to initialize I2C bus! Error: %s", esp_err_to_name(ret));
//         return ret;
//     }

//     ESP_LOGI(TAG, "I2C initialized successfully");

//     // Initialize DS3231 device
//     i2c_device_config_t dev_cfg = {
//         .dev_addr_length = I2C_ADDR_BIT_LEN_7,
//         .device_address = DS3231_ADDR,
//         .scl_speed_hz = I2C_MASTER_FREQ_HZ,
//     };

//     ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &ds3231_handle);
//     if (ret != ESP_OK)
//     {
//         ESP_LOGE(TAG, "Failed to add DS3231 device to I2C bus! Error: %s", esp_err_to_name(ret));
//         return ret;
//     }

//     return ESP_OK;
// }

esp_err_t i2c_master_init()
{
    i2c_master_bus_config_t i2c_mst_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .glitch_ignore_cnt = 7,
    };

    // Initialize I2C bus
    esp_err_t ret = i2c_new_master_bus(&i2c_mst_config, &bus_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize I2C bus! Error: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C bus initialized successfully");

    // Check if DS3231 device exists on the bus
    uint8_t cmd = 0x00;  // Address of the first register
    uint8_t data = 0;    // Buffer to store read data
    
    i2c_master_bus_handle_t temp_handle = bus_handle;
    ret = i2c_master_probe(temp_handle, DS3231_ADDR, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DS3231 not detected on I2C bus! Error: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }
    
    // Initialize DS3231 device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &ds3231_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add DS3231 device to I2C bus! Error: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DS3231 device detected and initialized successfully");
    return ESP_OK;
}

/* Function to read a register from DS3231 */
esp_err_t ds3231_read_register(uint8_t reg_addr, uint8_t *data)
{
    if (ds3231_handle == NULL)
    {
        ESP_LOGE(TAG, "DS3231 device not initialized!");
        return ESP_FAIL;
    }

    esp_err_t ret = i2c_master_transmit(ds3231_handle, &reg_addr, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write register address: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_master_receive(ds3231_handle, data, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read data: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* Function to get time and date from DS3231 */
ds3231_datetime_t ds3231_get_datetime()
{
    ds3231_datetime_t datetime = {0};
    esp_err_t ret;
    uint8_t temp;

    ret = ds3231_read_register(DS3231_REG_SECONDS, &temp);
    if (ret != ESP_OK)
        return datetime;
    datetime.seconds = temp;

    ret = ds3231_read_register(DS3231_REG_MINUTES, &temp);
    if (ret != ESP_OK)
        return datetime;
    datetime.minutes = temp;

    ret = ds3231_read_register(DS3231_REG_HOURS, &temp);
    if (ret != ESP_OK)
        return datetime;
    datetime.hours = temp;

    ret = ds3231_read_register(DS3231_REG_DAY, &temp);
    if (ret != ESP_OK)
        return datetime;
    datetime.day = temp;

    ret = ds3231_read_register(DS3231_REG_DATE, &temp);
    if (ret != ESP_OK)
        return datetime;
    datetime.date = temp;

    ret = ds3231_read_register(DS3231_REG_MONTH, &temp);
    if (ret != ESP_OK)
        return datetime;
    datetime.month = temp;

    ret = ds3231_read_register(DS3231_REG_YEAR, &temp);
    if (ret != ESP_OK)
        return datetime;
    datetime.year = temp;

    ESP_LOGI(TAG, "Date: %02d/%02d/20%02d | Day: %d | Time: %02d:%02d:%02d",
             bcd_to_dec(datetime.date), bcd_to_dec(datetime.month), bcd_to_dec(datetime.year),
             bcd_to_dec(datetime.day),
             bcd_to_dec(datetime.hours), bcd_to_dec(datetime.minutes), bcd_to_dec(datetime.seconds));

    return datetime;
}

ds3231_time_t ds3231_get_time()
{
    ds3231_time_t time = {0};
    esp_err_t ret;
    uint8_t temp;

    // Read seconds
    ret = ds3231_read_register(DS3231_REG_SECONDS, &temp);
    if (ret != ESP_OK)
        return time;
    time.seconds = temp;

    // Read minutes
    ret = ds3231_read_register(DS3231_REG_MINUTES, &temp);
    if (ret != ESP_OK)
        return time;
    time.minutes = temp;

    // Read hours
    ret = ds3231_read_register(DS3231_REG_HOURS, &temp);
    if (ret != ESP_OK)
        return time;
    time.hours = temp;

    ESP_LOGI(TAG, "Time: %02d:%02d:%02d",
             bcd_to_dec(time.hours), bcd_to_dec(time.minutes), bcd_to_dec(time.seconds));

    return time;
}

void get_file_path(char *output_path)
{
    const char *machine_id = "m-2003";
    const char *base_path = "/sdcard";
    struct tm date_obj = {0};
    char month[4];

    // Get datetime from DS3231
    ds3231_datetime_t datetime = ds3231_get_datetime();

    // Convert BCD values to decimal
    int day = bcd_to_dec(datetime.date);
    int month_num = bcd_to_dec(datetime.month);
    int year = 2000 + bcd_to_dec(datetime.year);

    date_obj.tm_mday = day;
    date_obj.tm_mon = month_num - 1;
    date_obj.tm_year = year - 1900;

    // Convert month number to name
    strftime(month, sizeof(month), "%b", &date_obj);

    // Convert month name to lowercase manually
    for (char *p = month; *p; p++)
    {
        if (*p >= 'A' && *p <= 'Z')
        {
            *p += 'a' - 'A';
        }
    }

    // Construct file path
    // sprintf(output_path, "%s/%s/%d/%s/%02d-%02d-%02d.csv",
    //         base_path, machine_id, year, month, day, month_num, year % 100);
   
    char dir_machine_id[32];
    sprintf(dir_machine_id, "%s/%s",
                 base_path, machine_id);
    
    char dir_year[32];
    sprintf(dir_year, "%s/%s/%d",
                 base_path, machine_id, year);
        
    char dir_months[32];
    sprintf(dir_months, "%s/%s/%d/%s",
                 base_path, machine_id, year, month);

    
    
    
    
    

    // Create the directory
    if (mkdir(dir_machine_id, 0777) != 0)
    {
        if (errno == EEXIST)
        {
            ESP_LOGI(TAG, "Directory already exists: %s", dir_machine_id);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create directory: %s", dir_machine_id);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Directory created: %s", dir_machine_id);
    }

    if (mkdir(dir_year, 0777) != 0)
    {
        if (errno == EEXIST)
        {
            ESP_LOGI(TAG, "Directory already exists: %s", dir_year);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create directory: %s", dir_year);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Directory created: %s", dir_year);
    }

    if (mkdir(dir_months, 0777) != 0)
    {
        if (errno == EEXIST)
        {
            ESP_LOGI(TAG, "Directory already exists: %s", dir_months);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to create directory: %s", dir_months);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Directory created: %s", dir_months);
    }

    sprintf(output_path, "%s/%s/%d/%s/%02d-%02d-%02d.csv",
                base_path, machine_id, year, month, day, month_num, year % 100);
}

void delete_file(const char *file_path)
{
    if (unlink(file_path) == 0)
    {
        printf("File deleted successfully: %s\n", file_path);
    }
    else
    {
        perror("Error deleting file");
    }
}
