#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_log.h"
#include "esp_http_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "mqtt_client.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "rom/ets_sys.h"

#include "driver/gpio.h"
#include "driver/adc.h"
#include "driver/timer.h"

#include "DHT22.h"
#include "ds18b20.h"
#include "ssd1306.h"

#define salida_RELE_motor 17
static uint8_t s_RELE_motor_state = 0;
#define salida_RELE_luces 16
static uint8_t s_RELE_luces_state = 0;

// Temp Sensors are on GPIO14
#define TEMP_BUS 14
#define LED 2
#define HIGH 1
#define LOW 0
#define digitalWrite gpio_set_level
DeviceAddress tempSensors[2];

/*HTTP buffer*/
#define MAX_HTTP_RECV_BUFFER 1024
#define MAX_HTTP_OUTPUT_BUFFER 2048

/* TAGs for the system*/
static const char *TAG = "HTTP_CLIENT Handler";
static const char *TAG1 = "wifi station";
static const char *TAG2 = "Sending getUpdates";
static const char *TAG3 = "Sending sendMessage";
static const char *TAG4 = "MQTT_EXAMPLE";

/*WIFI configuration*/
#define ESP_WIFI_SSID "adrian"
#define ESP_WIFI_PASS "0987654321a"
#define ESP_MAXIMUM_RETRY 10

/*Telegram configuration*/
#define TOKEN "5904901884:AAHfv333rDY6_uR1cMz-ZqJkApE4mWe5dJ0"
char url_string[512] = "https://api.telegram.org/bot";

char comando[25] = "";
char id_comando[20] = "0";
char tipo_comando[20] = "";
// The chat id that will receive the message
char chat_ID[20] = "";

float hum = 0;
float temp = 0;
float temp_inte = 0;

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

extern const char telegram_certificate_pem_start[] asm("_binary_telegram_certificate_pem_start");
extern const char telegram_certificate_pem_end[] asm("_binary_telegram_certificate_pem_end");

