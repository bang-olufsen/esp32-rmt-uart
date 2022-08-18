#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"
#include "hal/gpio_types.h"
#include "driver/rmt.h"

#define RMT_UART_NUM_0             (0) /*!< RMT UART port 0 */
#define RMT_UART_NUM_1             (1) /*!< RMT UART port 1 */
#if SOC_RMT_CHANNELS_PER_GROUP > 4
#define RMT_UART_NUM_2             (2) /*!< RMT UART port 2 */
#define RMT_UART_NUM_3             (3) /*!< RMT UART port 3 */
#define RMT_UART_NUM_4             (4) /*!< RMT UART port 4 */
#define RMT_UART_NUM_5             (5) /*!< RMT UART port 5 */
#define RMT_UART_NUM_6             (6) /*!< RMT UART port 6 */
#define RMT_UART_NUM_7             (7) /*!< RMT UART port 7 */
#define RMT_UART_NUM_MAX           (8)
#else
#define RMT_UART_NUM_MAX           (2)
#endif

typedef int rmt_uart_port_t;

typedef enum {
    RMT_UART_MODE_TX_ONLY  = 0x0,
    RMT_UART_MODE_RX_ONLY  = 0x1,
    RMT_UART_MODE_TX_RX   = 0x2,
} rmt_uart_mode_t;

typedef enum {
    RMT_UART_DATA_8_BITS   = 0x0,    /*!< word length: 8bits*/
    RMT_UART_DATA_9_BITS   = 0x1,    /*!< word length: 9bits*/
    RMT_UART_DATA_BITS_MAX = 0x2,
} rmt_uart_word_length_t;

typedef enum {
    RMT_UART_STOP_BITS_1   = 0x1,    /*!< stop bit: 1bit*/
    RMT_UART_STOP_BITS_MAX = 0x2,
} rmt_uart_stop_bits_t;

typedef enum {
    RMT_UART_PARITY_DISABLE  = 0x0,  /*!< Disable UART parity*/
} rmt_uart_parity_t;

typedef struct {
    int baud_rate;                        /*!< UART baud rate*/
    rmt_uart_mode_t mode;                 /*!< UART mode*/  
    rmt_uart_word_length_t data_bits;     /*!< UART byte size*/
    rmt_uart_parity_t parity;             /*!< UART parity mode*/
    rmt_uart_stop_bits_t stop_bits;       /*!< UART stop bits*/
    gpio_num_t tx_io_num;
    gpio_num_t rx_io_num;
    size_t rx_buffer_size;
    size_t tx_items_buffer_size;
    rmt_channel_t rmt_tx_channel;
    rmt_channel_t rmt_rx_channel;
} rmt_uart_config_t;

esp_err_t rmt_uart_init(rmt_uart_port_t uart_num_rx, rmt_uart_port_t uart_num_tx, const rmt_uart_config_t* uart_config);
esp_err_t rmt_uart_write_bytes(rmt_uart_port_t uart_num, const uint8_t* data, size_t size);
int rmt_uart_read_bytes(rmt_uart_port_t uart_num, uint8_t* buf, TickType_t ticks_to_wait);
esp_err_t rmt_uart_deinit(rmt_uart_port_t uart_num);


#ifdef __cplusplus
}
#endif