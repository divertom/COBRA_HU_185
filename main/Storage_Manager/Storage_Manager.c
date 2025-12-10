#include "Storage_Manager.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG = "Storage_Manager";
static bool spiffs_mounted = false;

esp_err_t storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "model",
        .max_files = 5,
        .format_if_mount_failed = false
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    spiffs_mounted = true;
    return ESP_OK;
}

esp_err_t storage_read_file(const char *path, void *buffer, size_t max_size, size_t *bytes_read)
{
    if (!spiffs_mounted) {
        ESP_LOGE(TAG, "SPIFFS not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/storage%s", path);

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return ESP_FAIL;
    }

    size_t read = fread(buffer, 1, max_size, f);
    fclose(f);

    if (bytes_read) {
        *bytes_read = read;
    }

    return ESP_OK;
}

esp_err_t storage_write_file(const char *path, const void *data, size_t size)
{
    if (!spiffs_mounted) {
        ESP_LOGE(TAG, "SPIFFS not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/storage%s", path);

    FILE *f = fopen(full_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", full_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    if (written != size) {
        ESP_LOGE(TAG, "Failed to write all data to file: %s", full_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool storage_file_exists(const char *path)
{
    if (!spiffs_mounted) {
        return false;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/storage%s", path);

    FILE *f = fopen(full_path, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    return false;
}

esp_err_t storage_get_file_size(const char *path, size_t *size)
{
    if (!spiffs_mounted) {
        ESP_LOGE(TAG, "SPIFFS not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/storage%s", path);

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file: %s", full_path);
        return ESP_FAIL;
    }

    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fclose(f);

    return ESP_OK;
}

esp_err_t storage_delete_file(const char *path)
{
    if (!spiffs_mounted) {
        ESP_LOGE(TAG, "SPIFFS not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/storage%s", path);

    if (remove(full_path) != 0) {
        ESP_LOGE(TAG, "Failed to delete file: %s", full_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t storage_list_files(const char *base_path, int max_files, char files[][64], int *file_count)
{
    if (!spiffs_mounted) {
        ESP_LOGE(TAG, "SPIFFS not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[128];
    snprintf(full_path, sizeof(full_path), "/storage%s", base_path);

    DIR *dir = opendir(full_path);
    if (dir == NULL) {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path);
        return ESP_FAIL;
    }

    *file_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && *file_count < max_files) {
        if (entry->d_type == DT_REG) {  // Regular file
            strncpy(files[*file_count], entry->d_name, 63);
            files[*file_count][63] = '\0';
            (*file_count)++;
        }
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t storage_get_info(size_t *total_bytes, size_t *used_bytes)
{
    if (!spiffs_mounted) {
        ESP_LOGE(TAG, "SPIFFS not mounted");
        return ESP_ERR_INVALID_STATE;
    }

    return esp_spiffs_info("model", total_bytes, used_bytes);
}

esp_err_t storage_deinit(void)
{
    if (!spiffs_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_vfs_spiffs_unregister("model");
    if (ret == ESP_OK) {
        spiffs_mounted = false;
    }
    return ret;
}

