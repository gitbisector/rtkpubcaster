/*
 * RTCM3 Configuration Management Implementation
 *
 * Provides data-driven configuration for RTCM3 message output.
 * Handles NVS storage, GPS query/apply, and configuration validation.
 */

#include "rtcm_config.h"
#include "uart.h"
#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cJSON.h>

static const char *TAG = "rtcm_config";

/* Global configuration state */
static rtcm_config_t s_config = {0};
static bool s_initialized = false;

/* RTCM3 message descriptions */
static const struct {
    uint16_t type;
    const char *description;
} rtcm_descriptions[] = {
    {1001, "L1-Only GPS RTK Observables"},
    {1002, "Extended L1-Only GPS RTK Observables"},
    {1003, "L1/L2 GPS RTK Observables"},
    {1004, "Extended L1/L2 GPS RTK Observables"},
    {1005, "Station Coordinates (no height)"},
    {1006, "Station Coordinates (with height)"},
    {1007, "Antenna Descriptor"},
    {1008, "Antenna Descriptor & Serial"},
    {1033, "Receiver/Antenna Descriptor"},
    {1074, "GPS MSM4"},
    {1075, "GPS MSM5"},
    {1076, "GPS MSM6"},
    {1077, "GPS MSM7"},
    {1084, "GLONASS MSM4"},
    {1085, "GLONASS MSM5"},
    {1086, "GLONASS MSM6"},
    {1087, "GLONASS MSM7"},
    {1094, "Galileo MSM4"},
    {1095, "Galileo MSM5"},
    {1096, "Galileo MSM6"},
    {1097, "Galileo MSM7"},
    {1114, "QZSS MSM4"},
    {1115, "QZSS MSM5"},
    {1116, "QZSS MSM6"},
    {1117, "QZSS MSM7"},
    {1124, "BeiDou MSM4"},
    {1125, "BeiDou MSM5"},
    {1126, "BeiDou MSM6"},
    {1127, "BeiDou MSM7"},
    {0, NULL}
};

const char* rtcm_config_get_message_description(uint16_t message_type) {
    for (int i = 0; rtcm_descriptions[i].description != NULL; i++) {
        if (rtcm_descriptions[i].type == message_type) {
            return rtcm_descriptions[i].description;
        }
    }
    return "Unknown RTCM3 Message";
}

void rtcm_config_get_default(rtcm_config_t *config) {
    memset(config, 0, sizeof(rtcm_config_t));
    config->message_count = 0;

    // Default BASE station configuration
    rtcm_config_add_message(config, 1005, COM_PORT_COM1, 10.0f, "Station ARP (no height)");
    rtcm_config_add_message(config, 1074, COM_PORT_COM1, 1.0f, "GPS MSM4");
    rtcm_config_add_message(config, 1084, COM_PORT_COM1, 1.0f, "GLONASS MSM4");
    rtcm_config_add_message(config, 1094, COM_PORT_COM1, 1.0f, "Galileo MSM4");
    rtcm_config_add_message(config, 1124, COM_PORT_COM1, 1.0f, "BeiDou MSM4");
}

esp_err_t rtcm_config_add_message(
    rtcm_config_t *config,
    uint16_t message_type,
    rtcm_com_port_t com_port,
    rtcm_update_rate_t rate,
    const char *description
) {
    if (config->message_count >= RTCM_CONFIG_MAX_MESSAGES) {
        return ESP_ERR_NO_MEM;
    }

    rtcm_message_config_t *msg = &config->messages[config->message_count];
    msg->message_type = message_type;
    msg->enabled = true;
    msg->com_port = com_port;
    msg->rate = rate;

    if (description) {
        strncpy(msg->description, description, sizeof(msg->description) - 1);
        msg->description[sizeof(msg->description) - 1] = '\0';
    } else {
        strncpy(msg->description, rtcm_config_get_message_description(message_type),
                sizeof(msg->description) - 1);
        msg->description[sizeof(msg->description) - 1] = '\0';
    }

    config->message_count++;
    return ESP_OK;
}

