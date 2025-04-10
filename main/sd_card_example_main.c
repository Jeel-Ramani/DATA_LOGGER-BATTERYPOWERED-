/* SD card and FAT filesystem example.
   This example uses SPI peripheral to communicate with SD card.

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

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
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "soc/soc_caps.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "deep_sleep_example.h"
#if SOC_RTC_FAST_MEM_SUPPORTED
static RTC_DATA_ATTR struct timeval sleep_enter_time;
#else
static struct timeval sleep_enter_time;
#endif
#if SOC_SDMMC_IO_POWER_EXTERNAL
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
static const char *TAG = "example";
#define EXAMPLE_MAX_CHAR_SIZE 32
static RTC_DATA_ATTR esp_sleep_wakeup_cause_t wakeup_reason;
#define MOUNT_POINT "/sdcard"
#define MNT_PATH "/usb"
#define APP_QUIT_PIN GPIO_NUM_0
#define BUFFER_SIZE 4096
// Pin assignments can be set in menuconfig, see "SD SPI Example Configuration" menu.
// You can also change the pin assignments here by changing the following 4 lines.
#define PIN_NUM_MISO 14
#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK 12
#define PIN_NUM_CS 11

static bool dev_present = false;

/**
 * @brief Application Queue and its messages ID
 */
static QueueHandle_t app_queue;
typedef struct
{
    enum
    {
        APP_QUIT,                // Signals request to exit the application
        APP_DEVICE_CONNECTED,    // USB device connect event
        APP_DEVICE_DISCONNECTED, // USB device disconnect event
    } id;
    union
    {
        uint8_t new_dev_address; // Address of new USB device for APP_DEVICE_CONNECTED event if
    } data;
} app_message_t;

/**
 * @brief BOOT button pressed callback
 *
 * Signal application to exit the main task
 *
 * @param[in] arg Unused
 */
static void gpio_cb(void *arg)
{
    BaseType_t xTaskWoken = pdFALSE;
    app_message_t message = {
        .id = APP_QUIT,
    };

    if (app_queue)
    {
        xQueueSendFromISR(app_queue, &message, &xTaskWoken);
    }

    if (xTaskWoken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief MSC driver callback
 *
 * Signal device connection/disconnection to the main task
 *
 * @param[in] event MSC event
 * @param[in] arg   MSC event data
 */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    if (event->event == MSC_DEVICE_CONNECTED)
    {
        ESP_LOGI(TAG, "MSC device connected (usb_addr=%d)", event->device.address);
        app_message_t message = {
            .id = APP_DEVICE_CONNECTED,
            .data.new_dev_address = event->device.address,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    }
    else if (event->event == MSC_DEVICE_DISCONNECTED)
    {
        ESP_LOGI(TAG, "MSC device disconnected");
        app_message_t message = {
            .id = APP_DEVICE_DISCONNECTED,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    }
}

static void print_device_info(msc_host_device_info_t *info)
{
    const size_t megabyte = 1024 * 1024;
    uint64_t capacity = ((uint64_t)info->sector_size * info->sector_count) / megabyte;

    printf("Device info:\n");
    printf("\t Capacity: %llu MB\n", capacity);
    printf("\t Sector size: %" PRIu32 "\n", info->sector_size);
    printf("\t Sector count: %" PRIu32 "\n", info->sector_count);
    printf("\t PID: 0x%04X \n", info->idProduct);
    printf("\t VID: 0x%04X \n", info->idVendor);
#ifndef CONFIG_NEWLIB_NANO_FORMAT
    wprintf(L"\t iProduct: %S \n", info->iProduct);
    wprintf(L"\t iManufacturer: %S \n", info->iManufacturer);
    wprintf(L"\t iSerialNumber: %S \n", info->iSerialNumber);
#endif
}
static void usb_task(void *args)
{
    const usb_host_config_t host_config = {.intr_flags = ESP_INTR_FLAG_LEVEL1};
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_config));

    bool has_clients = true;
    while (true)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            has_clients = false;
            if (usb_host_device_free_all() == ESP_OK)
            {
                break;
            };
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE && !has_clients)
        {
            break;
        }
    }

    vTaskDelay(10); // Give clients some time to uninstall
    ESP_LOGI(TAG, "Deinitializing USB");
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

