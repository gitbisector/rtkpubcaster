/*
 * RTKPubcaster Message Implementation
 *
 * This file is part of the ESP32 RTKPubcaster distribution.
 */

#include "ably_rtk.h"
#include "ably_client.h"
#include "rtcm_config.h"

#include <string.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <cJSON.h>
#include <mbedtls/base64.h>

static const char *TAG = "ably_rtk";

/* Module state */
static struct {
    bool initialized;
    char base_id[ABLY_RTK_BASE_ID_MAX_LEN];
    uint32_t sequence;
    ably_rtk_stats_t stats;
    SemaphoreHandle_t mutex;

    /* Current batch */
    ably_rtk_batch_t current_batch;
    uint8_t batch_buffers[ABLY_RTK_BATCH_MAX_MESSAGES][ABLY_RTK_BATCH_MAX_SIZE / ABLY_RTK_BATCH_MAX_MESSAGES];
    int64_t batch_start_time;

    /* Dynamic message tracking */
    rtcm_message_tracker_t trackers[RTCM_CONFIG_MAX_MESSAGES];
    size_t tracker_count;

    /* Presence data */
    ably_rtk_presence_t presence;
} s_rtk = {
    .initialized = false,
    .sequence = 0,
    .mutex = NULL,
    .batch_start_time = 0,
    .tracker_count = 0
};

/* Forward declarations */
static esp_err_t serialize_batch(const ably_rtk_correction_msg_t *msg, char *json_out, size_t json_len);
static void add_constellation_names(cJSON *array, const ably_rtk_gnss_status_t *gnss);
static const char *fix_type_to_string(gnss_fix_type_t fix_type);
static const char *accuracy_to_string(position_accuracy_t accuracy);

