/*
 * Ably MQTT Client Wrapper Implementation
 *
 * This file is part of the ESP32 RTKPubcaster distribution.
 */

#include "ably_client.h"
#include "ably_credentials.h"

#include <string.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_mac.h>
#include <mqtt_client.h>
#include <cJSON.h>

static const char *TAG = "ably_client";

/* Embedded ISRG Root X1 certificate (Let's Encrypt root CA) */
extern const uint8_t isrg_root_x1_pem_start[] asm("_binary_isrg_root_x1_pem_start");
extern const uint8_t isrg_root_x1_pem_end[]   asm("_binary_isrg_root_x1_pem_end");

/* Client state */
static struct {
    esp_mqtt_client_handle_t mqtt_client;
    ably_client_state_t state;
    char api_key[128];
    char channel_name[64];
    char client_id[32];
    ably_client_event_cb_t event_callback;
    bool presence_entered;
    uint32_t subscriber_count;  // Number of active subscribers on the channel
} s_client = {
    .mqtt_client = NULL,
    .state = ABLY_STATE_DISCONNECTED,
    .event_callback = NULL,
    .presence_entered = false,
    .subscriber_count = 0
};

/* Forward declarations */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void extract_api_key_parts(const char *api_key, char *key_name, char *key_secret);

esp_err_t ably_client_init(const char *api_key, const char *channel_name, ably_client_event_cb_t event_callback) {
    if (api_key == NULL || channel_name == NULL) {
        ESP_LOGE(TAG, "API key and channel name are required");
        return ESP_ERR_INVALID_ARG;
    }

    // Store configuration
    strncpy(s_client.api_key, api_key, sizeof(s_client.api_key) - 1);
    strncpy(s_client.channel_name, channel_name, sizeof(s_client.channel_name) - 1);
    s_client.event_callback = event_callback;

    // Generate client ID from MAC address
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client.client_id, sizeof(s_client.client_id),
             "esp32_%02x%02x%02x", mac[3], mac[4], mac[5]);

    // Extract API key parts
    char key_name[64];
    char key_secret[64];
    extract_api_key_parts(api_key, key_name, key_secret);

    // Configure MQTT client
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtts://mqtt.ably.io:8883",
        .broker.verification.certificate = (const char *)isrg_root_x1_pem_start,
        .credentials.username = key_name,
        .credentials.authentication.password = key_secret,
        .credentials.client_id = s_client.client_id,
        .session.keepalive = ABLY_MQTT_KEEPALIVE,
        .network.disable_auto_reconnect = false,
        .session.last_will = {
            .topic = NULL,  // Will be set if needed
            .msg = NULL,
            .qos = 0,
            .retain = 0
        }
    };

    s_client.mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client.mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    // Register event handler
    esp_mqtt_client_register_event(s_client.mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    ESP_LOGI(TAG, "Ably client initialized (client_id: %s, channel: %s)",
             s_client.client_id, s_client.channel_name);

    return ESP_OK;
}