// void speed_test(void)
// {
// #define TEST_FILE "/usb/esp/dummy"
// #define ITERATIONS 256 // 256 * 4kb = 1MB
//     int64_t test_start, test_end;

//     FILE *f = fopen(TEST_FILE, "wb+");
//     if (f == NULL)
//     {
//         ESP_LOGE(TAG, "Failed to open file for writing");
//         return;
//     }
//     // Set larger buffer for this file. It results in larger and more effective USB transfers
//     setvbuf(f, NULL, _IOFBF, BUFFER_SIZE);

//     // Allocate application buffer used for read/write
//     uint8_t *data = malloc(BUFFER_SIZE);
//     assert(data);

//     ESP_LOGI(TAG, "Writing to file %s", TEST_FILE);
//     test_start = esp_timer_get_time();
//     for (int i = 0; i < ITERATIONS; i++)
//     {
//         if (fwrite(data, BUFFER_SIZE, 1, f) == 0)
//         {
//             return;
//         }
//     }
//     test_end = esp_timer_get_time();
//     ESP_LOGI(TAG, "Write speed %1.2f MiB/s", (BUFFER_SIZE * ITERATIONS) / (float)(test_end - test_start));
//     rewind(f);

//     ESP_LOGI(TAG, "Reading from file %s", TEST_FILE);
//     test_start = esp_timer_get_time();
//     for (int i = 0; i < ITERATIONS; i++)
//     {
//         if (0 == fread(data, BUFFER_SIZE, 1, f))
//         {
//             return;
//         }
//     }
//     test_end = esp_timer_get_time();
//     ESP_LOGI(TAG, "Read speed %1.2f MiB/s", (BUFFER_SIZE * ITERATIONS) / (float)(test_end - test_start));

//     fclose(f);
//     free(data);
// }

static void deep_sleep_task(void *args)
{
    /**
     * Prefer to use RTC mem instead of NVS to save the deep sleep enter time, unless the chip
     * does not support RTC mem(such as esp32c2). Because the time overhead of NVS will cause
     * the recorded deep sleep enter time to be not very accurate.
     */
#if !SOC_RTC_FAST_MEM_SUPPORTED
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    nvs_handle_t nvs_handle;
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    }
    else
    {
        printf("Open NVS done\n");
    }

    // Get deep sleep enter time
    nvs_get_i32(nvs_handle, "slp_enter_sec", (int32_t *)&sleep_enter_time.tv_sec);
    nvs_get_i32(nvs_handle, "slp_enter_usec", (int32_t *)&sleep_enter_time.tv_usec);
#endif

    

    vTaskDelay(1000 / portTICK_PERIOD_MS);

#if CONFIG_IDF_TARGET_ESP32
    // Isolate GPIO12 pin from external circuits. This is needed for modules
    // which have an external pull-up resistor on GPIO12 (such as ESP32-WROVER)
    // to minimize current consumption.
    rtc_gpio_isolate(GPIO_NUM_12);
#endif

    printf("Entering deep sleep\n");

    // get deep sleep enter time
    gettimeofday(&sleep_enter_time, NULL);

#if !SOC_RTC_FAST_MEM_SUPPORTED
    // record deep sleep enter time via nvs
    ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, "slp_enter_sec", sleep_enter_time.tv_sec));
    ESP_ERROR_CHECK(nvs_set_i32(nvs_handle, "slp_enter_usec", sleep_enter_time.tv_usec));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
#endif

    // enter deep sleep
    esp_deep_sleep_start();
}
static esp_err_t s_example_write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "a");
    if (f == NULL)
    {
        ESP_LOGE(TAG, "Failed to open file for writing");
        return ESP_FAIL;
    }
    fprintf(f, data);
    fclose(f);
    ESP_LOGI(TAG, "File written");

    return ESP_OK;
}

