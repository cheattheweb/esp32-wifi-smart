#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "driver/gpio.h"

/** DEFINES **/
#define WIFI_SUCCESS 1 << 0
#define WIFI_FAILURE 1 << 1
#define TCP_SUCCESS 1 << 0
#define TCP_FAILURE 1 << 1
#define MAX_FAILURES 10
#define GPIO_PIN 23 



/** GLOBALS **/

// event group to contain status information
static EventGroupHandle_t wifi_event_group;

// variable to store the IP address

esp_ip4_addr_t ip_address;
char ip_address_str[16];
// retry tracker
static int s_retry_num = 0;

// task tag
static const char *TAG = "WIFI";

// message to send
const char *off_message = "HTTP/1.1 200 OK\r\n"
"Server: esp32\r\n"
"Content-Type: text/html\r\n"
"Content-Length: 51\r\n"
"Connection: close\r\n"
"\r\n"
"<html><body><h1>Turned off the light</h1></body></html>\r\n";

const char *on_message = "HTTP/1.1 200 OK\r\n"
"Server: esp32\r\n"
"Content-Type: text/html\r\n"
"Content-Length: 50\r\n"
"Connection: close\r\n"
"\r\n"
"<html><body><h1>Turned on the light</h1></body></html>\r\n";

char html_page[1024];

void generate_html_page(const char *ip_address) {
    snprintf(html_page, sizeof(html_page),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>\r\n"
        "<html>\r\n"
        "<head>\r\n"
        "    <title>Control Page</title>\r\n"
        "    <style>\r\n"
        "        .button {\r\n"
        "            display: inline-block;\r\n"
        "            padding: 15px 25px;\r\n"
        "            font-size: 24px;\r\n"
        "            cursor: pointer;\r\n"
        "            text-align: center;\r\n"
        "            text-decoration: none;\r\n"
        "            outline: none;\r\n"
        "            color: #fff;\r\n"
        "            background-color: #4CAF50;\r\n"
        "            border: none;\r\n"
        "            border-radius: 15px;\r\n"
        "            box-shadow: 0 9px #999;\r\n"
        "        }\r\n"
        "        .button:hover {background-color: #3e8e41}\r\n"
        "        .button:active {\r\n"
        "            background-color: #3e8e41;\r\n"
        "            box-shadow: 0 5px #666;\r\n"
        "            transform: translateY(4px);\r\n"
        "        }\r\n"
        "    </style>\r\n"
        "</head>\r\n"
        "<body>\r\n"
        "    <a href=\"http://%s/turnmeon\" class=\"button\" target=\"_blank\">Turn On</a>\r\n"
        "    <a href=\"http://%s/turnmeoff\" class=\"button\" target=\"_blank\">Turn Off</a>\r\n"
        "</body>\r\n"
        "</html>\r\n",
        ip_address, ip_address);
}
/** FUNCTIONS **/


//event handler for wifi events
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		ESP_LOGI(TAG, "Connecting to AP...");
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		if (s_retry_num < MAX_FAILURES)
		{
			ESP_LOGI(TAG, "Reconnecting to AP...");
			esp_wifi_connect();
			s_retry_num++;
		} else {
			xEventGroupSetBits(wifi_event_group, WIFI_FAILURE);
		}
	}
}

//event handler for ip events
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
	if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ip_address = event->ip_info.ip;
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_SUCCESS);
    }

}

