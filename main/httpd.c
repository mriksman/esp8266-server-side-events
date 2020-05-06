#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"

#include "esp_http_server.h"
#include "esp_ota_ops.h"

#include "esp_app_format.h"

static const char *TAG = "httpd";

#define SCRATCH_BUFSIZE  512


uint8_t flash_status = 0;

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
	// Unsucessful Flashing
	flash_status = -1;
    esp_err_t err;



    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);;

    assert(next != NULL);


    // File cannot be larger than partition size
    if (req->content_len > next->size) {

        /**
         * Cannot use this. As soon as ESP_FAIL is called, 
         * the socket is closed before the response is sent.
         * See https://github.com/espressif/esp-idf/issues/5008
         * A commit in ESP-IDF was meant to fix it, but it doesn't work
         * 
         * #ifdef CONFIG_IDF_TARGET_ESP32
         * httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
         *                  "File size must be less than 576KB!");
         * #else
         * #define STR "File size must be less than 576KB!"
         * httpd_resp_set_status(req, HTTPD_400);
         * httpd_resp_send(req, STR, strlen(STR));
         * #undef STR
         * #endif
         **/

        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);

        // Return failure to close underlying connection else the
        // incoming file content will continue to be sent
        return ESP_FAIL;
    }

    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;

    err = esp_ota_begin(next, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "esp_ota_begin succeeded");

 
    /* Content length of the request gives the size of the file being uploaded */
    int remaining = req->content_len;

    int received;
    char buf[SCRATCH_BUFSIZE];

    while (remaining > 0) {


        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, SCRATCH_BUFSIZE))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }
            ESP_LOGE(TAG, "File reception failed!");
            return ESP_FAIL;
        }

        if (remaining > req->content_len - 2000 || remaining < 2000)
            ESP_LOGI(TAG, "Rem:%d, Data:%.512s", received, buf);

        err = esp_ota_write( update_handle, (const void *)buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            return ESP_FAIL;
        }
        
        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }


    ESP_LOGI(TAG, "Complete");
//    httpd_resp_send(req, STR, strlen(STR));


    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        }
//        ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        return ESP_FAIL;
    }

/*
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    ESP_LOGI(TAG, "Running partition label %s (offset 0x%08x) size %dKB",
             running->label, running->address, running->size/1024);
    ESP_LOGI(TAG, "Configured Boot partition label %s (offset 0x%08x) size %dKB",
             configured->label, configured->address, configured->size/1024);
    ESP_LOGI(TAG, "Next partition label %s (offset 0x%08x) size %dKB",
             next->label, next->address, next->size/1024);
*/


/*
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(next, &app_desc);

    #define HASH_LEN 32 
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; i++) {
        for (int shift = 0; shift < 2; shift++) {
            uint8_t nibble = (app_desc.app_elf_sha256[i] >> (shift ? 0 : 4)) & 0x0F;
            if (nibble < 10) {
                hash_print[i * 2 + shift] = '0' + nibble;
            } else {
                hash_print[i * 2 + shift] = 'a' + nibble - 10;
            }
        }
    }

    ESP_LOGI(TAG, "Magic word %d, App version %.32s, Proj Name %.32s, SHA256 %s",
             app_desc.magic_word, app_desc.version, app_desc.project_name, hash_print);
*/




    return ESP_OK;
}




/* URI handler for uploading files to server */
httpd_uri_t file_upload = {
    .uri       = "/update",
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
    .user_ctx  = NULL  
};





httpd_handle_t start_webserver(void)
{


    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
 
        httpd_register_uri_handler(server, &file_upload);

        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

void stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    httpd_stop(server);
}


