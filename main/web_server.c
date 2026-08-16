/*
 * This file is part of the ESP32-XBee distribution (https://github.com/nebkat/esp32-xbee).
 * Copyright (c) 2019 Nebojsa Cvetkovic.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <esp_http_server.h>
#include <esp_log.h>
#include <wifi.h>
#include <cJSON.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <dirent.h>
#include <esp_vfs.h>
#include <esp_spiffs.h>
#include <lwip/apps/mdns.h>
#include <config.h>
#include <log.h>
#include <core_dump.h>
#include <util.h>
#include <lwip/inet.h>
#include <driver/uart.h>
#include <esp_ota_ops.h>
#include <esp_wifi_ap_get_sta_list.h>
#include <stream_stats.h>
#ifdef CONFIG_IDF_TARGET_ESP32
#include <esp32/rom/crc.h>
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#include <esp32c3/rom/crc.h>
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#include <esp32s3/rom/crc.h>
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
#include <esp32c6/rom/crc.h>
#else
#include <rom/crc.h>
#endif
#include <lwip/sockets.h>
#include <esp_timer.h>
#include "web_server.h"
#include "sd_logger.h"

// Max length a file path can have on storage
#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + CONFIG_SPIFFS_OBJ_NAME_LEN)
#define FILE_HASH_SUFFIX ".crc"

#define WWW_PARTITION_PATH "/www"
#define WWW_PARTITION_LABEL "www"
#define BUFFER_SIZE 2048

static const char *TAG = "WEB";

static char *buffer;

enum auth_method {
    AUTH_METHOD_OPEN = 0,
    AUTH_METHOD_HOTSPOT = 1,
    AUTH_METHOD_BASIC = 2
};

static char *basic_authentication;
static enum auth_method auth_method;

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

static esp_err_t www_spiffs_init() {
    ESP_LOGI(TAG, "Initializing SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
            .base_path = WWW_PARTITION_PATH,
            .partition_label = WWW_PARTITION_LABEL,
            .max_files = 10,
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
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(WWW_PARTITION_LABEL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SPIFFS: total=%d bytes, used=%d bytes (%.1f%%)", 
             total, used, (used * 100.0) / total);
    
    // List files in SPIFFS
    DIR *dir = opendir(WWW_PARTITION_PATH);
    if (dir) {
        ESP_LOGI(TAG, "Files in %s:", WWW_PARTITION_PATH);
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            char full_path[300];
            snprintf(full_path, sizeof(full_path), "%s/%s", WWW_PARTITION_PATH, entry->d_name);
            struct stat st;
            if (stat(full_path, &st) == 0) {
                ESP_LOGI(TAG, "  - %s (%ld bytes)", entry->d_name, st.st_size);
            }
        }
        closedir(dir);
    } else {
        ESP_LOGE(TAG, "Failed to open directory %s", WWW_PARTITION_PATH);
    }
    
    return ESP_OK;
}

// Set HTTP response content type according to file extension
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".js")) {
        return httpd_resp_set_type(req, "application/javascript");
    } else if (IS_FILE_EXT(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        // Full path string won't fit into destination buffer
        return NULL;
    }

    // Construct full path (base + path) - безопасная копия
    strlcpy(dest, base_path, destsize);
    strlcpy(dest + base_pathlen, uri, destsize - base_pathlen);

    // Return pointer to path, skipping the base
    return dest + base_pathlen;
}

static esp_err_t json_response(httpd_req_t *req, cJSON *root) {
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    // Set mime type
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t basic_auth(httpd_req_t *req) {
    if (!basic_authentication) goto _auth_required;

    size_t authorization_value_length = httpd_req_get_hdr_value_len(req, "Authorization");
    if (authorization_value_length == 0) goto _auth_required;

    size_t authorization_length = authorization_value_length + 1;

    char *authorization_header = malloc(authorization_length);
    if (!authorization_header) {
        ESP_LOGE(TAG, "Failed to allocate memory for authorization header");
        goto _auth_required;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", authorization_header,
                                    authorization_length) != ESP_OK) {
        free(authorization_header);
        goto _auth_required;
    }

    bool authenticated = strcasecmp(basic_authentication, authorization_header) == 0;
    free(authorization_header);

    if (authenticated) return ESP_OK;

    _auth_required:
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 XBee Config\"");
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Incorrect or no password provided";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t hotspot_auth(httpd_req_t *req) {
    int sock = httpd_req_to_sockfd(req);

    struct sockaddr_storage client_addr;
    socklen_t socklen = sizeof(client_addr);
    if (getpeername(sock, (struct sockaddr *)&client_addr, &socklen) != 0) goto auth_error;

    uint32_t client_ipv4;
    if (client_addr.ss_family == AF_INET) {
        client_ipv4 = ((struct sockaddr_in *) &client_addr)->sin_addr.s_addr;
    } else if (client_addr.ss_family == AF_INET6) {
        const struct in6_addr *address = &((struct sockaddr_in6 *) &client_addr)->sin6_addr;
        static const uint8_t mapped_prefix[12] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
        };
        if (memcmp(address->s6_addr, mapped_prefix, sizeof(mapped_prefix)) != 0) goto auth_error;
        memcpy(&client_ipv4, &address->s6_addr[12], sizeof(client_ipv4));
    } else {
        goto auth_error;
    }

    wifi_sta_list_t *ap_sta_list = wifi_ap_sta_list();
    wifi_sta_mac_ip_list_t esp_netif_ap_sta_list;
    if (esp_wifi_ap_get_sta_list_with_ip(ap_sta_list, &esp_netif_ap_sta_list) != ESP_OK) {
        goto auth_error;
    }

    for (int i = 0; i < esp_netif_ap_sta_list.num; i++) {
        if (esp_netif_ap_sta_list.sta[i].ip.addr == client_ipv4) return ESP_OK;
    }

auth_error:
    httpd_resp_set_status(req, "401"); // Unauthorized
    char *unauthorized = "401 Unauthorized - Configured to only accept connections from hotspot devices";
    httpd_resp_send(req, unauthorized, strlen(unauthorized));
    return ESP_FAIL;
}

static esp_err_t check_auth(httpd_req_t *req) {
    if (auth_method == AUTH_METHOD_HOTSPOT) return hotspot_auth(req);
    if (auth_method == AUTH_METHOD_BASIC) return basic_auth(req);
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    httpd_resp_set_type(req, "text/plain");

    size_t length;
    void *log_data = log_receive(&length, 1);
    if (log_data == NULL) {
        httpd_resp_sendstr(req, "");

        return ESP_OK;
    }

    httpd_resp_send(req, log_data, length);

    log_return(log_data);

    return ESP_OK;
}

static esp_err_t core_dump_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    size_t core_dump_size = core_dump_available();
    if (core_dump_size == 0) {
        httpd_resp_sendstr(req, "No core dump available");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");

    const esp_app_desc_t *app_desc = esp_app_get_description();

    char elf_sha256[7];
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));

    time_t t = time(NULL);
    char date[20] = "\0";
    if (t > 315360000l) strftime(date, sizeof(date), "_%F_%T", localtime(&t));

    char content_disposition[128];
    snprintf(content_disposition, sizeof(content_disposition),
            "attachment; filename=\"esp32_xbee_%s_core_dump_%s%s.bin\"", app_desc->version, elf_sha256, date);
    httpd_resp_set_hdr(req, "Content-Disposition", content_disposition);

    for (int offset = 0; offset < core_dump_size; offset += BUFFER_SIZE) {
        size_t read = core_dump_size - offset;
        if (read > BUFFER_SIZE) read = BUFFER_SIZE;

        core_dump_read(offset, buffer, read);
        httpd_resp_send_chunk(req, buffer, read);
    }

    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t heap_info_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

    cJSON *root = cJSON_CreateObject();

    cJSON_AddNumberToObject(root, "total_free_bytes", info.total_free_bytes);
    cJSON_AddNumberToObject(root, "total_allocated_bytes", info.total_allocated_bytes);
    cJSON_AddNumberToObject(root, "largest_free_block", info.largest_free_block);
    cJSON_AddNumberToObject(root, "minimum_free_bytes", info.minimum_free_bytes);
    cJSON_AddNumberToObject(root, "allocated_blocks", info.allocated_blocks);
    cJSON_AddNumberToObject(root, "free_blocks", info.free_blocks);
    cJSON_AddNumberToObject(root, "total_blocks", info.total_blocks);

    return json_response(req, root);
}

static esp_err_t file_check_etag_hash(httpd_req_t *req, char *file_hash_path, char *etag, size_t etag_size) {
    struct stat file_hash_stat;
    if (stat(file_hash_path, &file_hash_stat) == -1) {
        // Hash file not created yet
        return ESP_ERR_NOT_FOUND;
    }

    FILE *fd_hash = fopen(file_hash_path, "r+");

    // Ensure hash file was opened
    ERROR_ACTION(TAG, fd_hash == NULL, return ESP_FAIL,
            "Could not open hash file %s (%lu bytes) for reading/updating: %d %s", file_hash_path,
            file_hash_stat.st_size, errno, strerror(errno));

    // Read existing hash
    uint32_t crc;
    int read = fread(&crc, sizeof(crc), 1, fd_hash);
    fclose(fd_hash);
    ERROR_ACTION(TAG, read != 1, return ESP_FAIL,
            "Could not read hash file %s: %d %s", file_hash_path,
            errno, strerror(errno));

    snprintf(etag, etag_size, "\"%08lX\"", crc);

    // Compare to header sent by client
    size_t if_none_match_length = httpd_req_get_hdr_value_len(req, "If-None-Match") + 1;
    if (if_none_match_length > 1) {
        char *if_none_match = malloc(if_none_match_length);
        if (!if_none_match) {
            ESP_LOGE(TAG, "Failed to allocate memory for If-None-Match header");
            return ESP_ERR_NO_MEM;
        }
        httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, if_none_match_length);

        bool header_match = strcmp(etag, if_none_match) == 0;

        // Matching ETag, return not modified
        if (header_match) {
            free(if_none_match);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "ETag for file %s sent by client does not match (%s != %s)", file_hash_path, etag, if_none_match);
            free(if_none_match);
            return ESP_ERR_INVALID_CRC;
        }
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t file_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    ESP_LOGI(TAG, "File request for URI: %s", req->uri);

    char file_path[FILE_PATH_MAX - strlen(FILE_HASH_SUFFIX)];
    char file_hash_path[FILE_PATH_MAX];
    FILE *fd = NULL, *fd_hash = NULL;
    struct stat file_stat;

    // Extract filename from URL
    char *file_name = get_path_from_uri(file_path, WWW_PARTITION_PATH, req->uri, sizeof(file_path));
    ERROR_ACTION(TAG, file_name == NULL, {
        ESP_LOGE(TAG, "Filename too long for URI: %s", req->uri);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }, "Filename too long")
    
    ESP_LOGI(TAG, "Extracted file path: %s", file_path);

    // If name has trailing '/', respond with index page
    if (file_name[strlen(file_name) - 1] == '/' && strlen(file_name) + strlen("index.html") < FILE_PATH_MAX) {
        strlcat(file_name, "index.html", FILE_PATH_MAX);

        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }

    set_content_type_from_file(req, file_name);

    // Check if file exists
    ESP_LOGI(TAG, "Checking if file exists: %s", file_path);
    ERROR_ACTION(TAG, stat(file_path, &file_stat) == -1, {
        ESP_LOGE(TAG, "File not found: %s (errno: %d, %s)", file_path, errno, strerror(errno));
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }, "Could not stat file %s", file_path)
    
    ESP_LOGI(TAG, "File found: %s (%ld bytes)", file_path, file_stat.st_size);

    // Check file hash (if matches request, file is not modified) - безопасная копия
    strlcpy(file_hash_path, file_path, sizeof(file_hash_path));
    strlcat(file_hash_path, FILE_HASH_SUFFIX, sizeof(file_hash_path));
    char etag[8 + 2 + 1] = ""; // Store CRC32, quotes and \0
    if (file_check_etag_hash(req, file_hash_path, etag, sizeof(etag)) == ESP_OK) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    if (strlen(etag) > 0) httpd_resp_set_hdr(req, "ETag", etag);

    fd = fopen(file_path, "r");
    ERROR_ACTION(TAG, fd == NULL, {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not read file");
        return ESP_FAIL;
    }, "Could not read file %s", file_path)

    ESP_LOGI(TAG, "Sending file %s (%ld bytes)...", file_name, file_stat.st_size);

    // Retrieve the pointer to scratch buffer for temporary storage
    size_t length;
    uint32_t crc = 0;
    do {
        // Read file in chunks into the scratch buffer
        length = fread(buffer, 1, BUFFER_SIZE, fd);

        // Send the buffer contents as HTTP response chunk
        if (httpd_resp_send_chunk(req, buffer, length) != ESP_OK) {
            ESP_LOGE(TAG, "Failed sending file %s", file_name);
            httpd_resp_sendstr_chunk(req, NULL);

            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");

            fclose(fd);
            return ESP_FAIL;
        }

        // Update checksum
        crc = crc32_le(crc, (const uint8_t *)buffer, length);
    } while (length != 0);

    // Close file after sending complete
    fclose(fd);

    // Store CRC hash
    fd_hash = fopen(file_hash_path, "w");
    if (fd_hash != NULL) {
        fwrite(&crc, sizeof(crc), 1, fd_hash);
        fclose(fd_hash);
    } else {
        ESP_LOGW(TAG, "Could not open hash file %s for writing: %d %s", file_hash_path, errno, strerror(errno));
    }

    return ESP_OK;
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", app_desc->version);

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    for (int i = 0; i < config_item_count; i++) {
        const config_item_t *item = &config_items[i];

        int64_t int64 = 0;
        uint64_t uint64 = 0;

        size_t length = 0;
        char *string = NULL;

        config_color_t color;
        esp_ip4_addr_t ip;

        switch (item->type) {
            case CONFIG_ITEM_TYPE_STRING:
            case CONFIG_ITEM_TYPE_BLOB:
                // Get length
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, NULL, &length));
                string = calloc(1, length + 1);

                // Get value
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_str_blob(item, string, &length));
                string[length] = '\0';
                break;
            case CONFIG_ITEM_TYPE_COLOR:
                // Convert to hex
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &color));
                asprintf(&string, "#%02x%02x%02x", color.values.red, color.values.green, color.values.blue);
                break;
            case CONFIG_ITEM_TYPE_IP:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &ip));
                cJSON *ip_parts = cJSON_AddArrayToObject(root, item->key);
                for (int b = 0; b < 4; b++) {
                    cJSON_AddItemToArray(ip_parts, cJSON_CreateNumber(esp_ip4_addr_get_byte(&ip, b)));
                }

                break;
            case CONFIG_ITEM_TYPE_UINT8:
            case CONFIG_ITEM_TYPE_UINT16:
            case CONFIG_ITEM_TYPE_UINT32:
            case CONFIG_ITEM_TYPE_UINT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &uint64));
                asprintf(&string, "%llu", uint64);
                break;
            case CONFIG_ITEM_TYPE_BOOL:
            case CONFIG_ITEM_TYPE_INT8:
            case CONFIG_ITEM_TYPE_INT16:
            case CONFIG_ITEM_TYPE_INT32:
            case CONFIG_ITEM_TYPE_INT64:
                ESP_ERROR_CHECK_WITHOUT_ABORT(config_get_primitive(item, &int64));
                asprintf(&string, "%lld", int64);
                break;
            default:
                string = calloc(1, 1);
                break;
        }

        if (string != NULL) {
            // Hide secret values that aren't empty
            char *value = item->secret && strlen(string) > 0 ? CONFIG_VALUE_UNCHANGED : string;
            cJSON_AddStringToObject(root, item->key, value);

            free(string);
        }
    }

    return json_response(req, root);
}

static bool json_int64(cJSON *entry, int64_t *value) {
    if (cJSON_IsBool(entry)) {
        *value = cJSON_IsTrue(entry) ? 1 : 0;
        return true;
    }
    if (cJSON_IsNumber(entry)) {
        int64_t integer = (int64_t) entry->valuedouble;
        if ((double) integer != entry->valuedouble) return false;
        *value = integer;
        return true;
    }
    if (!cJSON_IsString(entry) || !entry->valuestring) return false;

    char *end;
    errno = 0;
    long long integer = strtoll(entry->valuestring, &end, 10);
    if (errno != 0 || end == entry->valuestring || *end != '\0') return false;
    *value = (int64_t) integer;
    return true;
}

static bool json_uint64(cJSON *entry, uint64_t *value) {
    int64_t signed_value;
    if (!json_int64(entry, &signed_value) || signed_value < 0) return false;
    *value = (uint64_t) signed_value;
    return true;
}

static esp_err_t config_process_entry(const config_item_t *item, cJSON *entry, bool apply) {
    size_t length = 0;
    if (cJSON_IsString(entry)) {
        length = strlen(entry->valuestring);

        // Empty primitive and masked secret values mean "leave unchanged".
        if ((length == 0 && item->type != CONFIG_ITEM_TYPE_BLOB &&
             item->type != CONFIG_ITEM_TYPE_STRING) ||
            strcmp(entry->valuestring, CONFIG_VALUE_UNCHANGED) == 0) {
            return ESP_OK;
        }
    }

    if (item->type >= CONFIG_ITEM_TYPE_MAX) return ESP_ERR_INVALID_ARG;

    if (item->type == CONFIG_ITEM_TYPE_STRING) {
        if (!cJSON_IsString(entry)) return ESP_ERR_INVALID_ARG;
        return apply ? config_set_str(item->key, entry->valuestring) : ESP_OK;
    }
    if (item->type == CONFIG_ITEM_TYPE_BLOB) {
        if (!cJSON_IsString(entry)) return ESP_ERR_INVALID_ARG;
        return apply ? config_set_blob(item->key, entry->valuestring, length) : ESP_OK;
    }
    if (item->type == CONFIG_ITEM_TYPE_COLOR) {
        if (!cJSON_IsString(entry) || strlen(entry->valuestring) != 7 ||
            entry->valuestring[0] != '#') {
            return ESP_ERR_INVALID_ARG;
        }

        char *end;
        unsigned long rgb = strtoul(entry->valuestring + 1, &end, 16);
        if (*end != '\0' || rgb > 0xffffffUL) return ESP_ERR_INVALID_ARG;

        config_color_t color = {.rgba = (uint32_t) rgb << 8u};
        if (rgb != 0) color.values.alpha = item->def.color.values.alpha;
        return apply ? config_set_color(item->key, color) : ESP_OK;
    }
    if (item->type == CONFIG_ITEM_TYPE_IP) {
        uint8_t octets[4];
        if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) != 4) {
            return ESP_ERR_INVALID_ARG;
        }
        for (int i = 0; i < 4; i++) {
            uint64_t octet;
            if (!json_uint64(cJSON_GetArrayItem(entry, i), &octet) || octet > 255) {
                return ESP_ERR_INVALID_ARG;
            }
            octets[i] = (uint8_t) octet;
        }
        uint32_t ip = esp_netif_htonl(esp_netif_ip4_makeu32(
                octets[0], octets[1], octets[2], octets[3]));
        return apply ? config_set_u32(item->key, ip) : ESP_OK;
    }

    int64_t signed_value;
    uint64_t unsigned_value;
    switch (item->type) {
        case CONFIG_ITEM_TYPE_BOOL:
            if (!json_int64(entry, &signed_value) ||
                (signed_value != 0 && signed_value != 1)) return ESP_ERR_INVALID_ARG;
            return apply ? config_set_bool1(item->key, signed_value == 1) : ESP_OK;
        case CONFIG_ITEM_TYPE_INT8:
            if (!json_int64(entry, &signed_value) || signed_value < INT8_MIN || signed_value > INT8_MAX)
                return ESP_ERR_INVALID_ARG;
            return apply ? config_set_i8(item->key, (int8_t) signed_value) : ESP_OK;
        case CONFIG_ITEM_TYPE_INT16:
            if (!json_int64(entry, &signed_value) || signed_value < INT16_MIN || signed_value > INT16_MAX)
                return ESP_ERR_INVALID_ARG;
            return apply ? config_set_i16(item->key, (int16_t) signed_value) : ESP_OK;
        case CONFIG_ITEM_TYPE_INT32:
            if (!json_int64(entry, &signed_value) || signed_value < INT32_MIN || signed_value > INT32_MAX)
                return ESP_ERR_INVALID_ARG;
            return apply ? config_set_i32(item->key, (int32_t) signed_value) : ESP_OK;
        case CONFIG_ITEM_TYPE_INT64:
            if (!json_int64(entry, &signed_value)) return ESP_ERR_INVALID_ARG;
            return apply ? config_set_i64(item->key, signed_value) : ESP_OK;
        case CONFIG_ITEM_TYPE_UINT8:
            if (!json_uint64(entry, &unsigned_value) || unsigned_value > UINT8_MAX)
                return ESP_ERR_INVALID_ARG;
            return apply ? config_set_u8(item->key, (uint8_t) unsigned_value) : ESP_OK;
        case CONFIG_ITEM_TYPE_UINT16:
            if (!json_uint64(entry, &unsigned_value) || unsigned_value > UINT16_MAX)
                return ESP_ERR_INVALID_ARG;
            return apply ? config_set_u16(item->key, (uint16_t) unsigned_value) : ESP_OK;
        case CONFIG_ITEM_TYPE_UINT32:
            if (!json_uint64(entry, &unsigned_value) || unsigned_value > UINT32_MAX)
                return ESP_ERR_INVALID_ARG;
            return apply ? config_set_u32(item->key, (uint32_t) unsigned_value) : ESP_OK;
        case CONFIG_ITEM_TYPE_UINT64:
            if (!json_uint64(entry, &unsigned_value)) return ESP_ERR_INVALID_ARG;
            return apply ? config_set_u64(item->key, unsigned_value) : ESP_OK;
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    if (req->content_len == 0 || req->content_len > 8192) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid configuration size");
        return ESP_ERR_INVALID_SIZE;
    }

    char *request_body = malloc(req->content_len + 1);
    if (!request_body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, request_body + received, req->content_len - received);
        if (ret <= 0) {
            free(request_body);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        received += (size_t) ret;
    }
    request_body[received] = '\0';

    cJSON *root = cJSON_Parse(request_body);
    free(request_body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    int config_item_count;
    const config_item_t *config_items = config_items_get(&config_item_count);
    const char *invalid_key = NULL;
    for (int i = 0; i < config_item_count; i++) {
        const config_item_t *item = &config_items[i];
        cJSON *entry = cJSON_GetObjectItem(root, item->key);
        if (entry && config_process_entry(item, entry, false) != ESP_OK) {
            invalid_key = item->key;
            break;
        }
    }

    if (invalid_key) {
        char message[96];
        snprintf(message, sizeof(message), "Invalid value for %s", invalid_key);
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, message);
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < config_item_count; i++) {
        const config_item_t *item = &config_items[i];
        cJSON *entry = cJSON_GetObjectItem(root, item->key);
        if (!entry) continue;

        esp_err_t err = config_process_entry(item, entry, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stage configuration value for %s: %s",
                     item->key, esp_err_to_name(err));
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Failed to save configuration");
            return err;
        }
    }

    cJSON_Delete(root);

    esp_err_t commit_error = config_commit();
    if (commit_error != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save configuration");
        return commit_error;
    }
    config_restart();

    root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);

    return json_response(req, root);
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();

    // Uptime
    cJSON_AddNumberToObject(root, "uptime", (int) ((double) esp_timer_get_time() / 1000000));

    // Heap
    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    cJSON_AddNumberToObject(heap, "total", heap_caps_get_total_size(MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(heap, "free", heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // Streams
    cJSON *streams = cJSON_AddObjectToObject(root, "streams");
    stream_stats_values_t values;
    for (stream_stats_handle_t stats = stream_stats_first(); stats != NULL; stats = stream_stats_next(stats)) {
        stream_stats_values(stats, &values);

        cJSON *stream = cJSON_AddObjectToObject(streams, values.name);
        cJSON *total = cJSON_AddObjectToObject(stream, "total");
        cJSON_AddNumberToObject(total, "in", values.total_in);
        cJSON_AddNumberToObject(total, "out", values.total_out);
        cJSON *rate = cJSON_AddObjectToObject(stream, "rate");
        cJSON_AddNumberToObject(rate, "in", values.rate_in);
        cJSON_AddNumberToObject(rate, "out", values.rate_out);
    }

    // Sockets
    cJSON *sockets = cJSON_AddArrayToObject(root, "sockets");
    for (int s = LWIP_SOCKET_OFFSET; s < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; s++) {
        int err;

        int socktype;
        socklen_t socktype_len = sizeof(socktype);
        err = getsockopt(s, SOL_SOCKET, SO_TYPE, &socktype, &socktype_len);
        if (err < 0) continue;

        cJSON *socket = cJSON_CreateObject();

        cJSON_AddStringToObject(socket, "type", SOCKTYPE_NAME(socktype));

        struct sockaddr_in6 addr;
        socklen_t socklen = sizeof(addr);

        err = getsockname(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "local", sockaddrtostr((struct sockaddr *) &addr));

        err = getpeername(s, (struct sockaddr *)&addr, &socklen);
        if (err == 0) cJSON_AddStringToObject(socket, "peer", sockaddrtostr((struct sockaddr *) &addr));

        cJSON_AddItemToArray(sockets, socket);
    }

    // WiFi
    wifi_ap_status_t ap_status;
    wifi_sta_status_t sta_status;

    wifi_ap_status(&ap_status);
    wifi_sta_status(&sta_status);

    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");

    cJSON *ap = cJSON_AddObjectToObject(wifi, "ap");
    cJSON_AddBoolToObject(ap, "active", ap_status.active);
    if (ap_status.active) {
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_status.ssid);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_status.authmode));
        cJSON_AddNumberToObject(ap, "devices", ap_status.devices);

        char ip[40];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ap_status.ip4_addr));
        cJSON_AddStringToObject(ap, "ip4", ip);
        snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(ap_status.ip6_addr));
        cJSON_AddStringToObject(ap, "ip6", ip);
    }

    cJSON *sta = cJSON_AddObjectToObject(wifi, "sta");
    cJSON_AddBoolToObject(sta, "active", sta_status.active);
    if (sta_status.active) {
        cJSON_AddBoolToObject(sta, "connected", sta_status.connected);
        if (sta_status.connected) {
            cJSON_AddStringToObject(sta, "ssid", (char *) sta_status.ssid);
            cJSON_AddStringToObject(sta, "authmode", wifi_auth_mode_name(sta_status.authmode));
            cJSON_AddNumberToObject(sta, "rssi", sta_status.rssi);

            char ip[40];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&sta_status.ip4_addr));
            cJSON_AddStringToObject(sta, "ip4", ip);
            snprintf(ip, sizeof(ip), IPV6STR, IPV62STR(sta_status.ip6_addr));
            cJSON_AddStringToObject(sta, "ip6", ip);
        }
    }

    return json_response(req, root);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    uint16_t ap_count;
    wifi_ap_record_t *ap_records =  wifi_scan(&ap_count);

    cJSON *root = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        wifi_ap_record_t *ap_record = &ap_records[i];
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddItemToArray(root, ap);
        cJSON_AddStringToObject(ap, "ssid", (char *) ap_record->ssid);
        cJSON_AddNumberToObject(ap, "rssi", ap_record->rssi);
        cJSON_AddStringToObject(ap, "authmode", wifi_auth_mode_name(ap_record->authmode));
    }

    free(ap_records);

    return json_response(req, root);
}

static esp_err_t sd_log_status_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    bool enabled = sd_logger_is_enabled();
    cJSON_AddBoolToObject(root, "enabled", enabled);
    
    return json_response(req, root);
}

static esp_err_t sd_log_toggle_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char buffer[256];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    buffer[ret] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *enabled_item = cJSON_GetObjectItem(root, "enabled");
    if (!enabled_item || !cJSON_IsBool(enabled_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled field");
        return ESP_FAIL;
    }

    bool enabled = cJSON_IsTrue(enabled_item);
    esp_err_t err = enabled ? sd_logger_init() : sd_logger_enable(false);
    if (err == ESP_OK) err = config_set_bool1(KEY_CONFIG_SD_LOGGING_ACTIVE, enabled);
    if (err == ESP_OK) err = config_commit();

    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to toggle SD logging: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to initialize SD card");
        return err;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddBoolToObject(resp, "enabled", enabled);
    
    return json_response(req, resp);
}

static esp_err_t serial_command_post_handler(httpd_req_t *req) {
    if (check_auth(req) == ESP_FAIL) return ESP_FAIL;

    char buffer[512];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        return ESP_FAIL;
    }
    buffer[ret] = '\0';

    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *command = cJSON_GetObjectItem(root, "command");
    if (!command || !cJSON_IsString(command)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command field");
        return ESP_FAIL;
    }

    const char *cmd = cJSON_GetStringValue(command);
    if (strlen(cmd) >= 256) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Command is too long");
        return ESP_ERR_INVALID_SIZE;
    }
    char command_copy[256];
    strlcpy(command_copy, cmd, sizeof(command_copy));

    cJSON_Delete(root);

    // uart_task is the only UART reader. Reading here would steal bytes from
    // NTRIP/socket/SD subscribers, so the command endpoint is asynchronous.
    uart_write(command_copy, strlen(command_copy));
    uart_write("\r\n", 2);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "command", command_copy);
    cJSON_AddStringToObject(resp, "response", "Command sent; response is forwarded through the configured streams");

    return json_response(req, resp);
}

static esp_err_t test_spiffs_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    
    // Test if SPIFFS is mounted
    DIR *dir = opendir("/www");
    if (dir == NULL) {
        httpd_resp_sendstr(req, "ERROR: Cannot open /www directory\n");
        httpd_resp_sendstr(req, "SPIFFS is NOT mounted!\n");
        return ESP_OK;
    }
    
    httpd_resp_sendstr(req, "SPIFFS is mounted!\nFiles in /www:\n\n");
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char line[300];
        snprintf(line, sizeof(line), "- %s\n", entry->d_name);
        httpd_resp_sendstr(req, line);
    }
    closedir(dir);
    
    return ESP_OK;
}

static esp_err_t root_redirect_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "Root path requested, redirecting to /index.html");
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/index.html");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t register_uri_handler(httpd_handle_t server, const char *path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *r)) {
    httpd_uri_t uri_config_get = {
            .uri        = path,
            .method     = method,
            .handler    = handler
    };
    return httpd_register_uri_handler(server, &uri_config_get);
}

static httpd_handle_t web_server_start(void)
{
    config_get_primitive(CONF_ITEM(KEY_CONFIG_ADMIN_AUTH), &auth_method);
    if (auth_method == AUTH_METHOD_BASIC) {
        char *username, *password;
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_USERNAME), (void **) &username);
        config_get_str_blob_alloc(CONF_ITEM(KEY_CONFIG_ADMIN_PASSWORD), (void **) &password);
        basic_authentication = http_auth_basic_header(username, password);
        free(username);
        free(password);
    }

    if (!buffer) {
        buffer = malloc(BUFFER_SIZE);
        if (!buffer) {
            ESP_LOGE(TAG, "Failed to allocate buffer for web server");
            return NULL;
        }
    }

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Увеличиваем число доступных слотов под URI-обработчики: у нас >12 маршрутов
    // иначе httpd_register_uri_handler начнёт возвращать "no slots left" и файловый обработчик "/*" не зарегистрируется
    config.max_uri_handlers = 20;
    config.uri_match_fn = httpd_uri_match_wildcard;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register test endpoint for SPIFFS debugging
        register_uri_handler(server, "/test", HTTP_GET, test_spiffs_handler);
        
        // Register root handler first for explicit / requests
        register_uri_handler(server, "/", HTTP_GET, root_redirect_handler);
        
        register_uri_handler(server, "/config", HTTP_GET, config_get_handler);
        register_uri_handler(server, "/config", HTTP_POST, config_post_handler);
        register_uri_handler(server, "/status", HTTP_GET, status_get_handler);

        register_uri_handler(server, "/log", HTTP_GET, log_get_handler);
        register_uri_handler(server, "/core_dump", HTTP_GET, core_dump_get_handler);
        register_uri_handler(server, "/heap_info", HTTP_GET, heap_info_get_handler);

        register_uri_handler(server, "/wifi/scan", HTTP_GET, wifi_scan_get_handler);
        register_uri_handler(server, "/serial/send", HTTP_POST, serial_command_post_handler);
        register_uri_handler(server, "/sdlog/status", HTTP_GET, sd_log_status_handler);
        register_uri_handler(server, "/sdlog/toggle", HTTP_POST, sd_log_toggle_handler);

        // Wildcard handler for all files - MUST be last
        register_uri_handler(server, "/*", HTTP_GET, file_get_handler);
    }

    if (server == NULL) {
        ESP_LOGE(TAG, "Could not start server");
        free(buffer);
        buffer = NULL;
        return NULL;
    }

    return server;
}

void web_server_init() {
    esp_err_t ret = www_spiffs_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS initialization failed! Web interface will not work.");
        ESP_LOGE(TAG, "Error: %s (%d)", esp_err_to_name(ret), ret);
        // Continue anyway to start the server
    } else {
        ESP_LOGI(TAG, "SPIFFS initialized successfully");
    }
    
    httpd_handle_t server = web_server_start();
    if (server == NULL) {
        ESP_LOGE(TAG, "Failed to start web server");
    } else {
        ESP_LOGI(TAG, "Web server started successfully");
    }
}
