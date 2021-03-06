#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "nvs_flash.h"
#include "esp_wifi.h"

#include "esp_log.h"
static const char *TAG = "main";

#define OTA_LISTEN_PORT 80
#define OTA_BUFF_SIZE 1024
#define MAX_SSE_CLIENTS 2

// needs to be accessible in sse_task if a client closes
static fd_set master_set;
// set volatile as a client could be removed at any time and the sse_task needs to make sure it has an updated copy
volatile int sse_sockets[MAX_SSE_CLIENTS];

#define LOG_BUF_MAX_LINE_SIZE 120
char log_buf[LOG_BUF_MAX_LINE_SIZE];
static QueueHandle_t q_sse_message_queue;
static putchar_like_t old_function = NULL;


int sse_logging_putchar(int chr) {
    if(chr == '\n'){
        // send without the '\n'
        xQueueSendToBack(q_sse_message_queue, log_buf, 0);
        // 'clear' string
        log_buf[0] = '\0';    
    } else {
        size_t len = strlen(log_buf);
        if (len < LOG_BUF_MAX_LINE_SIZE - 1) {
            log_buf[len] = chr;
            log_buf[len+1] = '\0';
        }
    }
    // still send to console
	return old_function( chr );
}

esp_err_t read_from_client (int client_fd) {
    char buffer[OTA_BUFF_SIZE] = {0};
    int len;

    len = read(client_fd, buffer, OTA_BUFF_SIZE);
    if (len < 0) {
        // Read error
        ESP_LOGE(TAG, "client read err %s", strerror(errno));
        return -1; 
    }
    else if (len == 0) {
        // Normal connection close from client
        ESP_LOGI(TAG, "client %d close connection", client_fd); 
        return -1;
    }
    else {
        // Data read   
        const char *method_start_p = buffer;
        const char *method_end_p = strstr(method_start_p, " ");
        const char *uri_start_p = method_end_p + 1;

        const char *content_length_start = "Content-Length: ";
        char *content_length_start_p = strstr(buffer, content_length_start);
        if (content_length_start_p != NULL) {
            int content_length = -1;
            sscanf(content_length_start_p + strlen(content_length_start), "%d", &content_length);
            ESP_LOGI(TAG, "Detected content length: %d", content_length);   
        }

        const char *header_end = "\r\n\r\n";
        char *body_start_p = strstr(buffer, header_end) + strlen(header_end);
        int body_part_len = len - (body_start_p - buffer);

        ESP_LOGI(TAG, "Read %d. Body Len %d. Header Length (including \\r\\n\\r\\n) %d", len, body_part_len, (body_start_p - buffer)); 

        ESP_LOGI(TAG, "\r\n%s", buffer); 

        if (  strncmp(method_start_p, "GET", strlen("GET")) == 0 && 
              strncmp(uri_start_p, "/event", strlen("/event")) == 0    ) {
            // disable sending to sse_socket until a proper HTTP 200 OK response has been sent back to client
            old_function = esp_log_set_putchar(old_function);

            int i;
            for (i = 0; i < MAX_SSE_CLIENTS; i++) {
                if (sse_sockets[i] == 0) {
                    sse_sockets[i] = client_fd;
                    ESP_LOGI(TAG, "sse_socket: %d slot %d ", sse_sockets[i], i);
                    break;
                }
            }
            if (i == MAX_SSE_CLIENTS) {
                len = sprintf(buffer, "HTTP/1.1 503 Server Busy\r\n\r\n");
                send(client_fd, buffer, len, 0);
                return -1; // close connection
            }

            len = sprintf(buffer, "HTTP/1.1 200 OK\r\n"
                                  "Connection: Keep-Alive\r\n"
                                  "Content-Type: text/event-stream\r\n"
                                  "Cache-Control: no-cache\r\n\r\n");
            send(client_fd, buffer, len, 0);
            
            // enable sse logging again
            old_function = esp_log_set_putchar(old_function);   
        }
        else if (    strncmp(method_start_p, "GET", strlen("GET")) == 0 && 
                     strncmp(uri_start_p, "/ ", strlen("/ ")) == 0          ) {

            extern const char sse_html_start[] asm("_binary_sse_html_gz_start");
            extern const char sse_html_end[] asm("_binary_sse_html_gz_end");

            len = sprintf(buffer, "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Encoding: gzip\r\n\r\n", (sse_html_end-sse_html_start));
            send(client_fd, buffer, len, 0);
            send(client_fd, sse_html_start, (sse_html_end-sse_html_start), 0);
        }
        else {
            len = sprintf(buffer, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            send(client_fd, buffer, len, 0); 
        }
    } //else
    return 0; // success
}

static void sse_task(void * param)
{
    const char *sse_begin = "data: ";
    const char *sse_end_message = "\n\n";
    char recv_buf[LOG_BUF_MAX_LINE_SIZE + strlen(sse_begin) + strlen(sse_end_message)];
    strcpy(recv_buf, sse_begin);
    int return_code;

    while(1) {
        if (xQueueReceive(q_sse_message_queue, recv_buf + strlen(sse_begin), portMAX_DELAY) == pdTRUE) {
            strcat(recv_buf, sse_end_message);

            for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
                if (sse_sockets[i] != 0) {

                    return_code = send(sse_sockets[i], recv_buf, strlen(recv_buf), 0);

                    if (return_code < 0) {
                        close(sse_sockets[i]);
                        FD_CLR(sse_sockets[i], &master_set);
                        sse_sockets[i] = 0;
                    }
                }
            } //for
        } // if
    } //while
}