esp_err_t ably_client_start(void) {
    if (s_client.mqtt_client == NULL) {
        ESP_LOGE(TAG, "Client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_client.state = ABLY_STATE_CONNECTING;
    esp_err_t err = esp_mqtt_client_start(s_client.mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        s_client.state = ABLY_STATE_ERROR;
        return err;
    }

    ESP_LOGI(TAG, "Ably client starting, connecting to mqtt.ably.io:8883");
    return ESP_OK;
}

esp_err_t ably_client_stop(void) {
    if (s_client.mqtt_client == NULL) {
        return ESP_OK;
    }

    // Leave presence if entered
    if (s_client.presence_entered) {
        ably_client_presence_leave();
    }

    esp_err_t err = esp_mqtt_client_stop(s_client.mqtt_client);
    if (err == ESP_OK) {
        s_client.state = ABLY_STATE_DISCONNECTED;
    }

    return err;
}

esp_err_t ably_client_publish(const char *data, size_t len, int qos) {
    if (s_client.mqtt_client == NULL) {
        ESP_LOGE(TAG, "Client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_client.state != ABLY_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Client not connected, cannot publish");
        return ESP_ERR_INVALID_STATE;
    }

    // Ably MQTT topic format: [channel_name]
    int msg_id = esp_mqtt_client_publish(s_client.mqtt_client,
                                         s_client.channel_name,
                                         data,
                                         len,
                                         qos,
                                         0);  // retain = 0

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish message");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Published %zu bytes to %s (msg_id: %d)", len, s_client.channel_name, msg_id);
    return ESP_OK;
}

esp_err_t ably_client_presence_enter(const char *presence_data) {
    if (s_client.mqtt_client == NULL) {
        ESP_LOGE(TAG, "Client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_client.state != ABLY_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Client not connected, cannot enter presence");
        return ESP_ERR_INVALID_STATE;
    }

    // Ably MQTT presence topic format: [channel_name]/presence
    char presence_topic[96];
    snprintf(presence_topic, sizeof(presence_topic), "%s/presence", s_client.channel_name);

    // Presence message format: JSON with "action": "enter"
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "enter");
    cJSON_AddStringToObject(root, "clientId", s_client.client_id);

    if (presence_data != NULL) {
        cJSON *data_obj = cJSON_Parse(presence_data);
        if (data_obj != NULL) {
            cJSON_AddItemToObject(root, "data", data_obj);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    int msg_id = esp_mqtt_client_publish(s_client.mqtt_client,
                                         presence_topic,
                                         json_str,
                                         strlen(json_str),
                                         1,  // QoS 1 for presence
                                         0);  // retain = 0

    cJSON_Delete(root);
    free(json_str);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to enter presence");
        return ESP_FAIL;
    }

    s_client.presence_entered = true;
    ESP_LOGI(TAG, "Entered presence on channel %s", s_client.channel_name);
    return ESP_OK;
}

esp_err_t ably_client_presence_update(const char *presence_data) {
    if (s_client.mqtt_client == NULL) {
        ESP_LOGE(TAG, "Client not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_client.state != ABLY_STATE_CONNECTED) {
        ESP_LOGW(TAG, "Client not connected, cannot update presence");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_client.presence_entered) {
        ESP_LOGW(TAG, "Presence not entered, call enter first");
        return ESP_ERR_INVALID_STATE;
    }

    // Ably MQTT presence topic format: [channel_name]/presence
    char presence_topic[96];
    snprintf(presence_topic, sizeof(presence_topic), "%s/presence", s_client.channel_name);

    // Presence message format: JSON with "action": "update"
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "update");
    cJSON_AddStringToObject(root, "clientId", s_client.client_id);

    if (presence_data != NULL) {
        cJSON *data_obj = cJSON_Parse(presence_data);
        if (data_obj != NULL) {
            cJSON_AddItemToObject(root, "data", data_obj);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    int msg_id = esp_mqtt_client_publish(s_client.mqtt_client,
                                         presence_topic,
                                         json_str,
                                         strlen(json_str),
                                         1,  // QoS 1 for presence
                                         0);  // retain = 0

    cJSON_Delete(root);
    free(json_str);

    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to update presence");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Updated presence on channel %s", s_client.channel_name);
    return ESP_OK;
}

esp_err_t ably_client_presence_leave(void) {
    if (s_client.mqtt_client == NULL) {
        return ESP_OK;
    }

    if (!s_client.presence_entered) {
        return ESP_OK;
    }

    // Ably MQTT presence topic format: [channel_name]/presence
    char presence_topic[96];
    snprintf(presence_topic, sizeof(presence_topic), "%s/presence", s_client.channel_name);

    // Presence message format: JSON with "action": "leave"
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "leave");
    cJSON_AddStringToObject(root, "clientId", s_client.client_id);

    char *json_str = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(s_client.mqtt_client,
                           presence_topic,
                           json_str,
                           strlen(json_str),
                           1,  // QoS 1 for presence
                           0);  // retain = 0

    cJSON_Delete(root);
    free(json_str);

    s_client.presence_entered = false;
    ESP_LOGI(TAG, "Left presence on channel %s", s_client.channel_name);
    return ESP_OK;
}

uint32_t ably_client_get_subscriber_count(void) {
    return s_client.subscriber_count;
}

esp_err_t ably_client_check_occupancy(uint32_t *subscriber_count) {
    if (subscriber_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Return current tracked count (will be updated if REST call succeeds)
    *subscriber_count = s_client.subscriber_count;

    // Extract API key parts for HTTP basic auth
    char key_name[64];
    char key_secret[64];
    extract_api_key_parts(s_client.api_key, key_name, key_secret);

    // Build REST API URL
    char url[256];
    snprintf(url, sizeof(url), "https://rest.ably.io/channels/%s/status", s_client.channel_name);

    // Configure HTTP client
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .username = key_name,
        .password = key_secret,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            int content_length = esp_http_client_get_content_length(client);
            if (content_length > 0 && content_length < 4096) {
                char *buffer = malloc(content_length + 1);
                if (buffer != NULL) {
                    int read_len = esp_http_client_read(client, buffer, content_length);
                    buffer[read_len] = '\0';

                    // Parse JSON response
                    cJSON *root = cJSON_Parse(buffer);
                    if (root != NULL) {
                        cJSON *occupancy = cJSON_GetObjectItem(root, "occupancy");
                        if (occupancy != NULL) {
                            cJSON *metrics = cJSON_GetObjectItem(occupancy, "metrics");
                            if (metrics != NULL) {
                                cJSON *subscribers = cJSON_GetObjectItem(metrics, "subscribers");
                                if (cJSON_IsNumber(subscribers)) {
                                    uint32_t new_count = (uint32_t)subscribers->valueint;
                                    *subscriber_count = new_count;
                                    s_client.subscriber_count = new_count;  // Update internal state
                                    ESP_LOGI(TAG, "Channel occupancy: %lu subscribers", new_count);
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                    free(buffer);
                }
            }
        } else {
            ESP_LOGW(TAG, "Channel status query returned %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

ably_client_state_t ably_client_get_state(void) {
    return s_client.state;
}

bool ably_client_is_connected(void) {
    return s_client.state == ABLY_STATE_CONNECTED;
}

esp_mqtt_client_handle_t ably_client_get_mqtt_handle(void) {
    return s_client.mqtt_client;
}

/* Private functions */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to Ably MQTT broker");
            s_client.state = ABLY_STATE_CONNECTED;

            // Subscribe to channel with occupancy events enabled
            // Ably MQTT format: [?occupancy=metrics]channel-name
            // This will deliver [meta]occupancy messages on this channel
            char occupancy_topic[128];
            snprintf(occupancy_topic, sizeof(occupancy_topic), "[?occupancy=metrics]%s", s_client.channel_name);
            int msg_id = esp_mqtt_client_subscribe(s_client.mqtt_client, occupancy_topic, 0);
            if (msg_id >= 0) {
                ESP_LOGI(TAG, "Subscribed to %s for occupancy events (msg_id: %d)", occupancy_topic, msg_id);
            } else {
                ESP_LOGW(TAG, "Failed to subscribe to occupancy events");
            }

            // Optimistically assume at least 1 subscriber when connected
            s_client.subscriber_count = 1;

            // Notify callback
            if (s_client.event_callback != NULL) {
                s_client.event_callback(ABLY_EVENT_CONNECTED, NULL);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected from Ably MQTT broker");
            s_client.state = ABLY_STATE_DISCONNECTED;
            s_client.presence_entered = false;
            s_client.subscriber_count = 0;  // Reset on disconnect

            // Notify callback
            if (s_client.event_callback != NULL) {
                s_client.event_callback(ABLY_EVENT_DISCONNECTED, NULL);
            }
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "Message published (msg_id: %d)", event->msg_id);

            // Successful publish confirms at least 1 subscriber
            if (s_client.subscriber_count == 0) {
                s_client.subscriber_count = 1;
                ESP_LOGI(TAG, "Subscriber detected via successful publish");
            }

            // Notify callback
            if (s_client.event_callback != NULL) {
                s_client.event_callback(ABLY_EVENT_PUBLISHED, &event->msg_id);
            }
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED (msg_id: %d)", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGW(TAG, "MQTT_EVENT_UNSUBSCRIBED (msg_id: %d)", event->msg_id);
            break;

        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error occurred: type=%d, connect_return_code=%d",
                    event->error_handle ? event->error_handle->error_type : -1,
                    event->error_handle ? event->error_handle->connect_return_code : -1);
            if (event->error_handle) {
                if (event->error_handle->esp_tls_last_esp_err != 0) {
                    ESP_LOGE(TAG, "TLS error: 0x%x", event->error_handle->esp_tls_last_esp_err);
                }
                if (event->error_handle->esp_transport_sock_errno != 0) {
                    ESP_LOGE(TAG, "Socket errno: %d", event->error_handle->esp_transport_sock_errno);
                }
            }
            s_client.state = ABLY_STATE_ERROR;

            // Notify callback
            if (s_client.event_callback != NULL) {
                s_client.event_callback(ABLY_EVENT_ERROR, event);
            }
            break;

        case MQTT_EVENT_DATA:
            // Handle incoming messages (occupancy updates, presence notifications, etc.)
            ESP_LOGD(TAG, "Received message on topic: %.*s (len=%d)", event->topic_len, event->topic, event->data_len);

            // Check if this is from our occupancy-enabled channel subscription
            // Messages on [?occupancy=metrics]channel will contain occupancy data
            if (event->data_len > 0) {
                // Parse occupancy message - format is: {"metrics":{...}}
                cJSON *root = cJSON_ParseWithLength(event->data, event->data_len);
                if (root != NULL) {
                    // Check if this has metrics field (occupancy message)
                    cJSON *metrics = cJSON_GetObjectItem(root, "metrics");
                    if (metrics) {
                        cJSON *subscribers = cJSON_GetObjectItem(metrics, "subscribers");
                        if (cJSON_IsNumber(subscribers)) {
                            uint32_t new_count = (uint32_t)subscribers->valueint;
                            if (new_count != s_client.subscriber_count) {
                                ESP_LOGI(TAG, "Occupancy update: %lu subscribers (was %lu)",
                                        new_count, s_client.subscriber_count);
                                s_client.subscriber_count = new_count;
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
            break;

        default:
            ESP_LOGD(TAG, "MQTT event: %d", event_id);
            break;
    }
}

static void extract_api_key_parts(const char *api_key, char *key_name, char *key_secret) {
    // API key format: "name:secret"
    const char *colon = strchr(api_key, ':');
    if (colon != NULL) {
        size_t name_len = colon - api_key;
        strncpy(key_name, api_key, name_len);
        key_name[name_len] = '\0';
        strcpy(key_secret, colon + 1);
    } else {
        ESP_LOGE(TAG, "Invalid API key format (expected 'name:secret')");
        key_name[0] = '\0';
        key_secret[0] = '\0';
    }
}
