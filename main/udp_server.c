/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include <math.h>

#define PORT CONFIG_EXAMPLE_PORT

static const char *TAG = "udp_server";

// Init AP and wifi connect event
#define EXAMPLE_ESP_WIFI_SSID      "udp_server_ap"
#define EXAMPLE_ESP_WIFI_PASS      "iloveSCU"
#define EXAMPLE_ESP_WIFI_CHANNEL   1
#define EXAMPLE_MAX_STA_CONN       4
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    }
}

void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
            .ap = {
                    .ssid = EXAMPLE_ESP_WIFI_SSID,
                    .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
                    .channel = EXAMPLE_ESP_WIFI_CHANNEL,
                    .password = EXAMPLE_ESP_WIFI_PASS,
                    .max_connection = EXAMPLE_MAX_STA_CONN,
                    .authmode = WIFI_AUTH_WPA_WPA2_PSK
            },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}
// End of init ap
// Config CSI

// Set true to print CSI frequency to serial
#define COUNT_CSI_FREQUENCY 0
#define PRINT_TIMESTAMP 0
// Count CSI data frequency
static uint32_t last_CSI_fre_time = 0;
static uint16_t csi_count = 0;
static QueueHandle_t csi_queue;

static void print_csi_freq_task() {
    while (1)
    {
        printf("csi_count: %u\n", csi_count);
        csi_count = 0;
        vTaskDelay(1000/portTICK_PERIOD_MS);
    }
}

static void _get_subcarrier_csi(const int8_t* csi_data, uint16_t subcarrier_index, int8_t* imagin, int8_t* real)
{
    *imagin = csi_data[2*subcarrier_index];
    *real = csi_data[2*subcarrier_index + 1];
}


static void serial_print_csi_task() {
    uint8_t window_len = 10;
    float win[window_len];
    for(uint8_t i = 0; i < window_len; ++i)
        win[i] = 0;
    uint8_t ptr = 0;

    // UDP Client part. Send CSI data to python UDP Server.
    char *host_ip = "dell-pc.local";
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr("192.168.4.2");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9999);
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct Csi_avg {
        float l_img_avg;
        float l_real_avg;
        float ht_img_avg;
        float ht_real_avg
    } csi_avg;

    while (1)
    {
        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if(sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            vTaskDelay(1000/portTICK_PERIOD_MS);
            continue;
            //break;
        }
        while (1) {
            // Get CSI from queue
            wifi_csi_info_t* data = NULL;
            xQueueReceive(csi_queue, &data, portMAX_DELAY);

            // Discard csi from macbook, only obtain csi from other esp32
            if(data->mac[1] != 75){
                free(data);
                continue;
            }

//            printf("sigmode: %d\nchannel bandwidth:%d\n", data->rx_ctrl.sig_mode, data->rx_ctrl.cwb);
//            printf("stbc: %d\nsecondary_channel: %u\n", data->rx_ctrl.stbc, data->rx_ctrl.secondary_channel);
//            printf("channel: %u\n", data->rx_ctrl.channel);
//            printf("datalen: %u\n", data->len);

            // Calculate Covariance between lltf avg and htltf avg
            if(data->rx_ctrl.sig_mode == 0) // non HT(11bg)mode, only contain lltf
            {
                free(data);
                continue;
            }

            uint16_t csi_length = data->len;
            uint16_t start_subcarrier = data->first_word_invalid ? 2 : 0;

            float ht_img_avg = 0, ht_real_avg = 0;
            float l_img_avg = 0, l_real_avg = 0;
            int8_t img, real;
            uint8_t ht_subc_num, l_subc_num;
            if(data->rx_ctrl.cwb == 1) { // 40MHz bandwidth
                // In 40MHz the lltf subcarrier index is 0-63 or -64~-1, so 64 subcarrier in total
                l_subc_num = 64;
                // In 40MHz the htltf subcarrier index is 0-63, -64-1(none STBC) or 0~60, -60~-1(STBC).
                ht_subc_num = data->rx_ctrl.stbc ? 121:128;
            }
            else { // 20Mhz bandwidth
                l_subc_num = 64;
                if(data->rx_ctrl.secondary_channel == 2)// below
                    ht_subc_num = data->rx_ctrl.stbc ? 63 : 64;
                else if(data->rx_ctrl.secondary_channel == 1)// above
                    ht_subc_num = data->rx_ctrl.stbc ? 62 : 64;
                else // none
                    ht_subc_num = 64;
            }
            // Get average LLTF imagine and real part.
            for(uint16_t i = start_subcarrier; i < l_subc_num; ++i) {
                _get_subcarrier_csi(data->buf, i, &img, &real);
                l_img_avg += ((float)img) / (l_subc_num - start_subcarrier);
                l_real_avg += ((float)real) / (l_subc_num - start_subcarrier);
            }

            // Get average HTltf imagine and real part

            for(uint16_t i = l_subc_num; i < l_subc_num + ht_subc_num; ++i) {
                _get_subcarrier_csi(data->buf, i, &img, &real);
                ht_img_avg += ((float)img) / ht_subc_num;
                ht_real_avg += ((float)real) / ht_subc_num;
            }

            csi_avg.ht_real_avg = ht_real_avg;
            csi_avg.ht_img_avg = ht_img_avg;
            csi_avg.l_img_avg = l_img_avg;
            csi_avg.l_real_avg = l_real_avg;

            // Send data to UDP server
            int err = sendto(sock, &csi_avg, sizeof(csi_avg), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            if(err < 0) {
                ESP_LOGE(TAG, "Error occur during sending: errno: %d", errno);
                free(data);
                if (sock != -1) {
                    ESP_LOGE(TAG, "Shutting down socket and restarting...");
                    shutdown(sock, 0);
                    close(sock);
                }
                break;
            }

            printf("%.2f ", csi_avg.ht_real_avg);
            if(PRINT_TIMESTAMP)
            {
                // Get timestamp from data ctrl field
                // This field is the local time when this packet is received.
                // It is precise only if modem sleep or light sleep is not enabled.
                // Unit: microsecond
                uint32_t timestamp = data->rx_ctrl.timestamp;
                printf("%u ", timestamp);
            }
            printf("\n");

            // Free memory allocated in csi_callback function
            free(data);
        }
    }
    vTaskDelete(NULL);
}

