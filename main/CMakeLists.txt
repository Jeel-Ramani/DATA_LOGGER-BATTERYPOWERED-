

idf_component_register(SRCS "logger.c" "sd_card_example_main.c" "led_strip_encoder.c" "DS3231.c" "adc_read.c" "gpio_wakeup.c" "ext_wakeup.c" "touch_wakeup.c" 
                       INCLUDE_DIRS "."
                       REQUIRES fatfs sd_card nvs_flash  
                       PRIV_REQUIRES esp_timer usb esp_driver_gpio esp_driver_i2c esp_adc 
                       WHOLE_ARCHIVE)
