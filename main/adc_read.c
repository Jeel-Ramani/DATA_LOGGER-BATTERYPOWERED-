#include "adc_read.h"
#include "esp_log.h"

static const char *TAG = "ADC_READER";
static adc_oneshot_unit_handle_t adc1_handle;
static adc_cali_handle_t adc_cali_handle;
static bool is_calibrated = false;

static bool adc_reader_calibration_init(void);

void adc_reader_init(void) {
    ESP_LOGI(TAG, "Initializing ADC...");

    adc_oneshot_unit_init_cfg_t adc_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_config, &adc1_handle));

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,  // Use correct attenuation level
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC1_CHANNEL_3, &channel_config));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC1_CHANNEL_2, &channel_config));

    is_calibrated = adc_reader_calibration_init();
}

static bool adc_reader_calibration_init(void) {
    ESP_LOGI(TAG, "Initializing ADC Calibration...");
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    if (adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle) == ESP_OK) {
        ESP_LOGI(TAG, "ADC Calibration Successful!");
        return true;
    } else {
        ESP_LOGW(TAG, "ADC Calibration Failed! Proceeding without calibration.");
        return false;
    }
}

int adc_reader_get_value1(void) {
    int raw_value = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC1_CHANNEL_3, &raw_value));
    
    if (is_calibrated) {
        int voltage = 0;
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, raw_value, &voltage));
        return voltage;
    }

    return raw_value;
}

int adc_reader_get_value2(void) {
    int raw_value = 0;
    ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC1_CHANNEL_2, &raw_value));

    if (is_calibrated) {
        int voltage = 0;
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(adc_cali_handle, raw_value, &voltage));
        return voltage;
    }

    return raw_value;
}

void adc_reader_deinit(void) {
    ESP_ERROR_CHECK(adc_oneshot_del_unit(adc1_handle));

    if (is_calibrated) {
        ESP_LOGI(TAG, "Deinitializing ADC Calibration...");
        ESP_ERROR_CHECK(adc_cali_delete_scheme_line_fitting(adc_cali_handle));
    }
}
