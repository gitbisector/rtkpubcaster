/*
 * RTCM3 Configuration Management
 *
 * Provides data-driven configuration for RTCM3 message output.
 * Configuration is stored in NVS and can be queried/updated via web UI.
 * System queries GPS to detect current configuration and adapts batching logic.
 *
 * Key Features:
 * - Per-message enable/disable control
 * - Configurable update rates (1Hz, 5s, 10s, etc.)
 * - COM port selection
 * - GPS auto-detection via LOG command parsing
 * - NVS persistence
 * - Web API for configuration management
 *
 * Date: 2024-11-23
 */

#ifndef RTCM_CONFIG_H
#define RTCM_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>

/* Maximum number of RTCM3 message types we can configure */
#define RTCM_CONFIG_MAX_MESSAGES 16

/* NVS Storage Keys */
#define NVS_NAMESPACE_RTCM "rtcm_cfg"
#define NVS_KEY_RTCM_COUNT "msg_count"
#define NVS_KEY_RTCM_PREFIX "msg_"  // Followed by index (e.g., "msg_0", "msg_1")

/* COM port enumeration for UM980 */
typedef enum {
    COM_PORT_COM1 = 1,
    COM_PORT_COM2 = 2,
    COM_PORT_COM3 = 3,
    COM_PORT_USB = 4
} rtcm_com_port_t;

/* Update rate in seconds (supports fractional values for high-frequency updates) */
typedef float rtcm_update_rate_t;

/**
 * Single RTCM3 message configuration entry
 */
typedef struct {
    uint16_t message_type;          // RTCM3 message type (e.g., 1074)
    bool enabled;                   // Whether this message is configured
    rtcm_com_port_t com_port;       // Output COM port
    rtcm_update_rate_t rate;        // Update rate in seconds
    char description[48];           // Human-readable description
} rtcm_message_config_t;

/**
 * Complete RTCM3 configuration
 */
typedef struct {
    uint8_t message_count;                              // Number of configured messages
    rtcm_message_config_t messages[RTCM_CONFIG_MAX_MESSAGES];
} rtcm_config_t;

/**
 * RTCM3 message tracking for batching
 * Used to track which messages we're expecting in each cycle
 */
typedef struct {
    uint16_t message_type;          // RTCM3 message type
    rtcm_update_rate_t rate;        // Expected rate
    uint64_t last_seen_ms;          // Last time this message was seen (esp_timer)
    bool received_this_cycle;       // Whether we've seen this message in current batch cycle
} rtcm_message_tracker_t;

/* Function Prototypes */

/**
 * Initialize RTCM configuration system
 * Loads configuration from NVS or creates default config
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_init(void);

/**
 * Get current RTCM configuration
 *
 * @param config Pointer to config structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_get(rtcm_config_t *config);

/**
 * Set RTCM configuration
 * Validates and saves to NVS
 *
 * @param config Pointer to new configuration
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_set(const rtcm_config_t *config);

/**
 * Apply configuration to GPS
 * Sends CONFIG and RTCM commands to UM980
 *
 * @param config Pointer to configuration to apply
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_apply(const rtcm_config_t *config);

/**
 * Query GPS for current RTCM configuration
 * Sends LOG command and parses response
 *
 * @param config Pointer to config structure to fill with detected settings
 * @param timeout_ms Timeout in milliseconds to wait for GPS response
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_detect_from_gps(rtcm_config_t *config, uint32_t timeout_ms);

/**
 * Get default RTCM configuration
 * Returns a sensible default config for BASE station operation
 *
 * @param config Pointer to config structure to fill
 */
void rtcm_config_get_default(rtcm_config_t *config);

/**
 * Add a message to configuration
 *
 * @param config Pointer to configuration
 * @param message_type RTCM3 message type
 * @param com_port COM port to output on
 * @param rate Update rate
 * @param description Human-readable description
 * @return ESP_OK on success, ESP_ERR_NO_MEM if config full
 */
esp_err_t rtcm_config_add_message(
    rtcm_config_t *config,
    uint16_t message_type,
    rtcm_com_port_t com_port,
    rtcm_update_rate_t rate,
    const char *description
);

/**
 * Remove a message from configuration
 *
 * @param config Pointer to configuration
 * @param message_type RTCM3 message type to remove
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t rtcm_config_remove_message(
    rtcm_config_t *config,
    uint16_t message_type
);

/**
 * Find a message in configuration
 *
 * @param config Pointer to configuration
 * @param message_type RTCM3 message type to find
 * @return Pointer to message config, or NULL if not found
 */
rtcm_message_config_t* rtcm_config_find_message(
    rtcm_config_t *config,
    uint16_t message_type
);

/**
 * Serialize configuration to JSON
 *
 * @param config Pointer to configuration
 * @param json_out Output buffer for JSON string
 * @param json_len Size of output buffer
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_serialize_json(
    const rtcm_config_t *config,
    char *json_out,
    size_t json_len
);

/**
 * Deserialize configuration from JSON
 *
 * @param json_str Input JSON string
 * @param config Pointer to config structure to fill
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_deserialize_json(
    const char *json_str,
    rtcm_config_t *config
);

/**
 * Validate configuration
 * Checks for duplicate message types, invalid rates, etc.
 *
 * @param config Pointer to configuration to validate
 * @return ESP_OK if valid, error code otherwise
 */
esp_err_t rtcm_config_validate(const rtcm_config_t *config);

/**
 * Get list of messages expected at a given rate
 * Used by batching logic to determine when to publish
 *
 * @param config Pointer to configuration
 * @param rate Update rate to filter by
 * @param types Output array for message types
 * @param max_types Size of output array
 * @param count Output: number of messages at this rate
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t rtcm_config_get_messages_at_rate(
    const rtcm_config_t *config,
    rtcm_update_rate_t rate,
    uint16_t *types,
    size_t max_types,
    size_t *count
);

/**
 * Check if we have received a complete set of messages for a given rate
 *
 * @param trackers Array of message trackers
 * @param tracker_count Number of trackers
 * @param rate Update rate to check
 * @return true if complete set received, false otherwise
 */
bool rtcm_config_is_complete_set(
    const rtcm_message_tracker_t *trackers,
    size_t tracker_count,
    rtcm_update_rate_t rate
);

/**
 * Reset "received this cycle" flags for all trackers at a given rate
 *
 * @param trackers Array of message trackers
 * @param tracker_count Number of trackers
 * @param rate Update rate to reset
 */
void rtcm_config_reset_cycle(
    rtcm_message_tracker_t *trackers,
    size_t tracker_count,
    rtcm_update_rate_t rate
);

/**
 * Get human-readable description for RTCM3 message type
 *
 * @param message_type RTCM3 message type
 * @return Description string (static, do not free)
 */
const char* rtcm_config_get_message_description(uint16_t message_type);

#endif // RTCM_CONFIG_H
