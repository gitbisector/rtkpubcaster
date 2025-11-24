/*
 * RTKPubcaster Message Format Definitions
 *
 * This file is part of the ESP32 RTKPubcaster distribution.
 *
 * Defines structures and constants for streaming RTCM3 corrections
 * from ESP32 base station to mobile clients via Ably MQTT.
 *
 * Message Format Specification:
 * - JSON envelope with base64-encoded RTCM3 binary data
 * - Publishes 1 batch per second (RTK corrections are time-sensitive)
 * - Sequence numbers for message loss detection
 * - Presence channel for base station metadata
 *
 * Protocol Version: 1.0
 * Date: 2024-11-23
 */

#ifndef ABLY_RTK_H
#define ABLY_RTK_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

/* Protocol Configuration */
#define ABLY_RTK_PROTOCOL_VERSION 1
#define ABLY_RTK_MESSAGE_TYPE "rtk_corrections"

/* Buffer Sizes */
#define ABLY_RTK_BASE_ID_MAX_LEN 32
#define ABLY_RTK_BATCH_MAX_MESSAGES 8
#define ABLY_RTK_BATCH_MAX_SIZE 8192       // Max binary size before base64
#define ABLY_RTK_JSON_BUFFER_SIZE 16384    // Must accommodate base64 expansion

/* RTCM3 Message Types (Common) */
#define RTCM3_MSG_1005 1005  // Station ARP (no height)
#define RTCM3_MSG_1006 1006  // Station ARP with height
#define RTCM3_MSG_1074 1074  // GPS MSM4
#define RTCM3_MSG_1084 1084  // GLONASS MSM4
#define RTCM3_MSG_1094 1094  // Galileo MSM4
#define RTCM3_MSG_1124 1124  // BeiDou MSM4
#define RTCM3_MSG_1114 1114  // QZSS MSM4

/* Batching Configuration */
#define ABLY_RTK_BATCH_TIMEOUT_MS 1500     // Timeout fallback if complete set not received
#define ABLY_RTK_BATCH_MIN_INTERVAL_MS 50  // Min time between publishes

/* Expected RTCM3 Messages (1Hz cycle) - Update this if GPS programming changes */
#define ABLY_RTK_EXPECTED_1HZ_MESSAGES { RTCM3_MSG_1074, RTCM3_MSG_1084, RTCM3_MSG_1094, RTCM3_MSG_1124 }
#define ABLY_RTK_EXPECTED_1HZ_COUNT 4

/* Presence Update Configuration */
#define ABLY_RTK_PRESENCE_UPDATE_INTERVAL_S 60  // Update every 60 seconds

/**
 * Single RTCM3 message within a batch
 */
typedef struct {
    uint16_t type;           // RTCM3 message type (e.g., 1074)
    uint16_t size;           // Size of binary data (bytes)
    uint8_t *data;           // Pointer to RTCM3 binary data
} ably_rtk_rtcm_message_t;

/**
 * Batch of RTCM3 messages
 */
typedef struct {
    uint8_t count;                                          // Number of messages in batch
    uint16_t total_size;                                    // Total size of all messages
    ably_rtk_rtcm_message_t messages[ABLY_RTK_BATCH_MAX_MESSAGES];
} ably_rtk_batch_t;

/**
 * Complete correction data message
 */
typedef struct {
    char base_id[ABLY_RTK_BASE_ID_MAX_LEN];  // Base station identifier
    int64_t timestamp;                        // Microseconds (esp_timer_get_time)
    uint32_t sequence;                        // Sequence number
    ably_rtk_batch_t batch;                   // Batch of RTCM3 messages
} ably_rtk_correction_msg_t;

/**
 * GNSS constellation enumeration
 */
typedef enum {
    GNSS_GPS = 0,
    GNSS_GLONASS,
    GNSS_GALILEO,
    GNSS_BEIDOU,
    GNSS_QZSS,
    GNSS_SBAS
} gnss_constellation_t;

/**
 * GNSS fix type enumeration
 */
typedef enum {
    FIX_NO_FIX = 0,
    FIX_2D,
    FIX_3D,
    FIX_RTK_FLOAT,
    FIX_RTK_FIXED
} gnss_fix_type_t;

/**
 * Position accuracy type
 */
