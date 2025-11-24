/*
 * This file is part of the ESP32 RTKPubcaster distribution.
 * Based on esp32-xbee (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_log.h>
#include <string.h>
#include <protocol/nmea.h>
#include <stream_stats.h>

#include "uart.h"
#include "config.h"
#include "tasks.h"
#include "ably_rtk.h"

static const char *TAG = "UART";

ESP_EVENT_DEFINE_BASE(UART_EVENT_READ);
ESP_EVENT_DEFINE_BASE(UART_EVENT_WRITE);

void uart_register_read_handler(esp_event_handler_t event_handler) {
    ESP_ERROR_CHECK(esp_event_handler_register(UART_EVENT_READ, ESP_EVENT_ANY_ID, event_handler, NULL));
}

void uart_unregister_read_handler(esp_event_handler_t event_handler) {
    ESP_ERROR_CHECK(esp_event_handler_unregister(UART_EVENT_READ, ESP_EVENT_ANY_ID, event_handler));
}

void uart_register_write_handler(esp_event_handler_t event_handler) {
    ESP_ERROR_CHECK(esp_event_handler_register(UART_EVENT_WRITE, ESP_EVENT_ANY_ID, event_handler, NULL));
}

void uart_unregister_write_handler(esp_event_handler_t event_handler) {
    ESP_ERROR_CHECK(esp_event_handler_unregister(UART_EVENT_WRITE, ESP_EVENT_ANY_ID, event_handler));
}

static int uart_port = -1;
static bool uart_log_forward = false;

static stream_stats_handle_t stream_stats;

/* RTCM3 parser state */
#define RTCM3_PREAMBLE 0xD3
#define RTCM3_MAX_FRAME_SIZE 1029
static uint8_t rtcm3_buffer[RTCM3_MAX_FRAME_SIZE];
static size_t rtcm3_buffer_len = 0;

static void uart_task(void *ctx);

/**
 * Parse RTCM3 frames from UART data
 * RTCM3 frame format:
 * - Preamble: 0xD3 (1 byte)
 * - Reserved + Length: 2 bytes (6 bits reserved + 10 bits length)
 * - Message type: 12 bits (first 1.5 bytes of payload)
 * - Payload: variable length
 * - CRC24: 3 bytes
 */
static void parse_rtcm3_frames(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        // Add byte to buffer
        if (rtcm3_buffer_len < RTCM3_MAX_FRAME_SIZE) {
            rtcm3_buffer[rtcm3_buffer_len++] = data[i];
        } else {
            // Buffer overflow, reset and look for new preamble
            ESP_LOGW(TAG, "RTCM3 buffer overflow, resetting");
            rtcm3_buffer_len = 0;
            if (data[i] == RTCM3_PREAMBLE) {
                rtcm3_buffer[rtcm3_buffer_len++] = data[i];
            }
            continue;
        }

        // Check if we have at least preamble + length (3 bytes)
        if (rtcm3_buffer_len < 3) {
            // Check if first byte is preamble
            if (rtcm3_buffer_len == 1 && rtcm3_buffer[0] != RTCM3_PREAMBLE) {
                // Not a valid start, discard
                rtcm3_buffer_len = 0;
            }
            continue;
        }

        // Extract message length from bytes 1-2
        // Format: 6 bits reserved + 10 bits length
        uint16_t msg_len = ((rtcm3_buffer[1] & 0x03) << 8) | rtcm3_buffer[2];

        // Total frame length = preamble (1) + header (2) + payload (msg_len) + CRC24 (3)
        uint16_t frame_len = 1 + 2 + msg_len + 3;

        // Check for reasonable length
        if (frame_len > RTCM3_MAX_FRAME_SIZE) {
            ESP_LOGW(TAG, "Invalid RTCM3 frame length: %d, resetting", frame_len);
            rtcm3_buffer_len = 0;
            continue;
        }

        // Check if we have complete frame
        if (rtcm3_buffer_len < frame_len) {
            // Need more data
            continue;
        }

        // We have a complete frame!
        // Extract message type from first 12 bits of payload
        uint16_t msg_type = (rtcm3_buffer[3] << 4) | ((rtcm3_buffer[4] >> 4) & 0x0F);

        ESP_LOGD(TAG, "RTCM3 frame: type=%d, len=%d bytes", msg_type, frame_len);

        // Pass to RTKPubcaster batching system
        esp_err_t err = ably_rtk_batch_add(rtcm3_buffer, frame_len, msg_type);
        if (err == ESP_ERR_NO_MEM) {
            // Batch full, publish it
            ESP_LOGD(TAG, "Batch full, publishing before adding new message");
            ably_rtk_batch_publish(true);

            // Retry adding this message
            err = ably_rtk_batch_add(rtcm3_buffer, frame_len, msg_type);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to add RTCM3 message to batch: %s", esp_err_to_name(err));
            }
        }

        // Check if batch should be published (timeout or full)
        if (ably_rtk_batch_should_publish()) {
            ably_rtk_batch_publish(false);
        }

        // Remove processed frame from buffer
        if (rtcm3_buffer_len > frame_len) {
            // Move remaining data to start of buffer
            memmove(rtcm3_buffer, rtcm3_buffer + frame_len, rtcm3_buffer_len - frame_len);
            rtcm3_buffer_len -= frame_len;
            // Reprocess from start since we might have another frame
            i = (size_t)-1;  // Will be incremented to 0 in next iteration
        } else {
            rtcm3_buffer_len = 0;
        }
    }
}

