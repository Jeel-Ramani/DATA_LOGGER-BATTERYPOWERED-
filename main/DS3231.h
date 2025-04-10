#ifndef DS3231_H
#define DS3231_H

#include <stdint.h>
#include "esp_err.h"

/* I2C Configuration */
#define I2C_MASTER_SCL_IO 9       /*!< GPIO for SCL */
#define I2C_MASTER_SDA_IO 8       /*!< GPIO for SDA */
#define I2C_MASTER_NUM 0          /*!< I2C port number */
#define I2C_MASTER_FREQ_HZ 400000 /*!< I2C clock frequency */
#define DS3231_ADDR 0x68          /*!< I2C address of DS3231 */

/* DS3231 Register Addresses */
#define DS3231_REG_SECONDS 0x00
#define DS3231_REG_MINUTES 0x01
#define DS3231_REG_HOURS 0x02
#define DS3231_REG_DAY 0x03
#define DS3231_REG_DATE 0x04
#define DS3231_REG_MONTH 0x05
#define DS3231_REG_YEAR 0x06

/* Data structure to store DS3231 Date and Time */
typedef struct
{
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t date;
    uint8_t month;
    uint8_t year;
} ds3231_datetime_t;

typedef struct {
    uint8_t hours;
    uint8_t minutes;
    uint8_t seconds;
} ds3231_time_t;
/* Function prototypes */
esp_err_t i2c_master_init(void);
esp_err_t ds3231_read_register(uint8_t reg_addr, uint8_t *data);
ds3231_datetime_t ds3231_get_datetime(void);
ds3231_time_t ds3231_get_time(void);
uint8_t bcd_to_dec(uint8_t val);
uint8_t dec_to_bcd(uint8_t val);
void get_file_path(char *output_path);
void delete_file(const char *file_path);
#endif /* DS3231_H */