typedef enum {
    POS_ACCURACY_SURVEY_IN = 0,
    POS_ACCURACY_MANUAL,
    POS_ACCURACY_AUTONOMOUS
} position_accuracy_t;

/**
 * Base station position
 */
typedef struct {
    double latitude;                  // Decimal degrees (WGS84)
    double longitude;                 // Decimal degrees (WGS84)
    double altitude;                  // Meters above ellipsoid
    position_accuracy_t accuracy;     // Position accuracy type
} ably_rtk_position_t;

/**
 * Antenna information
 */
typedef struct {
    float height;                     // Meters above marker
    char model[32];                   // Antenna model string
} ably_rtk_antenna_t;

/**
 * GNSS status
 */
typedef struct {
    bool constellations[6];           // Enabled constellations (indexed by gnss_constellation_t)
    uint8_t satellites;               // Currently tracked satellites
    gnss_fix_type_t fix_type;        // Current fix type
} ably_rtk_gnss_status_t;

/**
 * Statistics
 */
typedef struct {
    uint64_t messages_sent;           // Total messages sent since boot
    uint64_t bytes_sent;              // Total bytes sent since boot
    uint32_t avg_latency_ms;         // Average publish latency
} ably_rtk_stats_t;

/**
 * Firmware information
 */
typedef struct {
    char version[16];                 // Semantic version (e.g., "1.0.0")
    char build[16];                   // Build identifier
} ably_rtk_firmware_t;

/**
 * Presence data structure
 */
typedef struct {
    char status[16];                  // "online", "degraded", "offline"
    uint32_t uptime;                  // Seconds since boot
    ably_rtk_position_t position;
    ably_rtk_antenna_t antenna;
    ably_rtk_gnss_status_t gnss;
    ably_rtk_stats_t stats;
    ably_rtk_firmware_t firmware;
} ably_rtk_presence_t;

/* Function Prototypes */

/**
 * Initialize the RTKPubcaster messaging system
 *
 * @param base_id Base station identifier (e.g., "base1")
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_rtk_init(const char *base_id);

/**
 * Add an RTCM3 message to the current batch
 *
 * @param rtcm_data Pointer to RTCM3 binary data
 * @param size Size of RTCM3 data in bytes
 * @param type RTCM3 message type (e.g., 1074)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if batch is full
 */
esp_err_t ably_rtk_batch_add(const uint8_t *rtcm_data, uint16_t size, uint16_t type);

/**
 * Publish the current batch to Ably
 *
 * @param force If true, publish even if batch is not full
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_rtk_batch_publish(bool force);

/**
 * Serialize correction message to JSON
 *
 * @param msg Pointer to correction message structure
 * @param json_out Output buffer for JSON string
 * @param json_len Size of output buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_rtk_serialize_correction(
    const ably_rtk_correction_msg_t *msg,
    char *json_out,
    size_t json_len
);

/**
 * Serialize presence data to JSON
 *
 * @param presence Pointer to presence data structure
 * @param json_out Output buffer for JSON string
 * @param json_len Size of output buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_rtk_serialize_presence(
    const ably_rtk_presence_t *presence,
    char *json_out,
    size_t json_len
);

/**
 * Update presence data on Ably channel
 *
 * @param presence Pointer to presence data structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_rtk_update_presence(const ably_rtk_presence_t *presence);

/**
 * Base64 encode RTCM3 data using mbedtls
 *
 * @param input Binary input data
 * @param input_len Length of input data
 * @param output Output buffer for base64
 * @param output_len Size of output buffer
 * @param encoded_len Actual length of encoded data (output)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_rtk_base64_encode(
    const uint8_t *input,
    size_t input_len,
    char *output,
    size_t output_len,
    size_t *encoded_len
);

/**
 * Get current statistics
 *
 * @param stats Pointer to stats structure to fill
 */
void ably_rtk_get_stats(ably_rtk_stats_t *stats);

/**
 * Check if batch should be published (timeout or full)
 *
 * @return true if batch should be published, false otherwise
 */
bool ably_rtk_batch_should_publish(void);

/**
 * Get current batch size
 *
 * @return Number of messages in current batch
 */
uint8_t ably_rtk_batch_get_count(void);

/**
 * Update expected RTCM3 messages from configuration
 * Sets up dynamic batching based on configuration
 *
 * @param config Pointer to RTCM configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_rtk_set_expected_messages(const void *config);

#endif // ABLY_RTK_H
