/*
 * GPS NMEA Parser for UM980
 * Parses NMEA sentences and tracks GPS status
 */

#ifndef GPS_PARSER_H
#define GPS_PARSER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float latitude;           // Decimal degrees (positive = North, negative = South)
    float longitude;          // Decimal degrees (positive = East, negative = West)
    uint8_t fix_quality;      // 0=no fix, 1=GPS, 2=DGPS, 4=RTK fixed, 5=RTK float
    uint8_t satellites_used;  // Number of satellites in use
    float hdop;               // Horizontal dilution of precision
    float altitude;           // Altitude in meters
    char timestamp[16];       // Time from GPS (HHMMSS.SS)
    bool valid;               // True if we have valid GPS data
    uint32_t last_update_ms;  // Timestamp of last update (milliseconds)
} gps_status_t;

/**
 * Initialize GPS parser and register UART event handler
 */
void gps_parser_init(void);

/**
 * Get current GPS status
 * @param status Pointer to gps_status_t structure to fill
 */
void gps_get_status(gps_status_t *status);

#endif // GPS_PARSER_H
