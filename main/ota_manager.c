#include "ota_manager.h"
#include <esp_log.h>
#include <esp_app_format.h>
#include <esp_image_format.h>
#include <string.h>

static const char *TAG = "OTA";
static bool g_ota_in_progress = false;

esp_err_t ota_manager_init(void) {
    ESP_LOGI(TAG, "Initializing OTA manager");

    // Check if we're pending verification after OTA
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGW(TAG, "Running firmware pending verification (first boot after OTA)");
        } else if (ota_state == ESP_OTA_IMG_VALID) {
            ESP_LOGI(TAG, "Running firmware marked as valid");
        }
    }

    ESP_LOGI(TAG, "Running partition: %s at offset 0x%lx",
             running->label, running->address);

    return ESP_OK;
}

esp_err_t ota_begin(uint32_t image_size, ota_context_t *ctx) {
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_ota_in_progress) {
        ESP_LOGE(TAG, "OTA already in progress");
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Validate image size
    if (image_size == 0 || image_size > 0x150000) {  // Max 1.3 MB
        ESP_LOGE(TAG, "Invalid image size: %lu bytes (max 1376256)", image_size);
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Invalid size: %lu bytes", image_size);
        return ESP_ERR_INVALID_SIZE;
    }

    // Initialize context
    memset(ctx, 0, sizeof(ota_context_t));
    ctx->image_size = image_size;
    ctx->state = OTA_STATE_IDLE;

    // Get next update partition
    ctx->update_partition = esp_ota_get_next_update_partition(NULL);
    if (ctx->update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "No OTA partition");
        ctx->state = OTA_STATE_FAILED;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting OTA update");
    ESP_LOGI(TAG, "  Target partition: %s", ctx->update_partition->label);
    ESP_LOGI(TAG, "  Partition offset: 0x%lx", ctx->update_partition->address);
    ESP_LOGI(TAG, "  Partition size: %lu bytes", ctx->update_partition->size);
    ESP_LOGI(TAG, "  Image size: %lu bytes", image_size);

    // Begin OTA
    esp_err_t err = esp_ota_begin(ctx->update_partition, image_size, &ctx->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "OTA begin failed: %s", esp_err_to_name(err));
        ctx->state = OTA_STATE_FAILED;
        return err;
    }

    ctx->state = OTA_STATE_IN_PROGRESS;
    g_ota_in_progress = true;

    ESP_LOGI(TAG, "OTA session started");

    return ESP_OK;
}

esp_err_t ota_write(const void *data, size_t size, ota_context_t *ctx) {
    if (ctx == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->state != OTA_STATE_IN_PROGRESS) {
        ESP_LOGE(TAG, "OTA not in progress");
        return ESP_ERR_INVALID_STATE;
    }

    // Check for overflow
    if (ctx->bytes_written + size > ctx->image_size) {
        ESP_LOGE(TAG, "Data overflow: written=%lu, incoming=%u, max=%lu",
                 ctx->bytes_written, size, ctx->image_size);
        snprintf(ctx->error_msg, sizeof(ctx->error_msg), "Data overflow");
        ctx->state = OTA_STATE_FAILED;
        g_ota_in_progress = false;
        return ESP_ERR_INVALID_SIZE;
    }

    // Write to partition
    esp_err_t err = esp_ota_write(ctx->handle, data, size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Write failed: %s", esp_err_to_name(err));
        ctx->state = OTA_STATE_FAILED;
        g_ota_in_progress = false;
        return err;
    }

    ctx->bytes_written += size;

    // Log progress every 64 KB
    if (ctx->bytes_written % (64 * 1024) == 0) {
        uint8_t progress = (ctx->bytes_written * 100) / ctx->image_size;
        ESP_LOGI(TAG, "OTA progress: %u%% (%lu/%lu bytes)",
                 progress, ctx->bytes_written, ctx->image_size);
    }

    return ESP_OK;
}

esp_err_t ota_end_and_activate(ota_context_t *ctx) {
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->state != OTA_STATE_IN_PROGRESS) {
        ESP_LOGE(TAG, "OTA not in progress");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Finalizing OTA update (%lu bytes written)", ctx->bytes_written);

    // Finalize OTA write
    // This performs automatic signature verification if secure boot is enabled
    esp_err_t err = esp_ota_end(ctx->handle);

    g_ota_in_progress = false;

    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed - invalid signature or corrupted image");
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "Signature verification failed");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                     "OTA end failed: %s", esp_err_to_name(err));
        }
        ctx->state = OTA_STATE_FAILED;
        return err;
    }

    ESP_LOGI(TAG, "Image validation successful");

    // Set boot partition
    err = esp_ota_set_boot_partition(ctx->update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "Set boot partition failed: %s", esp_err_to_name(err));
        ctx->state = OTA_STATE_FAILED;
        return err;
    }

    ESP_LOGI(TAG, "Boot partition set to %s", ctx->update_partition->label);
    ESP_LOGI(TAG, "OTA update successful - device will restart");

    ctx->state = OTA_STATE_SUCCESS;

    return ESP_OK;
}

esp_err_t ota_mark_valid(void) {
    ESP_LOGI(TAG, "Marking current app as valid");

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mark app valid: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "App marked as valid - rollback disabled");

    return ESP_OK;
}

void ota_get_version_info(char *version, size_t v_len, char *sha256, size_t s_len) {
    const esp_app_desc_t *app_desc = esp_app_get_description();

    if (version != NULL && v_len > 0) {
        snprintf(version, v_len, "%s", app_desc->version);
    }

    if (sha256 != NULL && s_len > 0) {
        char elf_sha256[32];
        if (esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256)) == ESP_OK) {
            // Convert to hex string
            for (int i = 0; i < 32 && (i * 2 + 2) < s_len; i++) {
                snprintf(&sha256[i * 2], 3, "%02x", (unsigned char)elf_sha256[i]);
            }
            sha256[64] = '\0';
        } else {
            sha256[0] = '\0';
        }
    }
}

bool ota_is_in_progress(void) {
    return g_ota_in_progress;
}