// connect to wifi and return the result
esp_err_t connect_wifi()
{
	int status = WIFI_FAILURE;

	/** INITIALIZE ALL THE THINGS **/
	//initialize the esp network interface
	ESP_ERROR_CHECK(esp_netif_init());

	//initialize default esp event loop
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	//create wifi station in the wifi driver
	esp_netif_create_default_wifi_sta();

	//setup wifi station with the default wifi configuration
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /** EVENT LOOP CRAZINESS **/
	wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_t wifi_handler_event_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &wifi_handler_event_instance));

    esp_event_handler_instance_t got_ip_event_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &ip_event_handler,
                                                        NULL,
                                                        &got_ip_event_instance));

    /** START THE WIFI DRIVER **/
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "SelfWifi",
            .password = "password_esp32",
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // set the wifi controller to be a station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );

    // set the wifi config
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );

    // start the wifi driver
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "STA initialization complete");

    /** NOW WE WAIT **/
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
            WIFI_SUCCESS | WIFI_FAILURE,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_SUCCESS) {
        ESP_LOGI(TAG, "Connected to ap");
        status = WIFI_SUCCESS;
    } else if (bits & WIFI_FAILURE) {
        ESP_LOGI(TAG, "Failed to connect to ap");
        status = WIFI_FAILURE;
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
        status = WIFI_FAILURE;
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_event_instance));
    vEventGroupDelete(wifi_event_group);

    return status;
}

// connect to the server and return the result
esp_err_t connect_tcp_server(void)
{
    //setup the gpio
    gpio_set_direction(GPIO_PIN, GPIO_MODE_OUTPUT);

	struct sockaddr_in serverInfo = {0};        // Server address information
	char readBuffer[1024] = {0};    

	serverInfo.sin_family = AF_INET; // IPv4
	serverInfo.sin_addr.s_addr = 0x0;  // Server IP Address
	serverInfo.sin_port = htons(80); //  Port    


	int sock = socket(AF_INET, SOCK_STREAM, 0);     // Create socket    
	if (sock < 0) //    Check socket                                         
	{
		ESP_LOGE(TAG, "Failed to create a socket..?");
		return TCP_FAILURE;
	}
    // Connect to server
    if(bind(sock, (struct sockaddr *)&serverInfo, sizeof(serverInfo)) != 0) //    Check socket,port
    {
        ESP_LOGE(TAG, "Failed to bind socket,port %d", 80);
        close(sock);
        return TCP_FAILURE;
    }
    if(listen(sock, 0) != 0){
        ESP_LOGE(TAG, "Failed to listen socket,port");
        close(sock);
        return TCP_FAILURE;
    }

    while(1){
        struct sockaddr c_info = {0};
        uint32_t c_size = 0;
        int cfd = accept(sock,&c_info,&c_size);
        if(cfd < 0){
            ESP_LOGE(TAG, "Failed to accept client");
            close(sock);
            return TCP_FAILURE;
        }
        ESP_LOGI(TAG, "Client connected");
        ip4addr_ntoa_r(&ip_address, ip_address_str, sizeof(ip_address_str));
        generate_html_page(ip_address_str);

        int n = 1;
        while(n > 0){
            ESP_LOGE(TAG, "LOOP STARTED");
            n = read(cfd, readBuffer, sizeof(readBuffer));
            ESP_LOGE(TAG,"%s",readBuffer);

            if(strstr(readBuffer, "index.html"))
            {
                ESP_LOGI(TAG, "index html");
                write(cfd, html_page,strlen(html_page));
                n = 0;
            }

            if(strstr(readBuffer, "turnmeon"))
            {
                ESP_LOGI(TAG, "Turning on the light");
                gpio_set_level(GPIO_PIN, 1);
                write(cfd, on_message, strlen(on_message));
                n = 0;
            }
            else if(strstr(readBuffer, "turnmeoff"))
            {
                ESP_LOGI(TAG, "Turning off the light");
                gpio_set_level(GPIO_PIN, 0);
                write(cfd, off_message, strlen(off_message));
                n = 0;
            }
            else
            {
                ESP_LOGI(TAG, "Unknown command");
            }
        }
        close(cfd);

    }

    close(sock);
    return TCP_SUCCESS;
}

void app_main(void)
{
	esp_err_t status = WIFI_FAILURE;

	//initialize storage
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // connect to wireless AP
	status = connect_wifi();
	if (WIFI_SUCCESS != status)
	{
		ESP_LOGI(TAG, "Failed to associate to AP, dying...");
		return;
	}
	
	status = connect_tcp_server();
	if (TCP_SUCCESS != status)
	{
		ESP_LOGI(TAG, "Failed to connect to remote server, dying...");
		return;
	}
}
