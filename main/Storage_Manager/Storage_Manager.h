#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"

/**
 * @brief Initialize SPIFFS filesystem
 * 
 * Mounts the SPIFFS partition and registers it with VFS
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_init(void);

/**
 * @brief Read a file from SPIFFS
 * 
 * @param path File path (e.g., "/storage/images/widget.bin")
 * @param buffer Buffer to store file contents
 * @param max_size Maximum size to read
 * @param bytes_read Pointer to store number of bytes actually read
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_read_file(const char *path, void *buffer, size_t max_size, size_t *bytes_read);

/**
 * @brief Write a file to SPIFFS
 * 
 * @param path File path
 * @param data Data to write
 * @param size Size of data to write
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_write_file(const char *path, const void *data, size_t size);

/**
 * @brief Check if a file exists
 * 
 * @param path File path
 * @return true if file exists, false otherwise
 */
bool storage_file_exists(const char *path);

/**
 * @brief Get file size
 * 
 * @param path File path
 * @param size Pointer to store file size
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_get_file_size(const char *path, size_t *size);

/**
 * @brief Delete a file from SPIFFS
 * 
 * @param path File path
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_delete_file(const char *path);

/**
 * @brief List all files in a directory
 * 
 * @param base_path Base directory path (e.g., "/storage/images")
 * @param max_files Maximum number of files to list
 * @param files Array to store file names
 * @param file_count Pointer to store actual number of files found
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_list_files(const char *base_path, int max_files, char files[][64], int *file_count);

/**
 * @brief Log every regular file under /storage (recursive) at INFO — for serial monitor during boot.
 *
 * Each line is one full path, e.g. `/storage/Logos/Cobra_text.bin`.
 */
void storage_log_all_files(void);

/**
 * @brief Get total and used space in SPIFFS
 * 
 * @param total_bytes Pointer to store total bytes
 * @param used_bytes Pointer to store used bytes
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_get_info(size_t *total_bytes, size_t *used_bytes);

/**
 * @brief Deinitialize SPIFFS
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t storage_deinit(void);

#endif // STORAGE_MANAGER_H