void csi_callback(void* ctx, wifi_csi_info_t* data) {
    /*
     * Best practice:
     * Do NOT do lengthy operation in this callback function.
     * Post necessary data to a lower priority task and handle it.
     * */
    // Count csi frequency
    if(COUNT_CSI_FREQUENCY)
    {
        csi_count++;
    }
    // Copy data into heap memory
    wifi_csi_info_t* data_cp =(wifi_csi_info_t*)malloc(sizeof(*data));
    memcpy(data_cp, data, sizeof(*data));
    // Post data to queue
    xQueueSendToBack(csi_queue, &data_cp, 0);

    //serial_print_csi(data);

    /*
     * About the csi data:
     *   Each channel frequency response of sub-carrier is recorded by two bytes of signed characters.
     * The first one is imaginary part and the second one is real part.
     *
     * The default bandwidth for ESP32 station and AP is HT40, so handle CSI data with HT40 format.
     *
     * */
}

static void csi_init() {
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(&csi_callback, NULL));

    wifi_csi_config_t csicfg;

    csicfg.lltf_en = true;
    csicfg.htltf_en = true;
    csicfg.stbc_htltf2_en = false;
    // Enable to generate htlft data by averaging lltf and ht_ltf data when receiving HT packet.
    // Otherwise, use ht_ltf data directly. Default enabled
    csicfg.ltf_merge_en = false;
    //enable to turn on channel filter to smooth adjacent sub-carrier.
    // Disable it to keep independence of adjacent sub-carrier. Default enabled
    csicfg.channel_filter_en = true;
    csicfg.manu_scale = false;

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csicfg));



    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

static void udp_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;

    while (1) {

        if (addr_family == AF_INET) {
            struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
            dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr_ip4->sin_family = AF_INET;
            dest_addr_ip4->sin_port = htons(PORT);
            ip_protocol = IPPROTO_IP;
        } else if (addr_family == AF_INET6) {
            bzero(&dest_addr.sin6_addr.un, sizeof(dest_addr.sin6_addr.un));
            dest_addr.sin6_family = AF_INET6;
            dest_addr.sin6_port = htons(PORT);
            ip_protocol = IPPROTO_IPV6;
        }

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            break;
        }
        ESP_LOGI(TAG, "Socket created");

#if defined(CONFIG_EXAMPLE_IPV4) && defined(CONFIG_EXAMPLE_IPV6)
        if (addr_family == AF_INET6) {
            // Note that by default IPV6 binds to both protocols, it is must be disabled
            // if both protocols used at the same time (used in CI)
            int opt = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
        }
#endif

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket bound, port %d", PORT);

        while (1) {

            //ESP_LOGI(TAG, "Waiting for data");
            struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

            // Error occurred during receiving
            if (len < 0) {
                ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                break;
            }
            // Data received
            else {
//                // Get the sender's ip address as string
//                if (source_addr.ss_family == PF_INET) {
//                    inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
//                } else if (source_addr.ss_family == PF_INET6) {
//                    inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
//                }

                //rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
                //ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);
                //ESP_LOGI(TAG, "%s", rx_buffer);

                // Turn off data sendback
                //int err = sendto(sock, rx_buffer, len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
//                if (err < 0) {
//                    ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
//                    break;
//                }
            }
        }

        if (sock != -1) {
            ESP_LOGE(TAG, "Shutting down socket and restarting...");
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    csi_queue = xQueueCreate(5, sizeof(wifi_csi_info_t*));
    wifi_init_softap();
    csi_init();
#ifdef CONFIG_EXAMPLE_IPV4
    xTaskCreate(udp_server_task, "udp_server", 4096, (void*)AF_INET, 5, NULL);
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    xTaskCreate(udp_server_task, "udp_server", 4096, (void*)AF_INET6, 5, NULL);
#endif
    xTaskCreate(serial_print_csi_task, "serial_print_csi", 9192, NULL, 1, NULL);
    if(COUNT_CSI_FREQUENCY)
        xTaskCreate(print_csi_freq_task, "print_csi_freq", 2048, NULL, 2, NULL);

}