esp_mqtt_client_handle_t cliente_0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG1, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG1, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG1, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false},
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG1, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG1, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG1, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG1, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer; 
    static int output_len;     
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            if (evt->user_data)
            {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            }
            else
            {
                if (output_buffer == NULL)
                {
                    output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            if (output_buffer != NULL)
            {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        break;
    }
    return ESP_OK;
}

// Telgram bot
static void https_telegram_sendMessage_perform_post(void)
{
    char url[512] = "";
    char output_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    esp_http_client_config_t config = {
        .url = "https://api.telegram.org",
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .event_handler = _http_event_handler,
        .cert_pem = telegram_certificate_pem_start,
        .user_data = output_buffer,
    };
    // POST
    ESP_LOGW(TAG3, "Iniciare");
    esp_http_client_handle_t client = esp_http_client_init(&config);
    /* Creating the string of the url*/
    strcat(url, url_string);
    strcat(url, "/sendMessage");
    esp_http_client_set_url(client, url);

    char post_data_telegram[512] = "";
    if (strcmp(comando, "/start") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"Bienvenido, este es un proyecto creado por el grupo GM03 para la asignatura de SBC los comandos disponibles son:\n/help\n/info\n/humedad\n/temperatura_ambiente\n/temperatura_interior\n/estado_luces\n/cambiar_luces\n/estado_chorros\n/cambiar_chorros\n\"}", chat_ID);
    }
    else if (strcmp(comando, "/humedad") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"La humedad en el exterior es de %.2f %%.\n\"}", chat_ID, hum);
    }
    else if (strcmp(comando, "/temperatura_ambiente") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"La temperatura del ambiente es de %.2f degC.\"}", chat_ID, temp);
    }
    else if (strcmp(comando, "/temperatura_interior") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"La temperatura en el interior es de %0.1f C.\"}", chat_ID, temp_inte);
    }
    else if (strcmp(comando, "/info") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"Este es proyecto realizado por:\nAlberto Martinez\nFrancisco Jose Morera\nRaul Iglesias\nAdrian Sotomayor\"}", chat_ID);
    }
    else if (strcmp(comando, "/help") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"Los comandos disponibles son:\n/help\n/info\n/humedad\n/temperatura_ambiente\n/temperatura_interior\n/estado_luces\n/cambiar_luces\n/estado_chorros\n/cambiar_chorros\n\"}", chat_ID);
    }
    else if (strcmp(comando, "/estado_luces") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"El estado de las luces es %d\"}", chat_ID, s_RELE_luces_state);
    }
    else if (strcmp(comando, "/cambiar_luces") == 0)
    {
        s_RELE_luces_state = !s_RELE_luces_state;
        gpio_set_level(salida_RELE_luces, s_RELE_luces_state);
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"El estado de las luces ahora es %d\"}", chat_ID, s_RELE_luces_state);
    }
    else if (strcmp(comando, "/estado_chorros") == 0)
    {
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"El estado de los chorros es %d\"}", chat_ID, s_RELE_motor_state);
    }
    else if (strcmp(comando, "/cambiar_chorros") == 0)
    {
        s_RELE_motor_state = !s_RELE_motor_state;
        gpio_set_level(salida_RELE_motor, s_RELE_motor_state);
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"El estado de los chorros ahora es %d\"}", chat_ID, s_RELE_motor_state);
    }
    else
        sprintf(post_data_telegram, "{\"chat_id\":%s,\"text\":\"FALLO. No se ha entendido el comando.\"}", chat_ID);

    ESP_LOGW(TAG, "El json es: %s", post_data_telegram);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data_telegram, strlen(post_data_telegram));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG3, "HTTP POST Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
        ESP_LOGW(TAG3, "Desde Perform el output es: %s", output_buffer);
    }
    else
    {
        ESP_LOGE(TAG3, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "Limpiare");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG3, "esp_get_free_heap_size: %d", esp_get_free_heap_size());
}
static void https_telegram_getUpdates_perform(void)
{
    char buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    char buffer_AUX[MAX_HTTP_OUTPUT_BUFFER] = {0};
    char url[512] = "";
    esp_http_client_config_t config = {
        .url = "https://api.telegram.org",
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .event_handler = _http_event_handler,
        .cert_pem = telegram_certificate_pem_start,
        .user_data = buffer,
    };
    /* Creating the string of the url*/
    strcat(url, url_string);
    strcat(url, "/getUpdates?limit=1&offset=");
    strcat(url, id_comando);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG2, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
        ESP_LOGW(TAG2, "Desde Perform el output es: %s", buffer);

        strcpy(buffer_AUX, buffer);
        char *text = strstr(buffer_AUX, "update_id");
        if (text != NULL)
        {
            text += 11;
            char *end = strchr(text, ',');
            *end = '\0';

            int aux_update_id = atoi(text);
            aux_update_id = aux_update_id + 1;
            sprintf(text, "%d", aux_update_id);
            strcpy(id_comando, text);
            printf("%s\n", id_comando);

            strcpy(buffer_AUX, buffer);
            char *tipo = strstr(buffer_AUX, "type");
            if (tipo != NULL)
            {
                tipo += 7;
                char *tipo_end = strchr(tipo, '"');
                *tipo_end = '\0';
                strcpy(tipo_comando, tipo);
                printf("%s\n", tipo_comando);
            }

            strcpy(buffer_AUX, buffer);
            char *com = strstr(buffer_AUX, "text");
            if (com != NULL)
            {
                com += 7;
                if (strcmp(tipo_comando, "private") == 0)
                {
                    char *com_end = strchr(com, '"');
                    *com_end = '\0';
                }else{
                    char *com_end = strchr(com, '@');
                    *com_end = '\0';
                }
                strcpy(comando, com);
                printf("%s\n", comando);
            }

            strcpy(buffer_AUX, buffer);
            char *ch_id = strstr(buffer_AUX, "chat");
            if (ch_id != NULL)
            {
                ch_id += 12;
                char *ch_end = strchr(ch_id, ',');
                *ch_end = '\0';
                strcpy(chat_ID, ch_id);
                printf("%s\n", chat_ID);
            }
        }
        else
        {
            strcpy(comando, "");
        }
    }
    else
    {
        ESP_LOGE(TAG2, "Error perform http request %s", esp_err_to_name(err));
    }
    ESP_LOGW(TAG2, "Cerrar Cliente");
    esp_http_client_close(client);
    ESP_LOGW(TAG, "Limpiare");
    esp_http_client_cleanup(client);

    if(strcmp(comando, "") != 0){
    https_telegram_sendMessage_perform_post();
    } else printf("Sin mensajes nuevos\n");
}
static void http_test_task(void *pvParameters)
{
    strcat(url_string, TOKEN);
    ESP_LOGW(TAG, "Wait 1.5 second before start");
    while (1)
    {
    vTaskDelay(1500 / portTICK_PERIOD_MS);

    ESP_LOGW(TAG, "https_telegram_getUpdates_perform");
    https_telegram_getUpdates_perform();
    }
}