void uart_init() {
    uart_log_forward = config_get_bool1(CONF_ITEM(KEY_CONFIG_UART_LOG_FORWARD));

    uart_port = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_NUM));

    uart_hw_flowcontrol_t flow_ctrl;
    bool flow_ctrl_rts = config_get_bool1(CONF_ITEM(KEY_CONFIG_UART_FLOW_CTRL_RTS));
    bool flow_ctrl_cts = config_get_bool1(CONF_ITEM(KEY_CONFIG_UART_FLOW_CTRL_CTS));
    if (flow_ctrl_rts && flow_ctrl_cts) {
        flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
    } else if (flow_ctrl_rts) {
        flow_ctrl = UART_HW_FLOWCTRL_RTS;
    } else if (flow_ctrl_cts) {
        flow_ctrl = UART_HW_FLOWCTRL_CTS;
    } else {
        flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    }

    uart_config_t uart_config = {
            .baud_rate = config_get_u32(CONF_ITEM(KEY_CONFIG_UART_BAUD_RATE)),
            .data_bits = config_get_i8(CONF_ITEM(KEY_CONFIG_UART_DATA_BITS)),
            .parity = config_get_i8(CONF_ITEM(KEY_CONFIG_UART_PARITY)),
            .stop_bits = config_get_i8(CONF_ITEM(KEY_CONFIG_UART_STOP_BITS)),
            .flow_ctrl = flow_ctrl
    };
    ESP_ERROR_CHECK(uart_param_config(uart_port, &uart_config));

    // Handle UART_PIN_NO_CHANGE which wraps to 255 when stored as uint8
    uint8_t tx_pin = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_TX_PIN));
    uint8_t rx_pin = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_RX_PIN));
    uint8_t rts_pin = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_RTS_PIN));
    uint8_t cts_pin = config_get_u8(CONF_ITEM(KEY_CONFIG_UART_CTS_PIN));

    ESP_ERROR_CHECK(uart_set_pin(
            uart_port,
            (tx_pin == 255) ? UART_PIN_NO_CHANGE : tx_pin,
            (rx_pin == 255) ? UART_PIN_NO_CHANGE : rx_pin,
            (rts_pin == 255) ? UART_PIN_NO_CHANGE : rts_pin,
            (cts_pin == 255) ? UART_PIN_NO_CHANGE : cts_pin
    ));
    ESP_ERROR_CHECK(uart_driver_install(uart_port, UART_BUFFER_SIZE, UART_BUFFER_SIZE, 0, NULL, 0));

    stream_stats = stream_stats_new("uart");

    // Note: UM980 GPS configuration is managed via the RTCM config system
    // Manual NMEA configuration can be done via web UI if needed
    // The GPS retains its configuration after SAVECONFIG, so no need to reconfigure on every boot
    ESP_LOGI(TAG, "UART initialized - GPS configuration managed via RTCM config system");

    xTaskCreate(uart_task, "uart_task", 8192, NULL, TASK_PRIORITY_UART, NULL);
}