static void socket_server_task(void * param)
{
    int return_code;

    int server_socket = 0;
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        ESP_LOGE(TAG, "socket err %s", strerror(errno));
//      quit task;
    }
    ESP_LOGI(TAG, "server socket. port %d. server_fd %d", OTA_LISTEN_PORT, server_socket);

    // Set socket to be nonblocking.
    int on = 1;
    return_code = ioctl(server_socket, FIONBIO, (char *)&on);
    if (return_code < 0) {
        ESP_LOGE(TAG, "ioctl err %s", strerror(errno));
        close(server_socket);
//      quit task;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(OTA_LISTEN_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "bind err %s", strerror(errno));
        close(server_socket);
//      quit task;
    }
    if (listen(server_socket, 5) < 0) {
        ESP_LOGE(TAG, "listen err %s", strerror(errno));
        close(server_socket);
//      quit task;
    }

    struct sockaddr_in client_addr;
    size_t client_addrlen = sizeof(client_addr);

    fd_set working_set;
    // Initialize the set of active sockets (i.e. the server socket)
    FD_ZERO (&master_set);
    FD_SET (server_socket, &master_set);

    while (1) {
        memcpy(&working_set, &master_set, sizeof(master_set));

        ESP_LOGW(TAG, "waiting on select()");
        // Block until input arrives on one or more active sockets
        if (select(FD_SETSIZE, &working_set, NULL, NULL, NULL) < 0) {
            ESP_LOGE(TAG, "select err %s", strerror(errno));
            break;
        }

        // Service all the sockets with input pending
        for (int fd = 0; fd < FD_SETSIZE; ++fd) {
            if (FD_ISSET (fd, &working_set)) {
                if (fd == server_socket) {
                    int client_fd;
                    client_fd = accept(server_socket, (struct sockaddr *)&client_addr, &client_addrlen);

                    if (client_fd < 0) {
                        ESP_LOGE(TAG, "accept err %s", strerror(errno));
                        break;
                    }
                    
                    ESP_LOGI(TAG, "accept() connect from host %s, port %hu", 
                        inet_ntoa (client_addr.sin_addr),  ntohs (client_addr.sin_port));

                    FD_SET (client_fd, &master_set);                        
                }
                else {
                    // Data arriving on an already-connected socket.
                    if (read_from_client(fd) < 0) {
                        close(fd);
                        FD_CLR (fd, &master_set);
                        for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
                            if (sse_sockets[i] == fd) {
                                sse_sockets[i] = 0;
                            }
                        }
                    }
                }
            }
        } // for FD_SETSIZE
    } // while (1)
}



void app_main(void)
{
    //nvs_flash_erase();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    #ifdef CONFIG_IDF_TARGET_ESP32
        ESP_ERROR_CHECK(esp_netif_init());             // previously tcpip_adapter_init()
        esp_netif_create_default_wifi_sta();
    #elif CONFIG_IDF_TARGET_ESP8266
        tcpip_adapter_init();
    #endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_ap_config = {
        .ap = {
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 4,
        },
    };
    uint8_t mac;
    esp_read_mac(&mac, 1);
    snprintf((char *)wifi_ap_config.ap.ssid, 11, "esp_%02x%02x%02x", (&mac)[3], (&mac)[4], (&mac)[5]);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));       
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_ap_config));

    ESP_ERROR_CHECK(esp_wifi_start());

    // listen and read incoming sockets (HTTP)
    xTaskCreate(&socket_server_task, "socket_server", 4096, NULL, 2, NULL);

    // Task to accept messages from queue and send to SSE clients
    xTaskCreate(&sse_task, "sse", 2048, NULL, 4, NULL);
    q_sse_message_queue = xQueueCreate( 10, sizeof(char)*LOG_BUF_MAX_LINE_SIZE );

    old_function = esp_log_set_putchar(&sse_logging_putchar);

}