static void example_deep_sleep_register_rtc_timer_wakeup(void)
{
    const int wakeup_time_sec = 30;
    printf("Enabling timer wakeup, %ds\n", wakeup_time_sec);
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000));
}
// static esp_err_t transfer_data()
// {
//     FILE *fptr1, *fptr2;
//     const char *source_file = "/sdcard/hello.csv";
//     int c;

//     // Open one file for reading
//     fptr1 = fopen(source_file, "r");
//     if (fptr1 == NULL)
//     {
//         printf("Cannot open file %s\n", source_file);
//         exit(1);
//     }

//     const char *dest_file = "/usb/esp/11_00_00.csv";
//     // Open another file for writing
//     fptr2 = fopen(dest_file, "w");
//     if (fptr2 == NULL)
//     {
//         printf("Cannot open file %s\n", dest_file);
//         exit(1);
//     }

//     // Read contents from file
//     while ((c = fgetc(fptr1)) != EOF)
//     {
//         fputc(c, fptr2);
//     }

//     printf("Contents copied to %s\n", dest_file);

//     fclose(fptr1);
//     fclose(fptr2);
//     return 0;
// }

// static void generate_random_data(char *data, size_t max_size)
// {
//     // Generate some random numeric data for CSV
//     srand(0);
//     float temp = (float)(rand() % 500) / 10.0;      // Random temp between 0-50
//     float humidity = (float)(rand() % 1000) / 10.0; // Random humidity between 0-100
//     int pressure = rand() % 1200;                   // Random pressure between 0-1200

//     snprintf(data, max_size, "%.1f,%.1f,%d \n",
//              temp,
//              humidity,
//              pressure);
// }
// static esp_err_t s_example_read_file(const char *path)
// {
//     ESP_LOGI(TAG, "Reading file %s", path);
//     FILE *f = fopen(path, "r");
//     if (f == NULL)
//     {
//         ESP_LOGE(TAG, "Failed to open file for reading");
//         return ESP_FAIL;
//     }
//     char line[EXAMPLE_MAX_CHAR_SIZE];
//     fgets(line, sizeof(line), f);
//     fclose(f);

//     // strip newline
//     char *pos = strchr(line, '\n');
//     if (pos)
//     {
//         *pos = '\0';
//     }
//     ESP_LOGI(TAG, "Read from file: '%s'", line);

//     return ESP_OK;
// }
void log_data()
{
    char output_path[32];

    //     char dir_path[64];
    //     char file_path[256];
    //     const char *dir_name = "mydir/jeeldata";
    //     const char *file_name = "test.csv";
    // #define MOUNT_POINT "/sdcard"
    //     // Construct directory path: /sdcard/mydir
    //     snprintf(dir_path, sizeof(dir_path), "%s/%s", MOUNT_POINT, dir_name);

    //     // Create the directory
    //     if (mkdir(dir_path, 0777) != 0)
    //     {
    //         if (errno == EEXIST)
    //         {
    //             ESP_LOGI(TAG, "Directory already exists: %s", dir_path);
    //         }
    //         else
    //         {
    //             ESP_LOGE(TAG, "Failed to create directory: %s", dir_path);
    //         }
    //     }
    //     else
    //     {
    //         ESP_LOGI(TAG, "Directory created: %s", dir_path);
    //     }

    // ds3231_get_datetime();

    int voltage1 = adc_reader_get_value1();
    int voltage2 = adc_reader_get_value2();

    printf("ADC1 Channel 3: %d mV\n", voltage1);
    printf("ADC1 Channel 4: %d mV\n", voltage2);

    ds3231_time_t current_time = ds3231_get_time();
    char time_str[20];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
             bcd_to_dec(current_time.hours),
             bcd_to_dec(current_time.minutes),
             bcd_to_dec(current_time.seconds));
    printf("Time: %s, Voltage1: %d, Voltage2: %d\n", time_str, voltage1, voltage2);
    char test_string[32];
    snprintf(test_string, 32, "%s,%d,%d\n", time_str, voltage1, voltage2);
    printf("%s\n", test_string);

    get_file_path(output_path);
    printf("%s \n", output_path);
    // printf("%s \n", log_data);

    s_example_write_file(output_path, test_string);

    // ESP_LOGI(TAG, "Opening file %s", output_path);
    // FILE *f = fopen(output_path, "a");
    // if (f == NULL)
    // {
    //     ESP_LOGE(TAG, "Failed to open file for writing");

    // }
    // fprintf(f, "1 , 2 ,34,");import tkinter as tk

    // fclose(f);
    // ESP_LOGI(TAG, "File written");
}


