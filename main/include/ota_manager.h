#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <esp_err.h>
#include <esp_ota_ops.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_IN_PROGRESS,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *update_partition;
    uint32_t image_size;
    uint32_t bytes_written;
    ota_state_t state;
    char error_msg[128];
} ota_context_t;

/**
 * @brief Initialize OTA system
 *
 * @return ESP_OK on success
 */
esp_err_t ota_manager_init(void);

/**
 * @brief Begin OTA update
 *
 * @param image_size Expected firmware image size
 * @param ctx OTA context structure to initialize
 * @return ESP_OK on success
 */
esp_err_t ota_begin(uint32_t image_size, ota_context_t *ctx);

/**
 * @brief Write firmware chunk
 *
 * @param data Firmware data chunk
 * @param size Size of data chunk
 * @param ctx OTA context
 * @return ESP_OK on success
 */
esp_err_t ota_write(const void *data, size_t size, ota_context_t *ctx);

/**
 * @brief Finalize OTA update and activate new partition
 *
 * This function calls esp_ota_end() which automatically performs:
 * - SHA-256 verification
 * - Signature verification (if secure boot enabled)
 * - Anti-rollback check (if enabled)
 *
 * @param ctx OTA context
 * @return ESP_OK on success, ESP_ERR_OTA_VALIDATE_FAILED if signature invalid
 */
esp_err_t ota_end_and_activate(ota_context_t *ctx);

/**
 * @brief Mark current app as valid after successful boot
 *
 * Call this after verifying the new firmware is stable.
 * Prevents automatic rollback to previous partition.
 *
 * @return ESP_OK on success
 */
esp_err_t ota_mark_valid(void);

/**
 * @brief Get current firmware version information
 *
 * @param version Buffer for version string
 * @param v_len Version buffer length
 * @param sha256 Buffer for SHA256 string (65 bytes for full hex + null)
 * @param s_len SHA256 buffer length
 */
void ota_get_version_info(char *version, size_t v_len, char *sha256, size_t s_len);

/**
 * @brief Check if OTA update is in progress
 *
 * @return true if OTA is in progress, false otherwise
 */
bool ota_is_in_progress(void);

#endif // OTA_MANAGER_H
