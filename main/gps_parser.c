/*
 * GPS NMEA Parser for UM980
 */

#include <string.h>
#include <stdlib.h>
#include <esp_log.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "gps_parser.h"
#include "uart.h"

static const char *TAG = "GPS_PARSER";

static gps_status_t gps_status = {0};
static SemaphoreHandle_t gps_mutex = NULL;

// Helper: Convert NMEA lat/lon format to decimal degrees
static float nmea_to_decimal(const char *nmea_coord, char direction) {
    if (!nmea_coord || strlen(nmea_coord) == 0) return 0.0f;

    // NMEA format: DDMM.MMMM (lat) or DDDMM.MMMM (lon)
    float coord = atof(nmea_coord);
    int degrees = (int)(coord / 100);
    float minutes = coord - (degrees * 100);
    float decimal = degrees + (minutes / 60.0f);

    // Apply direction (S and W are negative)
    if (direction == 'S' || direction == 'W') {
        decimal = -decimal;
    }

    return decimal;
}

// Parse $GNGGA or $GPGGA sentence
static void parse_gga(const char *sentence) {
    // $GNGGA,015736.00,,,,,0,00,9999.0,,,,,,*4E
    // Fields: time, lat, N/S, lon, E/W, quality, sats, hdop, alt, M, ...

    char *fields[15];
    char buffer[256];
    strncpy(buffer, sentence, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    // Tokenize by comma
    int field_count = 0;
    char *token = strtok(buffer, ",");
    while (token != NULL && field_count < 15) {
        fields[field_count++] = token;
        token = strtok(NULL, ",");
    }

    if (field_count < 10) {
        ESP_LOGD(TAG, "GGA: insufficient fields (%d)", field_count);
        return;
    }

    xSemaphoreTake(gps_mutex, portMAX_DELAY);

    // Time (field 1)
    if (strlen(fields[1]) > 0) {
        strncpy(gps_status.timestamp, fields[1], sizeof(gps_status.timestamp) - 1);
    }

    // Latitude (fields 2, 3)
    if (strlen(fields[2]) > 0 && strlen(fields[3]) > 0) {
        gps_status.latitude = nmea_to_decimal(fields[2], fields[3][0]);
    }

    // Longitude (fields 4, 5)
    if (strlen(fields[4]) > 0 && strlen(fields[5]) > 0) {
        gps_status.longitude = nmea_to_decimal(fields[4], fields[5][0]);
    }

    // Fix quality (field 6)
    if (strlen(fields[6]) > 0) {
        gps_status.fix_quality = atoi(fields[6]);
    }

    // Satellites (field 7)
    if (strlen(fields[7]) > 0) {
        gps_status.satellites_used = atoi(fields[7]);
    }

    // HDOP (field 8)
    if (strlen(fields[8]) > 0) {
        gps_status.hdop = atof(fields[8]);
    }

    // Altitude (field 9)
    if (strlen(fields[9]) > 0) {
        gps_status.altitude = atof(fields[9]);
    }

    // Mark as valid if we have a fix
    gps_status.valid = (gps_status.fix_quality > 0);
    gps_status.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    xSemaphoreGive(gps_mutex);

    ESP_LOGD(TAG, "GGA: fix=%d sats=%d lat=%.6f lon=%.6f alt=%.1f",
             gps_status.fix_quality, gps_status.satellites_used,
             gps_status.latitude, gps_status.longitude, gps_status.altitude);
}

// Parse $GNRMC or $GPRMC sentence
static void parse_rmc(const char *sentence) {
    // $GNRMC,015736.00,V,,,,,,,221125,,,N*73
    // Fields: time, status(A/V), lat, N/S, lon, E/W, speed, track, date, ...

    char *fields[13];
    char buffer[256];
    strncpy(buffer, sentence, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';

    // Tokenize by comma
    int field_count = 0;
    char *token = strtok(buffer, ",");
    while (token != NULL && field_count < 13) {
        fields[field_count++] = token;
        token = strtok(NULL, ",");
    }

    if (field_count < 6) {
        ESP_LOGD(TAG, "RMC: insufficient fields (%d)", field_count);
        return;
    }

    xSemaphoreTake(gps_mutex, portMAX_DELAY);

    // Status (field 2): A=valid, V=invalid
    bool status_valid = (strlen(fields[2]) > 0 && fields[2][0] == 'A');

    if (status_valid) {
        // Time (field 1)
        if (strlen(fields[1]) > 0) {
            strncpy(gps_status.timestamp, fields[1], sizeof(gps_status.timestamp) - 1);
        }

        // Latitude (fields 3, 4)
        if (strlen(fields[3]) > 0 && strlen(fields[4]) > 0) {
            gps_status.latitude = nmea_to_decimal(fields[3], fields[4][0]);
        }

        // Longitude (fields 5, 6)
        if (strlen(fields[5]) > 0 && strlen(fields[6]) > 0) {
            gps_status.longitude = nmea_to_decimal(fields[5], fields[6][0]);
        }

        gps_status.valid = true;
        gps_status.last_update_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }

    xSemaphoreGive(gps_mutex);

    ESP_LOGD(TAG, "RMC: status=%c lat=%.6f lon=%.6f",
             fields[2][0], gps_status.latitude, gps_status.longitude);
}

// UART event handler - called when NMEA data is received
static void gps_uart_handler(void *handler_args, esp_event_base_t base,
                             int32_t length, void *buffer) {
    if (length <= 0) return;

    char *data = (char *)buffer;

    // Look for NMEA sentences (start with $, end with \r\n)
    // Process line by line
    char line[256];
    int line_pos = 0;

    for (int i = 0; i < length; i++) {
        if (data[i] == '\n') {
            line[line_pos] = '\0';

            // Process complete line
            if (line_pos > 0 && line[0] == '$') {
                // Remove \r if present
                if (line_pos > 0 && line[line_pos - 1] == '\r') {
                    line[line_pos - 1] = '\0';
                }

                // Parse based on sentence type
                if (strncmp(line, "$GNGGA", 6) == 0 || strncmp(line, "$GPGGA", 6) == 0) {
                    parse_gga(line);
                } else if (strncmp(line, "$GNRMC", 6) == 0 || strncmp(line, "$GPRMC", 6) == 0) {
                    parse_rmc(line);
                }
            }

            line_pos = 0;
        } else if (line_pos < sizeof(line) - 1) {
            line[line_pos++] = data[i];
        }
    }
}

void gps_parser_init(void) {
    ESP_LOGI(TAG, "Initializing GPS parser");

    // Create mutex for thread-safe access
    gps_mutex = xSemaphoreCreateMutex();
    if (gps_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create GPS mutex");
        return;
    }

    // Initialize GPS status
    memset(&gps_status, 0, sizeof(gps_status));

    // Register UART event handler
    uart_register_read_handler(gps_uart_handler);

    ESP_LOGI(TAG, "GPS parser initialized");
}

void gps_get_status(gps_status_t *status) {
    if (status == NULL) return;

    if (gps_mutex != NULL) {
        xSemaphoreTake(gps_mutex, portMAX_DELAY);
        memcpy(status, &gps_status, sizeof(gps_status_t));
        xSemaphoreGive(gps_mutex);
    } else {
        memcpy(status, &gps_status, sizeof(gps_status_t));
    }
}