void wake_checker()
{
    struct timeval now;
    gettimeofday(&now, NULL);
    int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

    switch (esp_sleep_get_wakeup_cause())
    {
    case ESP_SLEEP_WAKEUP_TIMER:
    {
        printf("Wake up from timer. Time spent in deep sleep: %dms\n", sleep_time_ms);
        printf("sleeping after logging data \n");
        xTaskCreate(deep_sleep_task, "deep_sleep_task", 4096, NULL, 6, NULL);
        break;
    }

#if CONFIG_EXAMPLE_GPIO_WAKEUP
    case ESP_SLEEP_WAKEUP_GPIO:
    {
        uint64_t wakeup_pin_mask = esp_sleep_get_gpio_wakeup_status();
        if (wakeup_pin_mask != 0)
        {
            int pin = __builtin_ffsll(wakeup_pin_mask) - 1;
            printf("Wake up from GPIO %d\n", pin);
        }
        else
        {
            printf("Wake up from GPIO\n");
        }
        break;
    }
#endif // CONFIG_EXAMPLE_GPIO_WAKEUP

#if CONFIG_EXAMPLE_EXT0_WAKEUP
    case ESP_SLEEP_WAKEUP_EXT0:
    {
        printf("Wake up from ext0\n");
        break;
    }
#endif // CONFIG_EXAMPLE_EXT0_WAKEUP


    case ESP_SLEEP_WAKEUP_EXT1:
    {
        uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
        if (wakeup_pin_mask != 0)
        {
            int pin = __builtin_ffsll(wakeup_pin_mask) - 1;
            printf("Wake up from GPIO %d\n", pin);
            printf("entering data extraction mode\n");
        }
        else
        {
            printf("Wake up from GPIO\n");
        }
        break;
    }


#ifdef CONFIG_EXAMPLE_TOUCH_WAKEUP
    case ESP_SLEEP_WAKEUP_TOUCHPAD:
    {
        printf("Wake up from touch on pad %d\n", esp_sleep_get_touchpad_wakeup_status());
        break;
    }
#endif // CONFIG_EXAMPLE_TOUCH_WAKEUP

    case ESP_SLEEP_WAKEUP_UNDEFINED:
    default:
        printf("Not a deep sleep reset\n");
        xTaskCreate(deep_sleep_task, "deep_sleep_task", 4096, NULL, 6, NULL);
    }
}
void app_main(void)
{   wake_checker();
    example_deep_sleep_register_rtc_timer_wakeup();

    /* Enable wakeup from deep sleep by ext1 */
    example_deep_sleep_register_ext1_wakeup();

    // Log the previous wakeup reason if this is not the first boot
    if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED)
    {
        printf("Previous wakeup reason: %d\n", wakeup_reason);
    }
    adc_reader_init();
    app_queue = xQueueCreate(5, sizeof(app_message_t));
    assert(app_queue);

    BaseType_t task_created = xTaskCreate(usb_task, "usb_task", 4096, NULL, 2, NULL);
    assert(task_created);

    // Init BOOT button: Pressing the button simulates app request to exit
    // It will disconnect the USB device and uninstall the MSC driver and USB Host Lib
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_cb, NULL));

    ESP_LOGI(TAG, "Waiting for USB flash drive to be connected");
    msc_host_device_handle_t msc_device = NULL;
    msc_host_vfs_handle_t vfs_handle = NULL;
    esp_err_t ret;

    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    // Use settings defined above to initialize SD card and mount FAT filesystem.
    // Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
    // Please check its source code and implement error recovery when developing
    // production applications.
    ESP_LOGI(TAG, "Using SPI peripheral");

    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 20MHz for SDSPI)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

    // For SoCs where the SD power can be supplied both via an internal or external (e.g. on-board LDO) power supply.
    // When using specific IO pins (which can be used for ultra high-speed SDMMC) to connect to the SD card
    // and the internal LDO power supply, we need to initialize the power supply first.
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create a new on-chip LDO power control driver");
        return;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                          "If you want the card to be formatted, set the CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                          "Make sure SD card lines have pull-up resistors in place.",
                     esp_err_to_name(ret));