esp_err_t ably_rtk_init(const char *base_id) {
    if (base_id == NULL) {
        ESP_LOGE(TAG, "Base ID required");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_rtk.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Create mutex for thread safety
    s_rtk.mutex = xSemaphoreCreateMutex();
    if (s_rtk.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    // Store base ID
    strncpy(s_rtk.base_id, base_id, sizeof(s_rtk.base_id) - 1);

    // Initialize batch
    memset(&s_rtk.current_batch, 0, sizeof(s_rtk.current_batch));

    // Initialize statistics
    memset(&s_rtk.stats, 0, sizeof(s_rtk.stats));

    // Initialize presence data
    memset(&s_rtk.presence, 0, sizeof(s_rtk.presence));
    strcpy(s_rtk.presence.status, "online");

    s_rtk.initialized = true;
    ESP_LOGI(TAG, "RTKPubcaster initialized (base_id: %s)", s_rtk.base_id);

    return ESP_OK;
}

esp_err_t ably_rtk_batch_add(const uint8_t *rtcm_data, uint16_t size, uint16_t type) {
    if (!s_rtk.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (rtcm_data == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_rtk.mutex, portMAX_DELAY);

    // Check if batch is full
    if (s_rtk.current_batch.count >= ABLY_RTK_BATCH_MAX_MESSAGES) {
        xSemaphoreGive(s_rtk.mutex);
        ESP_LOGD(TAG, "Batch full, publish first");
        return ESP_ERR_NO_MEM;
    }

    // Check if adding this message would exceed size limit
    if (s_rtk.current_batch.total_size + size > ABLY_RTK_BATCH_MAX_SIZE) {
        xSemaphoreGive(s_rtk.mutex);
        ESP_LOGD(TAG, "Batch size limit reached, publish first");
        return ESP_ERR_NO_MEM;
    }

    // Copy RTCM data to batch buffer
    uint8_t index = s_rtk.current_batch.count;
    memcpy(s_rtk.batch_buffers[index], rtcm_data, size);

    // Add message to batch
    s_rtk.current_batch.messages[index].type = type;
    s_rtk.current_batch.messages[index].size = size;
    s_rtk.current_batch.messages[index].data = s_rtk.batch_buffers[index];

    s_rtk.current_batch.count++;
    s_rtk.current_batch.total_size += size;

    // Record batch start time on first message
    if (s_rtk.current_batch.count == 1) {
        s_rtk.batch_start_time = esp_timer_get_time();
    }

    bool should_publish_now = false;

    // Dynamic batching: Mark this message as received in tracker
    if (s_rtk.tracker_count > 0) {
        uint64_t now_ms = esp_timer_get_time() / 1000;

        // Find and update tracker for this message type
        for (size_t i = 0; i < s_rtk.tracker_count; i++) {
            if (s_rtk.trackers[i].message_type == type) {
                s_rtk.trackers[i].last_seen_ms = now_ms;
                s_rtk.trackers[i].received_this_cycle = true;
                break;
            }
        }

        // Check if we have a complete set for the fastest rate
        // Start with 1Hz (1.0s) as it's most common
        if (rtcm_config_is_complete_set(s_rtk.trackers, s_rtk.tracker_count, 1.0f)) {
            should_publish_now = true;
            ESP_LOGD(TAG, "Complete 1Hz cycle detected, triggering publish");
        }
        // Check for 10s cycle (includes messages like 1005)
        else if (s_rtk.current_batch.count >= 4 &&
                 rtcm_config_is_complete_set(s_rtk.trackers, s_rtk.tracker_count, 10.0f)) {
            should_publish_now = true;
            ESP_LOGD(TAG, "Complete 10s cycle detected, triggering publish");
        }
    } else {
        // Fallback to hardcoded logic if trackers not initialized
        if (type == RTCM3_MSG_1124 && s_rtk.current_batch.count >= 3) {
            should_publish_now = true;
            ESP_LOGD(TAG, "Complete cycle detected (fallback logic), triggering publish");
        }
    }

    xSemaphoreGive(s_rtk.mutex);

    ESP_LOGD(TAG, "Added RTCM%d (%d bytes) to batch (count: %d, total: %d bytes)%s",
             type, size, s_rtk.current_batch.count, s_rtk.current_batch.total_size,
             should_publish_now ? " [CYCLE COMPLETE]" : "");

    return should_publish_now ? ESP_ERR_NO_MEM : ESP_OK;  // Signal caller to publish
}

bool ably_rtk_batch_should_publish(void) {
    if (!s_rtk.initialized) {
        return false;
    }

    xSemaphoreTake(s_rtk.mutex, portMAX_DELAY);

    bool should_publish = false;

    // Publish if batch is full
    if (s_rtk.current_batch.count >= ABLY_RTK_BATCH_MAX_MESSAGES) {
        should_publish = true;
    }
    // Publish if timeout elapsed and batch not empty
    else if (s_rtk.current_batch.count > 0) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_rtk.batch_start_time) / 1000;
        if (elapsed_ms >= ABLY_RTK_BATCH_TIMEOUT_MS) {
            should_publish = true;
        }
    }

    xSemaphoreGive(s_rtk.mutex);

    return should_publish;
}

esp_err_t ably_rtk_batch_publish(bool force) {
    if (!s_rtk.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_rtk.mutex, portMAX_DELAY);

    // Check if batch is empty
    if (s_rtk.current_batch.count == 0) {
        xSemaphoreGive(s_rtk.mutex);
        return ESP_OK;
    }

    // Check if there are any external subscribers
    // Note: subscriber_count includes the ESP32 itself (monitoring occupancy)
    // So we need >1 subscribers to have actual external clients
    uint32_t subscriber_count = ably_client_get_subscriber_count();
    if (subscriber_count <= 1) {
        // Drop the batch - no external subscribers (only ourselves)
        ESP_LOGD(TAG, "Dropping batch (count=%d, size=%d bytes) - no external subscribers (count=%lu)",
                 s_rtk.current_batch.count, s_rtk.current_batch.total_size, subscriber_count);
        memset(&s_rtk.current_batch, 0, sizeof(s_rtk.current_batch));
        s_rtk.batch_start_time = 0;
        xSemaphoreGive(s_rtk.mutex);
        return ESP_OK;
    }

    // Check if should publish (unless forced)
    if (!force) {
        int64_t elapsed_ms = (esp_timer_get_time() - s_rtk.batch_start_time) / 1000;
        if (elapsed_ms < ABLY_RTK_BATCH_TIMEOUT_MS &&
            s_rtk.current_batch.count < ABLY_RTK_BATCH_MAX_MESSAGES) {
            xSemaphoreGive(s_rtk.mutex);
            return ESP_OK;  // Not time yet
        }
    }

    // Create correction message
    ably_rtk_correction_msg_t msg;
    strncpy(msg.base_id, s_rtk.base_id, sizeof(msg.base_id) - 1);
    msg.timestamp = esp_timer_get_time();
    msg.sequence = s_rtk.sequence++;
    memcpy(&msg.batch, &s_rtk.current_batch, sizeof(ably_rtk_batch_t));

    // Serialize to JSON
    char *json_buffer = malloc(ABLY_RTK_JSON_BUFFER_SIZE);
    if (json_buffer == NULL) {
        xSemaphoreGive(s_rtk.mutex);
        ESP_LOGE(TAG, "Failed to allocate JSON buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = serialize_batch(&msg, json_buffer, ABLY_RTK_JSON_BUFFER_SIZE);
    if (err != ESP_OK) {
        free(json_buffer);
        xSemaphoreGive(s_rtk.mutex);
        ESP_LOGE(TAG, "Failed to serialize batch");
        return err;
    }

    size_t json_len = strlen(json_buffer);

    // Publish via Ably client
    int64_t publish_start = esp_timer_get_time();
    err = ably_client_publish(json_buffer, json_len, 1);  // QoS 1
    int64_t publish_end = esp_timer_get_time();

    free(json_buffer);

    if (err == ESP_OK) {
        // Update statistics
        s_rtk.stats.messages_sent++;
        s_rtk.stats.bytes_sent += json_len;

        uint32_t latency_ms = (publish_end - publish_start) / 1000;
        s_rtk.stats.avg_latency_ms = (s_rtk.stats.avg_latency_ms * (s_rtk.stats.messages_sent - 1) + latency_ms) / s_rtk.stats.messages_sent;

        ESP_LOGI(TAG, "Published batch seq=%lu, count=%d, size=%d bytes, latency=%lums",
                 msg.sequence, msg.batch.count, msg.batch.total_size, latency_ms);

        // Clear batch
        memset(&s_rtk.current_batch, 0, sizeof(s_rtk.current_batch));
        s_rtk.batch_start_time = 0;

        // Reset cycle trackers for the published rate
        // For now, reset all 1Hz trackers after each publish
        if (s_rtk.tracker_count > 0) {
            rtcm_config_reset_cycle(s_rtk.trackers, s_rtk.tracker_count, 1.0f);
        }
    } else {
        ESP_LOGE(TAG, "Failed to publish batch");
    }

    xSemaphoreGive(s_rtk.mutex);

    return err;
}

uint8_t ably_rtk_batch_get_count(void) {
    if (!s_rtk.initialized) {
        return 0;
    }

    xSemaphoreTake(s_rtk.mutex, portMAX_DELAY);
    uint8_t count = s_rtk.current_batch.count;
    xSemaphoreGive(s_rtk.mutex);

    return count;
}

esp_err_t ably_rtk_set_expected_messages(const void *config) {
    if (!s_rtk.initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Config required");
        return ESP_ERR_INVALID_ARG;
    }

    const rtcm_config_t *rtcm_cfg = (const rtcm_config_t *)config;

    xSemaphoreTake(s_rtk.mutex, portMAX_DELAY);

    // Initialize trackers for each enabled message
    s_rtk.tracker_count = 0;
    for (int i = 0; i < rtcm_cfg->message_count && s_rtk.tracker_count < RTCM_CONFIG_MAX_MESSAGES; i++) {
        if (rtcm_cfg->messages[i].enabled) {
            s_rtk.trackers[s_rtk.tracker_count].message_type = rtcm_cfg->messages[i].message_type;
            s_rtk.trackers[s_rtk.tracker_count].rate = rtcm_cfg->messages[i].rate;
            s_rtk.trackers[s_rtk.tracker_count].last_seen_ms = 0;
            s_rtk.trackers[s_rtk.tracker_count].received_this_cycle = false;
            s_rtk.tracker_count++;
        }
    }

    xSemaphoreGive(s_rtk.mutex);

    ESP_LOGI(TAG, "Dynamic batching configured with %d message types", s_rtk.tracker_count);
    return ESP_OK;
}

esp_err_t ably_rtk_base64_encode(const uint8_t *input, size_t input_len,
                                  char *output, size_t output_len, size_t *encoded_len) {
    if (input == NULL || output == NULL || encoded_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Calculate required output size
    size_t required_len = ((input_len + 2) / 3) * 4 + 1;  // +1 for null terminator

    if (output_len < required_len) {
        ESP_LOGE(TAG, "Output buffer too small: need %zu, have %zu", required_len, output_len);
        return ESP_ERR_NO_MEM;
    }

    int ret = mbedtls_base64_encode(
        (unsigned char *)output,
        output_len,
        encoded_len,
        input,
        input_len
    );

    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encoding failed: %d", ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ably_rtk_serialize_correction(const ably_rtk_correction_msg_t *msg,
                                        char *json_out, size_t json_len) {
    if (msg == NULL || json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return serialize_batch(msg, json_out, json_len);
}

esp_err_t ably_rtk_serialize_presence(const ably_rtk_presence_t *presence,
                                      char *json_out, size_t json_len) {
    if (presence == NULL || json_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    // Basic info
    cJSON_AddStringToObject(root, "status", presence->status);
    cJSON_AddNumberToObject(root, "uptime", presence->uptime);

    // Position
    cJSON *position = cJSON_CreateObject();
    cJSON_AddNumberToObject(position, "lat", presence->position.latitude);
    cJSON_AddNumberToObject(position, "lon", presence->position.longitude);
    cJSON_AddNumberToObject(position, "alt", presence->position.altitude);
    cJSON_AddStringToObject(position, "accuracy", accuracy_to_string(presence->position.accuracy));
    cJSON_AddItemToObject(root, "position", position);

    // Antenna
    cJSON *antenna = cJSON_CreateObject();
    cJSON_AddNumberToObject(antenna, "height", presence->antenna.height);
    cJSON_AddStringToObject(antenna, "model", presence->antenna.model);
    cJSON_AddItemToObject(root, "antenna", antenna);

    // GNSS status
    cJSON *gnss = cJSON_CreateObject();
    cJSON *constellations = cJSON_CreateArray();
    add_constellation_names(constellations, &presence->gnss);
    cJSON_AddItemToObject(gnss, "constellations", constellations);
    cJSON_AddNumberToObject(gnss, "satellites", presence->gnss.satellites);
    cJSON_AddStringToObject(gnss, "fix_type", fix_type_to_string(presence->gnss.fix_type));
    cJSON_AddItemToObject(root, "gnss", gnss);

    // Statistics
    cJSON *stats = cJSON_CreateObject();
    cJSON_AddNumberToObject(stats, "messages_sent", (double)presence->stats.messages_sent);
    cJSON_AddNumberToObject(stats, "bytes_sent", (double)presence->stats.bytes_sent);
    cJSON_AddNumberToObject(stats, "avg_latency_ms", presence->stats.avg_latency_ms);
    cJSON_AddItemToObject(root, "stats", stats);

    // Firmware
    cJSON *firmware = cJSON_CreateObject();
    cJSON_AddStringToObject(firmware, "version", presence->firmware.version);
    cJSON_AddStringToObject(firmware, "build", presence->firmware.build);
    cJSON_AddItemToObject(root, "firmware", firmware);

    // Print to string
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to print JSON");
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(json_str);
    if (len >= json_len) {
        free(json_str);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "JSON buffer too small");
        return ESP_ERR_NO_MEM;
    }

    strcpy(json_out, json_str);
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

esp_err_t ably_rtk_update_presence(const ably_rtk_presence_t *presence) {
    if (presence == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Serialize presence data
    char *json_buffer = malloc(2048);
    if (json_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate presence buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ably_rtk_serialize_presence(presence, json_buffer, 2048);
    if (err != ESP_OK) {
        free(json_buffer);
        return err;
    }

    // Update presence via Ably client
    err = ably_client_presence_update(json_buffer);
    free(json_buffer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Updated presence data");
    }

    return err;
}

void ably_rtk_get_stats(ably_rtk_stats_t *stats) {
    if (stats == NULL || !s_rtk.initialized) {
        return;
    }

    xSemaphoreTake(s_rtk.mutex, portMAX_DELAY);
    memcpy(stats, &s_rtk.stats, sizeof(ably_rtk_stats_t));
    xSemaphoreGive(s_rtk.mutex);
}

/* Private functions */

static esp_err_t serialize_batch(const ably_rtk_correction_msg_t *msg, char *json_out, size_t json_len) {
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_ERR_NO_MEM;
    }

    // Message envelope
    cJSON_AddStringToObject(root, "type", ABLY_RTK_MESSAGE_TYPE);
    cJSON_AddNumberToObject(root, "version", ABLY_RTK_PROTOCOL_VERSION);
    cJSON_AddStringToObject(root, "base_id", msg->base_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)msg->timestamp);
    cJSON_AddNumberToObject(root, "sequence", msg->sequence);

    // Batch
    cJSON *batch = cJSON_CreateObject();
    cJSON_AddNumberToObject(batch, "count", msg->batch.count);
    cJSON_AddNumberToObject(batch, "total_size", msg->batch.total_size);

    // Messages array
    cJSON *messages = cJSON_CreateArray();
    for (int i = 0; i < msg->batch.count; i++) {
        cJSON *message = cJSON_CreateObject();
        cJSON_AddNumberToObject(message, "type", msg->batch.messages[i].type);
        cJSON_AddNumberToObject(message, "size", msg->batch.messages[i].size);

        // Base64 encode RTCM data
        size_t encoded_len;
        size_t required_len = ((msg->batch.messages[i].size + 2) / 3) * 4 + 1;
        char *base64_buffer = malloc(required_len);
        if (base64_buffer == NULL) {
            cJSON_Delete(root);
            ESP_LOGE(TAG, "Failed to allocate base64 buffer");
            return ESP_ERR_NO_MEM;
        }

        esp_err_t err = ably_rtk_base64_encode(
            msg->batch.messages[i].data,
            msg->batch.messages[i].size,
            base64_buffer,
            required_len,
            &encoded_len
        );

        if (err != ESP_OK) {
            free(base64_buffer);
            cJSON_Delete(root);
            return err;
        }

        cJSON_AddStringToObject(message, "data", base64_buffer);
        free(base64_buffer);

        cJSON_AddItemToArray(messages, message);
    }

    cJSON_AddItemToObject(batch, "messages", messages);
    cJSON_AddItemToObject(root, "batch", batch);

    // Print to string
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str == NULL) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to print JSON");
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(json_str);
    if (len >= json_len) {
        free(json_str);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "JSON buffer too small: need %zu, have %zu", len, json_len);
        return ESP_ERR_NO_MEM;
    }

    strcpy(json_out, json_str);
    free(json_str);
    cJSON_Delete(root);

    return ESP_OK;
}

static void add_constellation_names(cJSON *array, const ably_rtk_gnss_status_t *gnss) {
    if (gnss->constellations[GNSS_GPS]) {
        cJSON_AddItemToArray(array, cJSON_CreateString("GPS"));
    }
    if (gnss->constellations[GNSS_GLONASS]) {
        cJSON_AddItemToArray(array, cJSON_CreateString("GLONASS"));
    }
    if (gnss->constellations[GNSS_GALILEO]) {
        cJSON_AddItemToArray(array, cJSON_CreateString("Galileo"));
    }
    if (gnss->constellations[GNSS_BEIDOU]) {
        cJSON_AddItemToArray(array, cJSON_CreateString("BeiDou"));
    }
    if (gnss->constellations[GNSS_QZSS]) {
        cJSON_AddItemToArray(array, cJSON_CreateString("QZSS"));
    }
    if (gnss->constellations[GNSS_SBAS]) {
        cJSON_AddItemToArray(array, cJSON_CreateString("SBAS"));
    }
}

static const char *fix_type_to_string(gnss_fix_type_t fix_type) {
    switch (fix_type) {
        case FIX_NO_FIX: return "NO_FIX";
        case FIX_2D: return "2D";
        case FIX_3D: return "3D";
        case FIX_RTK_FLOAT: return "RTK_FLOAT";
        case FIX_RTK_FIXED: return "RTK_FIXED";
        default: return "UNKNOWN";
    }
}

static const char *accuracy_to_string(position_accuracy_t accuracy) {
    switch (accuracy) {
        case POS_ACCURACY_SURVEY_IN: return "survey_in";
        case POS_ACCURACY_MANUAL: return "manual";
        case POS_ACCURACY_AUTONOMOUS: return "autonomous";
        default: return "unknown";
    }
}