esp_err_t rtcm_config_remove_message(rtcm_config_t *config, uint16_t message_type) {
    for (int i = 0; i < config->message_count; i++) {
        if (config->messages[i].message_type == message_type) {
            // Shift remaining messages down
            memmove(&config->messages[i], &config->messages[i + 1],
                    (config->message_count - i - 1) * sizeof(rtcm_message_config_t));
            config->message_count--;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

rtcm_message_config_t* rtcm_config_find_message(rtcm_config_t *config, uint16_t message_type) {
    for (int i = 0; i < config->message_count; i++) {
        if (config->messages[i].message_type == message_type) {
            return &config->messages[i];
        }
    }
    return NULL;
}

esp_err_t rtcm_config_validate(const rtcm_config_t *config) {
    if (config->message_count > RTCM_CONFIG_MAX_MESSAGES) {
        ESP_LOGE(TAG, "Too many messages: %d (max %d)", config->message_count, RTCM_CONFIG_MAX_MESSAGES);
        return ESP_ERR_INVALID_ARG;
    }

    // Check for duplicate message types
    for (int i = 0; i < config->message_count; i++) {
        for (int j = i + 1; j < config->message_count; j++) {
            if (config->messages[i].message_type == config->messages[j].message_type) {
                ESP_LOGE(TAG, "Duplicate message type: %d", config->messages[i].message_type);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    // Validate rates and COM ports
    for (int i = 0; i < config->message_count; i++) {
        const rtcm_message_config_t *msg = &config->messages[i];

        if (msg->com_port < COM_PORT_COM1 || msg->com_port > COM_PORT_USB) {
            ESP_LOGE(TAG, "Invalid COM port: %d", msg->com_port);
            return ESP_ERR_INVALID_ARG;
        }

        // Validate rate is a positive number (supports fractional rates for high-frequency updates)
        if (msg->rate <= 0.0f || msg->rate > 60.0f) {
            ESP_LOGE(TAG, "Invalid rate: %g (must be between 0 and 60 seconds)", msg->rate);
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

esp_err_t rtcm_config_init(void) {
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing RTCM configuration");

    // Initialize NVS if not already done
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Try to load from NVS
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE_RTCM, NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        // Load message count
        uint8_t msg_count = 0;
        err = nvs_get_u8(nvs_handle, NVS_KEY_RTCM_COUNT, &msg_count);

        if (err == ESP_OK && msg_count > 0 && msg_count <= RTCM_CONFIG_MAX_MESSAGES) {
            s_config.message_count = msg_count;

            // Load each message
            for (int i = 0; i < msg_count; i++) {
                char key[16];
                snprintf(key, sizeof(key), "%s%d", NVS_KEY_RTCM_PREFIX, i);

                size_t required_size = sizeof(rtcm_message_config_t);
                err = nvs_get_blob(nvs_handle, key, &s_config.messages[i], &required_size);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "Failed to load message %d from NVS: %s", i, esp_err_to_name(err));
                    break;
                }
            }

            ESP_LOGI(TAG, "Loaded %d messages from NVS", msg_count);
        } else {
            ESP_LOGI(TAG, "No valid configuration in NVS, using defaults");
            rtcm_config_get_default(&s_config);
        }

        nvs_close(nvs_handle);
    } else {
        ESP_LOGI(TAG, "NVS namespace not found, using defaults");
        rtcm_config_get_default(&s_config);
    }

    // Validate loaded configuration
    err = rtcm_config_validate(&s_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Loaded configuration invalid, using defaults");
        rtcm_config_get_default(&s_config);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized with %d messages", s_config.message_count);

    return ESP_OK;
}

esp_err_t rtcm_config_get(rtcm_config_t *config) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(config, &s_config, sizeof(rtcm_config_t));
    return ESP_OK;
}

esp_err_t rtcm_config_set(const rtcm_config_t *config) {
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    // Validate first
    esp_err_t err = rtcm_config_validate(config);
    if (err != ESP_OK) {
        return err;
    }

    // Save to NVS
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE_RTCM, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Save message count
    err = nvs_set_u8(nvs_handle, NVS_KEY_RTCM_COUNT, config->message_count);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    // Save each message
    for (int i = 0; i < config->message_count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_RTCM_PREFIX, i);

        err = nvs_set_blob(nvs_handle, key, &config->messages[i], sizeof(rtcm_message_config_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save message %d: %s", i, esp_err_to_name(err));
            nvs_close(nvs_handle);
            return err;
        }
    }

    // Commit
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Update runtime config
    memcpy(&s_config, config, sizeof(rtcm_config_t));
    ESP_LOGI(TAG, "Configuration saved (%d messages)", config->message_count);

    return ESP_OK;
}

esp_err_t rtcm_config_apply(const rtcm_config_t *config) {
    ESP_LOGI(TAG, "Applying configuration to GPS (%d messages)", config->message_count);

    // Disable all RTCM messages first
    const char *unlog_cmd = "UNLOG\r\n";
    uart_write((char *)unlog_cmd, strlen(unlog_cmd));
    vTaskDelay(pdMS_TO_TICKS(100));

    // Apply each configured message
    for (int i = 0; i < config->message_count; i++) {
        const rtcm_message_config_t *msg = &config->messages[i];

        if (!msg->enabled) {
            continue;
        }

        // Build command: "RTCM1074 COM1 1\r\n"
        char cmd[64];
        const char *port_name;
        switch (msg->com_port) {
            case COM_PORT_COM1: port_name = "COM1"; break;
            case COM_PORT_COM2: port_name = "COM2"; break;
            case COM_PORT_COM3: port_name = "COM3"; break;
            case COM_PORT_USB:  port_name = "USB"; break;
            default: port_name = "COM1"; break;
        }

        snprintf(cmd, sizeof(cmd), "RTCM%d %s %g\r\n",
                 msg->message_type, port_name, msg->rate);

        ESP_LOGI(TAG, "Sending: %s", cmd);
        uart_write(cmd, strlen(cmd));
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Save configuration to GPS
    const char *save_cmd = "SAVECONFIG\r\n";
    uart_write((char *)save_cmd, strlen(save_cmd));

    ESP_LOGI(TAG, "Configuration applied successfully");
    return ESP_OK;
}

esp_err_t rtcm_config_detect_from_gps(rtcm_config_t *config, uint32_t timeout_ms) {
    ESP_LOGI(TAG, "Querying GPS for RTCM configuration...");

    // TODO: Implement LOG command parsing
    // This requires:
    // 1. Send "LOG\r\n" command to GPS
    // 2. Parse response lines looking for RTCM messages
    // 3. Extract message type, COM port, and rate from each line
    // 4. Populate config structure

    // For now, return error - this will be implemented in the UART task
    ESP_LOGW(TAG, "GPS detection not yet implemented");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t rtcm_config_get_messages_at_rate(
    const rtcm_config_t *config,
    rtcm_update_rate_t rate,
    uint16_t *types,
    size_t max_types,
    size_t *count
) {
    *count = 0;

    for (int i = 0; i < config->message_count && *count < max_types; i++) {
        if (config->messages[i].enabled && config->messages[i].rate == rate) {
            types[*count] = config->messages[i].message_type;
            (*count)++;
        }
    }

    return ESP_OK;
}

bool rtcm_config_is_complete_set(
    const rtcm_message_tracker_t *trackers,
    size_t tracker_count,
    rtcm_update_rate_t rate
) {
    bool found_any = false;

    for (size_t i = 0; i < tracker_count; i++) {
        if (trackers[i].rate == rate) {
            found_any = true;
            if (!trackers[i].received_this_cycle) {
                return false;  // Missing at least one message
            }
        }
    }

    return found_any;  // Complete if we found at least one and all were received
}

void rtcm_config_reset_cycle(
    rtcm_message_tracker_t *trackers,
    size_t tracker_count,
    rtcm_update_rate_t rate
) {
    for (size_t i = 0; i < tracker_count; i++) {
        if (trackers[i].rate == rate) {
            trackers[i].received_this_cycle = false;
        }
    }
}

esp_err_t rtcm_config_serialize_json(
    const rtcm_config_t *config,
    char *json_out,
    size_t json_len
) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddNumberToObject(root, "message_count", config->message_count);

    cJSON *messages = cJSON_CreateArray();
    for (int i = 0; i < config->message_count; i++) {
        const rtcm_message_config_t *msg = &config->messages[i];

        cJSON *msg_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(msg_obj, "message_type", msg->message_type);
        cJSON_AddBoolToObject(msg_obj, "enabled", msg->enabled);
        cJSON_AddNumberToObject(msg_obj, "com_port", msg->com_port);
        cJSON_AddNumberToObject(msg_obj, "rate", msg->rate);
        cJSON_AddStringToObject(msg_obj, "description", msg->description);

        cJSON_AddItemToArray(messages, msg_obj);
    }
    cJSON_AddItemToObject(root, "messages", messages);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    if (strlen(json_str) >= json_len) {
        free(json_str);
        return ESP_ERR_INVALID_SIZE;
    }

    strcpy(json_out, json_str);
    free(json_str);

    return ESP_OK;
}

esp_err_t rtcm_config_deserialize_json(const char *json_str, rtcm_config_t *config) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(config, 0, sizeof(rtcm_config_t));

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (!cJSON_IsArray(messages)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int count = cJSON_GetArraySize(messages);
    if (count > RTCM_CONFIG_MAX_MESSAGES) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_SIZE;
    }

    for (int i = 0; i < count; i++) {
        cJSON *msg_obj = cJSON_GetArrayItem(messages, i);
        rtcm_message_config_t *msg = &config->messages[i];

        cJSON *message_type = cJSON_GetObjectItem(msg_obj, "message_type");
        cJSON *enabled = cJSON_GetObjectItem(msg_obj, "enabled");
        cJSON *com_port = cJSON_GetObjectItem(msg_obj, "com_port");
        cJSON *rate = cJSON_GetObjectItem(msg_obj, "rate");
        cJSON *description = cJSON_GetObjectItem(msg_obj, "description");

        if (!cJSON_IsNumber(message_type) || !cJSON_IsBool(enabled) ||
            !cJSON_IsNumber(com_port) || !cJSON_IsNumber(rate)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        msg->message_type = message_type->valueint;
        msg->enabled = cJSON_IsTrue(enabled);
        msg->com_port = com_port->valueint;
        msg->rate = (float)rate->valuedouble;

        if (cJSON_IsString(description)) {
            strncpy(msg->description, description->valuestring, sizeof(msg->description) - 1);
            msg->description[sizeof(msg->description) - 1] = '\0';
        } else {
            msg->description[0] = '\0';
        }
    }

    config->message_count = count;
    cJSON_Delete(root);

    return rtcm_config_validate(config);
}