// MQTT Thingboard
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG4, "MQTT_EVENT_CONNECTED");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG4, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG4, "MQTT_EVENT_SUBSCRIBED");
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG4, "MQTT_EVENT_UNSUBSCRIBED");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG4, "MQTT_EVENT_PUBLISHED");
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG4, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG4, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG4, "Other event id:%d", event->event_id);
        break;
    }
    return ESP_OK;
}
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG4, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}
static void mqtt_sensor(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "Humedad", hum);
    char *post_data = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(cliente_0, "v1/devices/me/telemetry", post_data, 0, 1, 0);

    cJSON_AddNumberToObject(root, "Temp_Ambiente", temp);
    post_data = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(cliente_0, "v1/devices/me/telemetry", post_data, 0, 1, 0);

    cJSON_AddNumberToObject(root, "Temp_Interior", temp_inte);
    post_data = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(cliente_0, "v1/devices/me/telemetry", post_data, 0, 1, 0);

    cJSON_AddNumberToObject(root, "Estado_Rele_Chorros", s_RELE_motor_state);
    post_data = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(cliente_0, "v1/devices/me/telemetry", post_data, 0, 1, 0);

    cJSON_AddNumberToObject(root, "Estado_Rele_Luces", s_RELE_luces_state);
    post_data = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(cliente_0, "v1/devices/me/telemetry", post_data, 0, 1, 0);
    cJSON_Delete(root);
    free(post_data);    
}
static void mqtt_app_start_task(void *pvParameters)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://demo.thingsboard.io",
        .event_handle = mqtt_event_handler,
        .port = 1883,
        .username = "AqqI0zeVcdzFbiFXVthL",
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    cliente_0 = client;
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
    while (1)
    {
    mqtt_sensor();
    vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

void getTempAddresses(DeviceAddress *tempSensorAddresses)
{
    unsigned int numberFound = 0;
    reset_search();
    // search for 2 addresses on the oneWire protocol
    while (search(tempSensorAddresses[numberFound], true))
    {
        numberFound++;
        if (numberFound == 2)
            break;
    }
    // if 2 addresses aren't found then flash the LED rapidly
    while (numberFound != 2)
    {
        numberFound = 0;
        digitalWrite(LED, HIGH);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        digitalWrite(LED, LOW);
        vTaskDelay(100 / portTICK_PERIOD_MS);
        // search in the loop for the temp sensors as they may hook them up
        reset_search();
        while (search(tempSensorAddresses[numberFound], true))
        {
            numberFound++;
            if (numberFound == 2)
                break;
        }
    }
    return;
}

void DHT_Oled_task(void *pvParameter)
{
    setDHTgpio(GPIO_NUM_27);

    SSD1306_t dev;
    i2c_master_init(&dev, CONFIG_SDA_GPIO, CONFIG_SCL_GPIO, CONFIG_RESET_GPIO);

    ssd1306_init(&dev, 128, 64);
    ssd1306_clear_screen(&dev, false);
    ssd1306_contrast(&dev, 0xff);

    while (1)
    {
        int ret = readDHT();

        errorHandler(ret);

        hum = getHumidity();
        char humidity_ambiente[15];
        sprintf(humidity_ambiente, "Hum %.2f %%", hum);
        printf("Humidity %.2f %%\n", hum);

        temp = getTemperature();
        char temperature_exterior[15];
        sprintf(temperature_exterior, "Ext %.2f C", temp);
        printf("Temperature exterior %.2f C\n", temp);

        temp_inte = ds18b20_get_temp();
        char temperature_agua[15];
        sprintf(temperature_agua, "Int %0.1f C", temp_inte);
        printf("Temperatura interior: %0.1fC\n\n", temp_inte);

        ssd1306_display_text(&dev, 0, humidity_ambiente, 8, false);
        ssd1306_display_text(&dev, 2, temperature_exterior, 12, false);
        ssd1306_display_text(&dev, 4, temperature_agua, 8, false);

        vTaskDelay(3000 / portTICK_RATE_MS);
    }
}

static void configure_reles(void)
{
    gpio_reset_pin(salida_RELE_motor);
    gpio_set_direction(salida_RELE_motor, GPIO_MODE_OUTPUT);

    gpio_reset_pin(salida_RELE_luces);
    gpio_set_direction(salida_RELE_luces, GPIO_MODE_OUTPUT);
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_pad_select_gpio(LED);
    gpio_set_direction(LED, GPIO_MODE_OUTPUT);
    gpio_set_level(LED, 1);

    configure_reles();
    ds18b20_init(TEMP_BUS);

    ESP_LOGI(TAG1, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    xTaskCreate(&http_test_task, "http_test_task", 8192 * 4, NULL, 5, NULL);
    xTaskCreate(&DHT_Oled_task, "DHT_Oled_task", 2048 * 2, NULL, 4, NULL);
    xTaskCreate(&mqtt_app_start_task, "mqtt_app_start_task", 8192 * 4, NULL, 4, NULL);
}