static void uart_task(void *ctx) {
    uint8_t buffer[UART_BUFFER_SIZE];
    static uint32_t total_bytes = 0;
    static uint32_t last_log_time = 0;

    while (true) {
        int32_t len = uart_read_bytes(uart_port, buffer, sizeof(buffer), pdMS_TO_TICKS(50));
        if (len < 0) {
            ESP_LOGE(TAG, "Error reading from UART");
        } else if (len == 0) {
            continue;
        }

        total_bytes += len;
        stream_stats_increment(stream_stats, len, 0);

        // Parse RTCM3 frames and add to Ably batch
        parse_rtcm3_frames(buffer, len);

        // Debug logging: Show received data every 5 seconds or on first receive
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (total_bytes <= len || (now - last_log_time) >= 5000) {
            last_log_time = now;

            // Log byte count and hex dump of first 64 bytes
            ESP_LOGI(TAG, "UART RX: %d bytes (total: %u)", len, total_bytes);

            // Show hex dump for debugging
            int hex_len = len > 64 ? 64 : len;
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, hex_len, ESP_LOG_INFO);

            // Show ASCII representation (printable chars only)
            char ascii_buf[65];
            for (int i = 0; i < hex_len && i < 64; i++) {
                ascii_buf[i] = (buffer[i] >= 32 && buffer[i] < 127) ? buffer[i] : '.';
            }
            ascii_buf[hex_len] = '\0';
            ESP_LOGI(TAG, "ASCII: %s", ascii_buf);
        }

        esp_event_post(UART_EVENT_READ, len, &buffer, len, portMAX_DELAY);
    }
}

void uart_inject(void *buf, size_t len) {
    esp_event_post(UART_EVENT_READ, len,  buf, len, portMAX_DELAY);
}

int uart_log(char *buf, size_t len) {
    if (!uart_log_forward) return 0;
    return uart_write(buf, len);
}

int uart_nmea(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    char *nmea;
    nmea_vasprintf(&nmea, fmt, args);
    int l = uart_write(nmea, strlen(nmea));
    free(nmea);

    va_end(args);

    return l;
}

int uart_write(char *buf, size_t len) {
    if (uart_port < 0) return 0;
    if (len == 0) return 0;

    int written = uart_write_bytes(uart_port, buf, len);
    if (written < 0) return written;

    stream_stats_increment(stream_stats, 0, len);

    esp_event_post(UART_EVENT_WRITE, len, buf, len, portMAX_DELAY);

    return written;
}

esp_err_t uart_query_rtcm_config(rtcm_config_t *config, uint32_t timeout_ms) {
    ESP_LOGI(TAG, "Querying GPS for RTCM configuration...");

    // TODO: Implement LOG command response parsing
    // This requires:
    // 1. Temporarily intercept UART data instead of normal processing
    // 2. Send "LOG\r\n" command
    // 3. Collect response lines (starts with #LOGLISTB)
    // 4. Parse each line to extract message type, COM port, and rate
    // 5. Populate config structure
    // 6. Restore normal UART processing
    //
    // UM980 LOG response format:
    // #LOGLISTB,COM1,0,68.0,FINESTEERING,2362,328416.00,02000000,cdba,16809;109,1,<msg_type>*<checksum>
    //
    // For now, return not supported - user can configure manually via web UI

    ESP_LOGW(TAG, "GPS auto-detection not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}
