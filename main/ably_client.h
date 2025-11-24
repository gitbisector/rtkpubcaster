/*
 * Ably MQTT Client Wrapper
 *
 * This file is part of the ESP32 RTKPubcaster distribution.
 *
 * Provides a simplified interface for connecting to Ably via MQTT,
 * managing presence, and checking channel occupancy.
 *
 * Connection: mqtt.ably.io:8883 (MQTT over TLS)
 * Authentication: API key (username = key name, password = key secret)
 */

#ifndef ABLY_CLIENT_H
#define ABLY_CLIENT_H

#include <esp_err.h>
#include <mqtt_client.h>

/**
 * Ably client connection state
 */
typedef enum {
    ABLY_STATE_DISCONNECTED = 0,
    ABLY_STATE_CONNECTING,
    ABLY_STATE_CONNECTED,
    ABLY_STATE_ERROR
} ably_client_state_t;

/**
 * Ably client event types
 */
typedef enum {
    ABLY_EVENT_CONNECTED = 0,
    ABLY_EVENT_DISCONNECTED,
    ABLY_EVENT_PUBLISHED,
    ABLY_EVENT_ERROR,
    ABLY_EVENT_SUBSCRIBER_JOINED,
    ABLY_EVENT_SUBSCRIBER_LEFT
} ably_client_event_t;

/**
 * Ably client event callback
 *
 * @param event Event type
 * @param data Event-specific data (can be NULL)
 */
typedef void (*ably_client_event_cb_t)(ably_client_event_t event, void *data);

/**
 * Initialize Ably MQTT client
 *
 * @param api_key Ably API key (format: "name:secret")
 * @param channel_name Channel to publish/subscribe
 * @param event_callback Optional event callback (can be NULL)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_init(const char *api_key, const char *channel_name, ably_client_event_cb_t event_callback);

/**
 * Start Ably client (connect to broker)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_start(void);

/**
 * Stop Ably client (disconnect from broker)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_stop(void);

/**
 * Publish data to configured channel
 *
 * @param data Data to publish
 * @param len Length of data
 * @param qos QoS level (0, 1, or 2)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_publish(const char *data, size_t len, int qos);

/**
 * Enter presence on the channel
 *
 * @param presence_data JSON string with presence data (can be NULL)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_presence_enter(const char *presence_data);

/**
 * Update presence data
 *
 * @param presence_data JSON string with presence data
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_presence_update(const char *presence_data);

/**
 * Leave presence (called automatically on disconnect)
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_presence_leave(void);

/**
 * Get current subscriber count (cached/estimated)
 * Uses optimistic tracking based on successful publishes
 *
 * @return Number of subscribers (0 if unknown/none)
 */
uint32_t ably_client_get_subscriber_count(void);

/**
 * Check channel occupancy (number of subscribers)
 * Uses Ably REST API to query channel status
 *
 * @param subscriber_count Output parameter for subscriber count
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ably_client_check_occupancy(uint32_t *subscriber_count);

/**
 * Get current connection state
 *
 * @return Current state
 */
ably_client_state_t ably_client_get_state(void);

/**
 * Check if client is connected
 *
 * @return true if connected, false otherwise
 */
bool ably_client_is_connected(void);

/**
 * Get underlying MQTT client handle (for advanced usage)
 *
 * @return MQTT client handle or NULL if not initialized
 */
esp_mqtt_client_handle_t ably_client_get_mqtt_handle(void);

#endif // ABLY_CLIENT_H