#ifdef CONFIG_EXAMPLE_DEBUG_PIN_CONNECTIONS
            check_sd_card_pins(&config, pin_count);
#endif
        }
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    // Use POSIX and C standard library functions to work with files.

    // First create a file.
    const char *file_hello = MOUNT_POINT "/hello.csv";
    char data[EXAMPLE_MAX_CHAR_SIZE];
    snprintf(data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Hello from ESP32", card->cid.name);
    ret = s_example_write_file("/sdcard/jeel.csv", data);
    if (ret != ESP_OK)
    {
        return;
    }
    ESP_ERROR_CHECK(i2c_master_init());

    log_data();
    wake_checker();
    

    
    // while (1)
    // {
    //     app_message_t msg;
    //     xQueueReceive(app_queue, &msg, portMAX_DELAY);

    //     if (msg.id == APP_DEVICE_CONNECTED)
    //     {
    //         if (dev_present)
    //         {
    //             ESP_LOGW(TAG, "MSC Example handles only one device at a time");
    //         }
    //         else
    //         {
    //             // 0. Change flag
    //             dev_present = true;
    //             // 1. MSC flash drive connected. Open it and map it to Virtual File System
    //             ESP_ERROR_CHECK(msc_host_install_device(msg.data.new_dev_address, &msc_device));
    //             const esp_vfs_fat_mount_config_t mount_config = {
    //                 .format_if_mount_failed = false,
    //                 .max_files = 3,
    //                 .allocation_unit_size = 8192,
    //             };
    //             ESP_ERROR_CHECK(msc_host_vfs_register(msc_device, MNT_PATH, &mount_config, &vfs_handle));

    //             // 2. Print information about the connected disk
    //             msc_host_device_info_t info;
    //             ESP_ERROR_CHECK(msc_host_get_device_info(msc_device, &info));
    //             msc_host_print_descriptors(msc_device);
    //             print_device_info(&info);

    //             // 3. List all the files in root directory
    //             ESP_LOGI(TAG, "ls command output:");
    //             struct dirent *d;
    //             DIR *dh = opendir(MNT_PATH);
    //             assert(dh);
    //             while ((d = readdir(dh)) != NULL)
    //             {
    //                 printf("%s\n", d->d_name);
    //             }
    //             closedir(dh);

    //             // 4. The disk is mounted to Virtual File System, perform some basic demo file operation

    //             transfer_data();

    //             // 5. Perform speed test
    //             speed_test();

    //             ESP_LOGI(TAG, "Example finished, you can disconnect the USB flash drive");
    //         }
    //     }
    //     if ((msg.id == APP_DEVICE_DISCONNECTED) || (msg.id == APP_QUIT))
    //     {
    //         if (dev_present)
    //         {
    //             dev_present = false;
    //             if (vfs_handle)
    //             {
    //                 ESP_ERROR_CHECK(msc_host_vfs_unregister(vfs_handle));
    //                 vfs_handle = NULL;
    //             }
    //             if (msc_device)
    //             {
    //                 ESP_ERROR_CHECK(msc_host_uninstall_device(msc_device));
    //                 msc_device = NULL;
    //             }
    //         }
    //         if (msg.id == APP_QUIT)
    //         {
    //             // This will cause the usb_task to exit
    //             ESP_ERROR_CHECK(msc_host_uninstall());
    //             break;
    //         }
    //     }
    // }
}