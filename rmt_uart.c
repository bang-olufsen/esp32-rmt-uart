#include <string.h>
#include "rmt_uart.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/rmt.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "rmt-uart";

typedef struct {
    rmt_item32_t* items;
    int item_index;
} rmt_uart_contex_tx_t;

typedef struct {
    uint8_t* bytes;
    int byte_num;
    int bit_num;
    uint16_t raw_data;
    RingbufHandle_t rb;
} rmt_uart_contex_rx_t;

typedef struct {
    rmt_config_t rmt_config_tx;
    rmt_config_t rmt_config_rx;
    rmt_uart_config_t rmt_uart_config;
    rmt_uart_contex_tx_t rmt_uart_contex_tx;
    rmt_uart_contex_rx_t rmt_uart_contex_rx;
    uint16_t rmt_bit_len;
    bool configured;
} rmt_uart_contex_t;

static rmt_uart_contex_t rmt_uart_contex[RMT_UART_NUM_MAX] = {0};

static int convert_byte_to_items(rmt_uart_contex_t* ctx, uint16_t byte)
{
    rmt_uart_contex_tx_t* rtc = &ctx->rmt_uart_contex_tx;
    size_t total_bits = ctx->rmt_uart_config.data_bits == RMT_UART_DATA_8_BITS ? 9 : 11;
    uint16_t stop_bit = ctx->rmt_uart_config.data_bits == RMT_UART_DATA_8_BITS ? 0x200 : 0xC00;
    uint16_t data = (byte << 1) | stop_bit;
    ESP_LOGD("rmt convert","total_bits:%d, stop_bit:%d, data: %.4X",total_bits, stop_bit, data);

    for (int i = 0; i < total_bits; i += 2)
    {
        rmt_item32_t* item = &rtc->items[rtc->item_index];
        item->duration0 = ctx->rmt_bit_len;
        item->duration1 = ctx->rmt_bit_len;
        item->level0 = (data >> i) ^ 1;
        item->level1 = (data >> (i + 1)) ^ 1;
        rtc->item_index++;
        if (rtc->item_index >= ctx->rmt_uart_config.tx_items_buffer_size)
        {
            ESP_LOGE(TAG, "DATA TOO LONG - increase tx_items_buffer_size");
            return -1;
        }
        ESP_LOGD(TAG, "\trmt tx item %02d: duration0: %d level0: %d  duration1: %d level1: %d", rtc->item_index, item->duration0, item->level0, item->duration1, item->level1);
    }
    return 0;
}


static int convert_data_to_items(rmt_uart_contex_t* ctx, const uint8_t* data, uint16_t len)
{
    rmt_uart_contex_tx_t* rtc = &ctx->rmt_uart_contex_tx;
    rtc->item_index = 0;
    
    for (int i = 0; i < len; ++i)
    {
        if(ctx->rmt_uart_config.data_bits == RMT_UART_DATA_9_BITS) {
            if(convert_byte_to_items(ctx, (data[i]<<8)|data[i+1])) return -1;
            i++;
        }
        else {
            if (convert_byte_to_items(ctx, (uint16_t)data[i])) return -1;
        }
    }
    return rtc->item_index;
}


static unsigned int round_closest(unsigned int dividend, unsigned int divisor)
{
    return (dividend + (divisor / 2)) / divisor;
}

static void fill_bits(rmt_uart_contex_t* ctx, uint32_t duration, uint32_t level)
{
    rmt_uart_contex_rx_t* rrc = &ctx->rmt_uart_contex_rx;
    int rmt_bit_numbers = round_closest(duration, ctx->rmt_bit_len);
    // all remaining bits are high
    if (rmt_bit_numbers == 0 && level)
    {
        rmt_bit_numbers = 11 - rrc->bit_num;
    }

    int from = rrc->bit_num;
    int to = rrc->bit_num + rmt_bit_numbers;
    for (int j = from; j < to; ++j)
    {
        if (rrc->bit_num == 0 && level)
        {
            // ESP_LOGW(TAG, "not a start bit, skip"); 
            return;
        }
        rrc->bit_num++;
        if (rrc->bit_num == 11 && !level)
        {
           ESP_LOGE(TAG, "not a stop bit byte_num=%d", rrc->byte_num);
           return;
        }
        rrc->raw_data |= (level << j);
        if (rrc->bit_num == 11)
        {
            rrc->raw_data >>= 1;
            rrc->raw_data &= 0x01FF;
            rrc->bytes[rrc->byte_num] = rrc->raw_data >> 8;
            rrc->bytes[rrc->byte_num+1] = rrc->raw_data & 0xFF;
            ESP_LOGD(TAG, "\trmt rx data=%d", rrc->raw_data);
            rrc->byte_num+=2;
            rrc->bit_num = 0;
            rrc->raw_data = 0;
        }
    }
}

