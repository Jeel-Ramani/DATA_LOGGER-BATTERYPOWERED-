#ifndef ADC_READER_H
#define ADC_READER_H

#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#define ADC1_CHANNEL_3 ADC_CHANNEL_3  // GPIO39
#define ADC1_CHANNEL_2 ADC_CHANNEL_4  // GPIO40

void adc_reader_init(void);
int adc_reader_get_value1(void);
int adc_reader_get_value2(void);
void adc_reader_deinit(void);

#endif // ADC_READER_H