esp_err_t rmt_uart_init(rmt_uart_port_t uart_num_rx, rmt_uart_port_t uart_num_tx, const rmt_uart_config_t* rmt_uart_config)
{
    const int RMT_DIV = APB_CLK_FREQ / 50 / rmt_uart_config->baud_rate;
    const int RMT_TICK = APB_CLK_FREQ / RMT_DIV;
    ESP_LOGD(TAG, "baud=%d rmt_div=%d rmt_tick=%d", rmt_uart_config->baud_rate, RMT_DIV, RMT_TICK);

    ESP_RETURN_ON_FALSE((uart_num_rx < RMT_UART_NUM_MAX), ESP_FAIL, TAG, "uart_num error");
    ESP_RETURN_ON_FALSE((rmt_uart_config), ESP_FAIL, TAG, "uart_config error");
    ESP_RETURN_ON_FALSE(((10UL * RMT_TICK / rmt_uart_config->baud_rate) < 0xFFFF), ESP_FAIL, TAG, "rmt tick too long, reconfigure 'RMT_DIV'");
    ESP_RETURN_ON_FALSE(((RMT_TICK / rmt_uart_config->baud_rate) > 49), ESP_FAIL, TAG, "rmt tick too long, reconfigure 'RMT_DIV'");
    ESP_RETURN_ON_FALSE(((RMT_TICK / rmt_uart_config->baud_rate / 2) < 0xFF), ESP_FAIL, TAG, "baud rate too slow, reconfigure 'RMT_DIV'");

    uint16_t bit_len = RMT_TICK / rmt_uart_config->baud_rate;
    rmt_uart_contex[uart_num_rx].rmt_bit_len = bit_len;
    rmt_uart_contex[uart_num_tx].rmt_bit_len = bit_len;
    memcpy(&rmt_uart_contex[uart_num_rx].rmt_uart_config, rmt_uart_config, sizeof(rmt_uart_config_t));
    memcpy(&rmt_uart_contex[uart_num_tx].rmt_uart_config, rmt_uart_config, sizeof(rmt_uart_config_t));

    if (rmt_uart_config->mode == RMT_UART_MODE_RX_ONLY || rmt_uart_config->mode == RMT_UART_MODE_TX_RX)
    {
        rmt_config_t rmt_config_rx = RMT_DEFAULT_CONFIG_RX(rmt_uart_config->rx_io_num, rmt_uart_config->rmt_rx_channel);
        rmt_config_rx.clk_div = RMT_DIV;
        rmt_config_rx.mem_block_num = (rmt_uart_config->mode == RMT_UART_MODE_RX_ONLY) ? 2 : 1;
        rmt_config_rx.rx_config.idle_threshold = 15 * bit_len;
        rmt_config_rx.rx_config.filter_en = true;
        rmt_config_rx.rx_config.filter_ticks_thresh = bit_len / 2;
        ESP_ERROR_CHECK(rmt_config(&rmt_config_rx));
        ESP_ERROR_CHECK(rmt_driver_install(rmt_config_rx.channel, rmt_uart_config->rx_buffer_size, 0));
        rmt_uart_contex[uart_num_rx].rmt_config_rx = rmt_config_rx;
        rmt_get_ringbuf_handle(rmt_config_rx.channel, &rmt_uart_contex[uart_num_rx].rmt_uart_contex_rx.rb);
        ESP_RETURN_ON_FALSE((rmt_uart_contex[uart_num_rx].rmt_uart_contex_rx.rb), ESP_FAIL, TAG, "rmt ringbuffer is null");
        rmt_rx_start(rmt_uart_contex[uart_num_rx].rmt_config_rx.channel, true);
    }

    if (rmt_uart_config->mode == RMT_UART_MODE_TX_ONLY || rmt_uart_config->mode == RMT_UART_MODE_TX_RX)
    {
        rmt_config_t rmt_config_tx = RMT_DEFAULT_CONFIG_TX(rmt_uart_config->tx_io_num, rmt_uart_config->rmt_tx_channel);
        rmt_config_tx.tx_config.carrier_en = false;
        rmt_config_tx.clk_div = RMT_DIV;
        rmt_config_tx.flags = RMT_CHANNEL_FLAGS_INVERT_SIG;
        ESP_ERROR_CHECK(rmt_config(&rmt_config_tx));
        ESP_ERROR_CHECK(rmt_driver_install(rmt_config_tx.channel, 0, 0));
        rmt_uart_contex[uart_num_tx].rmt_config_tx = rmt_config_tx;

        #if CONFIG_SPIRAM_USE_MALLOC
        rmt_uart_contex[uart_num_tx].rmt_uart_contex_tx.items = heap_caps_calloc(1, rmt_uart_config->tx_items_buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        #else
        rmt_uart_contex[uart_num_tx].rmt_uart_contex_tx.items = calloc(rmt_uart_config->tx_items_buffer_size, sizeof(rmt_item32_t));
        #endif
    }

    rmt_uart_contex[uart_num_rx].configured = true;
    rmt_uart_contex[uart_num_tx].configured = true;

    return ESP_OK;
}

esp_err_t rmt_uart_write_bytes(rmt_uart_port_t uart_num, const uint8_t* data, size_t size)
{
    rmt_uart_contex_t* ctx = &rmt_uart_contex[uart_num];
    ESP_RETURN_ON_FALSE((ctx->configured), ESP_FAIL, TAG, "uart not configured");
    ESP_RETURN_ON_FALSE((ctx->rmt_uart_config.mode != RMT_UART_MODE_RX_ONLY), ESP_FAIL, TAG, "uart RX only");
    rmt_uart_contex_tx_t* rtc = &ctx->rmt_uart_contex_tx;
    if (convert_data_to_items(ctx, data, size) < 0) return ESP_FAIL;
    return rmt_write_items(ctx->rmt_config_tx.channel, ctx->rmt_uart_contex_tx.items, rtc->item_index, true);
}

int rmt_uart_read_bytes(rmt_uart_port_t uart_num, uint8_t* buf, TickType_t ticks_to_wait)
{
    rmt_uart_contex_t* ctx = &rmt_uart_contex[uart_num];
    ESP_RETURN_ON_FALSE((ctx->configured), ESP_FAIL, TAG, "uart not configured");
    ESP_RETURN_ON_FALSE((ctx->rmt_uart_config.mode != RMT_UART_MODE_TX_ONLY), ESP_FAIL, TAG, "uart TX only");
    rmt_uart_contex_rx_t* rrc = &ctx->rmt_uart_contex_rx;
    rrc->bytes = buf;
    rrc->byte_num = 0;
    rrc->bit_num = 0;
    size_t length = 0;
    rmt_item32_t* items = NULL;
    
    items = (rmt_item32_t*)xRingbufferReceive(rrc->rb, &length, ticks_to_wait);
    if (items) {
        for (size_t i = 0; i < length / 4; ++i)
        {
            ESP_LOGD(TAG, "\trmt rx item %02d: duration0: %d level0: %d  duration1: %d level1: %d", i, items[i].duration0, items[i].level0, items[i].duration1, items[i].level1);
            fill_bits(ctx, items[i].duration0, items[i].level0);
            fill_bits(ctx, items[i].duration1, items[i].level1);
        }
        vRingbufferReturnItem(rrc->rb, (void*)items);

    }
    rrc->byte_num = length;
    ESP_LOGD(TAG, "\trx is complete byte_num=%d", rrc->byte_num);
    return rrc->byte_num;
}

esp_err_t rmt_uart_deinit(rmt_uart_port_t uart_num)
{
    rmt_uart_contex_t* ctx = &rmt_uart_contex[uart_num];
    esp_err_t ret = ESP_FAIL;
    
    if (!ctx->configured) {
        return ret;
    }

    if (ctx->rmt_uart_config.mode == RMT_UART_MODE_TX_RX) {
        if (ctx->rmt_config_rx.rmt_mode == RMT_MODE_RX) {
            ret = rmt_driver_uninstall(uart_num);
            ESP_LOGD(TAG,"deinit RX uart_num %d", uart_num);
            if (ret != ESP_OK) return ret;
        } else if (ctx->rmt_config_tx.rmt_mode == RMT_MODE_TX) {
            rmt_uart_contex_tx_t* rtc = &ctx->rmt_uart_contex_tx;
            #if CONFIG_SPIRAM_USE_MALLOC
                heap_caps_free(rtc->items);
            #else
                free(rtc->items);
            #endif
            ret = rmt_driver_uninstall(uart_num);
            ESP_LOGD(TAG,"deinit TX uart_num %d", uart_num);
            if (ret != ESP_OK) return ret;
        }
    }

    else if (ctx->rmt_uart_config.mode != RMT_UART_MODE_RX_ONLY)
    {
        rmt_uart_contex_tx_t* rtc = &ctx->rmt_uart_contex_tx;
#if CONFIG_SPIRAM_USE_MALLOC
        heap_caps_free(rtc->items);
#else
        free(rtc->items);
#endif
        ret = rmt_driver_uninstall(uart_num);
        ESP_LOGD(TAG,"deinit TX uart_num %d", uart_num);
        if (ret != ESP_OK) return ret;
    }

    else if (ctx->rmt_uart_config.mode != RMT_UART_MODE_TX_ONLY)
    {
        ret = rmt_driver_uninstall(uart_num);
        ESP_LOGD(TAG,"deinit RX uart_num %d", uart_num);
        if (ret != ESP_OK) return ret;
    }

    ctx->configured = false;
    return ret;
}