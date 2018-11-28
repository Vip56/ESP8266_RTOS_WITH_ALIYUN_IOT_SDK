/*
 * Copyright (c) 2014-2018 Alibaba Group. All rights reserved.
 * License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "iot_import.h"
#include "iot_export.h"
#include "esp_smartconfig.h"
#include "driver/uart.h"

/*
 * 如果需要使用smartConfig请取消以下两行的注释
 */
/*
#ifndef USE_SMARTCONFIG
#define USE_SMARTCONFIG
#endif
*/

/*
 * 如果需要使用UART0，还需要查看调试窗口的可以将调试接口转换为UART1，那么仅需要
 * 去掉以下注释即可
 */

#ifndef USE_UART0_SEND_DATA
#define USE_UART0_SEND_DATA
#endif


#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

#define PRODUCT_KEY             CONFIG_PRODUCT_KEY
#define DEVICE_NAME             CONFIG_DEVICE_NAME
#define DEVICE_SECRET           CONFIG_DEVICE_SECRET

#define EX_UART_NUM UART_NUM_0

static EventGroupHandle_t wifi_event_group;
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;
static const char *TAG = "MQTT";

static char __product_key[PRODUCT_KEY_LEN + 1];
static char __device_name[DEVICE_NAME_LEN + 1];
static char __device_secret[DEVICE_SECRET_LEN + 1];

/* These are pre-defined topics */
#define TOPIC_UPDATE            "/"PRODUCT_KEY"/"DEVICE_NAME"/update"
#define TOPIC_ERROR             "/"PRODUCT_KEY"/"DEVICE_NAME"/update/error"
#define TOPIC_GET               "/"PRODUCT_KEY"/"DEVICE_NAME"/get"
#define TOPIC_DATA              "/"PRODUCT_KEY"/"DEVICE_NAME"/data"

/* These are pre-defined topics format*/
#define TOPIC_UPDATE_FMT            "/%s/%s/update"
#define TOPIC_ERROR_FMT             "/%s/%s/update/error"
#define TOPIC_GET_FMT               "/%s/%s/get"
#define TOPIC_DATA_FMT              "/%s/%s/data"

#define MQTT_MSGLEN             (1024)
#define BUF_SIZE                (1024)
#define RD_BUF_SIZE             (1024)
#define REQUEST_TIMEOUT_MS      (2000)
#define KEEPALIVE_INTERVAL_MS   (60000)

#ifdef USE_UART0_SEND_DATA
static QueueHandle_t uart0_queue;
#endif

void mqtt_task(void *pvParameter);

#define EXAMPLE_TRACE(fmt, ...)  \
    do { \
        HAL_Printf("%s|%03d :: ", __func__, __LINE__); \
        HAL_Printf(fmt, ##__VA_ARGS__); \
        HAL_Printf("%s", "\r\n"); \
    } while(0)

#ifdef USE_SMARTCONFIG // # 使用 smartConfig
static void sc_callback(smartconfig_status_t status, void *pdata)
{
	switch(status) {
		case SC_STATUS_WAIT:
			ESP_LOGI(TAG, "SC_STATUS_WAIT");
			break;
        case SC_STATUS_FIND_CHANNEL:
            ESP_LOGI(TAG, "SC_STATUS_FINDING_CHANNEL");
            break;
        case SC_STATUS_GETTING_SSID_PSWD:
            ESP_LOGI(TAG, "SC_STATUS_GETTING_SSID_PSWD");
            break;
        case SC_STATUS_LINK:
            ESP_LOGI(TAG, "SC_STATUS_LINK");
            wifi_config_t *wifi_config = pdata;
            ESP_LOGI(TAG, "SSID:%s", wifi_config->sta.ssid);
            ESP_LOGI(TAG, "PASSWORD:%s", wifi_config->sta.password);
            ESP_ERROR_CHECK( esp_wifi_disconnect() );
            ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, wifi_config) );
            ESP_ERROR_CHECK( esp_wifi_connect() );

            break;
        case SC_STATUS_LINK_OVER:
            ESP_LOGI(TAG, "SC_STATUS_LINK_OVER");
            if (pdata != NULL) {
                uint8_t phone_ip[4] = { 0 };
                memcpy(phone_ip, (uint8_t* )pdata, 4);
                ESP_LOGI(TAG, "Phone ip: %d.%d.%d.%d\n", phone_ip[0], phone_ip[1], phone_ip[2], phone_ip[3]);
            }
            xEventGroupSetBits(wifi_event_group, ESPTOUCH_DONE_BIT);
            break;
        default:
            break;
	}
}

void smartconfig_task(void * parm)
{
	EventBits_t uxBits;
	ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
	ESP_ERROR_CHECK(esp_smartconfig_start(sc_callback));
	while(1) {
		uxBits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
		if(uxBits & CONNECTED_BIT) {
			EXAMPLE_TRACE("WiFi Connected to ap");
		}
		if(uxBits & ESPTOUCH_DONE_BIT) {
			ESP_LOGI(TAG, "smartconfig over");
			esp_smartconfig_stop();
			xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
			xTaskCreate(&mqtt_task, "mqtt_task", 8192, NULL, 5, NULL);
			vTaskDelete(NULL);
		}
	}
}
#endif // # 使用 smartConfig

void event_handle(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
    uintptr_t packet_id = (uintptr_t)msg->msg;
    iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;

    switch (msg->event_type) {
        case IOTX_MQTT_EVENT_UNDEF:
            EXAMPLE_TRACE("undefined event occur.");
            break;

        case IOTX_MQTT_EVENT_DISCONNECT:
            EXAMPLE_TRACE("MQTT disconnect.");
            ESP_ERROR_CHECK( esp_wifi_disconnect() );
            ESP_ERROR_CHECK( esp_wifi_connect() );
            break;

        case IOTX_MQTT_EVENT_RECONNECT:
            EXAMPLE_TRACE("MQTT reconnect.");
            break;

        case IOTX_MQTT_EVENT_SUBCRIBE_SUCCESS:
            EXAMPLE_TRACE("subscribe success, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_SUBCRIBE_TIMEOUT:
            EXAMPLE_TRACE("subscribe wait ack timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_SUBCRIBE_NACK:
            EXAMPLE_TRACE("subscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_UNSUBCRIBE_SUCCESS:
            EXAMPLE_TRACE("unsubscribe success, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_UNSUBCRIBE_TIMEOUT:
            EXAMPLE_TRACE("unsubscribe timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_UNSUBCRIBE_NACK:
            EXAMPLE_TRACE("unsubscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_SUCCESS:
            EXAMPLE_TRACE("publish success, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_TIMEOUT:
            EXAMPLE_TRACE("publish timeout, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_NACK:
            EXAMPLE_TRACE("publish nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_PUBLISH_RECEIVED:
            EXAMPLE_TRACE("topic message arrived but without any related handle: topic=%.*s, topic_msg=%.*s",
                          topic_info->topic_len,
                          topic_info->ptopic,
                          topic_info->payload_len,
                          topic_info->payload);
            break;

        case IOTX_MQTT_EVENT_BUFFER_OVERFLOW:
            EXAMPLE_TRACE("buffer overflow, %s", msg->msg);
            break;

        default:
            EXAMPLE_TRACE("Should NOT arrive here.");
            break;
    }
}

static void message_arrive(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
	iotx_mqtt_topic_info_pt ptopic_info = (iotx_mqtt_topic_info_pt)msg->msg;
	uart_write_bytes(EX_UART_NUM, ptopic_info->payload, ptopic_info->topic_len);
}

int self_mqtt_client(void)
{
	uart_event_t event;
	uint8_t *dtmp = (uint8_t *)malloc(RD_BUF_SIZE);
    int rc = 0, msg_len, cnt = 0;
    void *pclient;
    iotx_conn_info_pt pconn_info;
    iotx_mqtt_param_t mqtt_params;
    iotx_mqtt_topic_info_t topic_msg;
    char msg_pub[128];

    HAL_GetProductKey(__product_key);
    HAL_GetDeviceName(__device_name);
    HAL_GetDeviceSecret(__device_secret);

    /* Device AUTH */
    if (0 != IOT_SetupConnInfo(__product_key, __device_name, __device_secret, (void **)&pconn_info)) {
        EXAMPLE_TRACE("AUTH request failed!");
        rc = -1;
        goto do_exit;
    }

    /* Initialize MQTT parameter */
    memset(&mqtt_params, 0x0, sizeof(mqtt_params));

    mqtt_params.port = pconn_info->port;
    mqtt_params.host = pconn_info->host_name;
    mqtt_params.client_id = pconn_info->client_id;
    mqtt_params.username = pconn_info->username;
    mqtt_params.password = pconn_info->password;
    mqtt_params.pub_key = pconn_info->pub_key;

    mqtt_params.request_timeout_ms = REQUEST_TIMEOUT_MS;
    mqtt_params.clean_session = 0;
    mqtt_params.keepalive_interval_ms = KEEPALIVE_INTERVAL_MS;
    mqtt_params.read_buf_size = MQTT_MSGLEN;
    mqtt_params.write_buf_size = MQTT_MSGLEN;

    mqtt_params.handle_event.h_fp = event_handle;
    mqtt_params.handle_event.pcontext = NULL;

    pclient = IOT_MQTT_Construct(&mqtt_params);
    if (NULL == pclient) {
        EXAMPLE_TRACE("MQTT construct failed");
        rc = -1;
        goto do_exit;
    }

    rc = IOT_MQTT_Subscribe(pclient, TOPIC_GET, IOTX_MQTT_QOS1, message_arrive, NULL);
    if (rc < 0) {
    	IOT_MQTT_Destroy(&pclient);
    	EXAMPLE_TRACE("IOT_MQTT_Subscribe() failed, rc = %d", rc);
    	rc = -1;
    	goto do_exit;
    }

    /* Initialize topic information */
    memset(&topic_msg, 0x0, sizeof(iotx_mqtt_topic_info_t));
    topic_msg.qos = IOTX_MQTT_QOS1;
    topic_msg.retain = 0;
    topic_msg.dup = 0;

    do {
#ifdef USE_UART0_SEND_DATA
    	if(xQueueReceive(uart0_queue, (void *)&event, (portTickType)portMAX_DELAY))
    	{
    		bzero(dtmp, RD_BUF_SIZE);
    		if(event.type == UART_DATA)
    		{
    			uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);
    			msg_len = snprintf(msg_pub, sizeof(msg_pub), (const char*)dtmp);
    			if (msg_len < 0) {
    				EXAMPLE_TRACE("Error occur! Exit program");
    				rc = -1;
    				break;
    			}

    			topic_msg.payload = (void *)msg_pub;
    			topic_msg.payload_len = msg_len;

    			rc = IOT_MQTT_Publish(pclient, TOPIC_DATA, &topic_msg);
    			if (rc < 0) {
    				EXAMPLE_TRACE("error occur when publish");
    				rc = -1;
    				break;
    			}
    			EXAMPLE_TRACE("packet-id=%u", (uint32_t)rc);
    		}
    		else if(event.type == UART_FIFO_OVF)
    		{
    			uart_flush_input(EX_UART_NUM);
    			xQueueReset(uart0_queue);
    		}
    		else if(event.type == UART_BUFFER_FULL)
    		{
    			uart_flush_input(EX_UART_NUM);
    			xQueueReset(uart0_queue);
    		}
    	}
#else
        /* Generate topic message */
        cnt++;
        msg_len = snprintf(msg_pub, sizeof(msg_pub), "{\"attr_name\":\"temperature\", \"attr_value\":\"%d\"}", cnt);
        if (msg_len < 0) {
            EXAMPLE_TRACE("Error occur! Exit program");
            rc = -1;
            break;
        }

        topic_msg.payload = (void *)msg_pub;
        topic_msg.payload_len = msg_len;

        rc = IOT_MQTT_Publish(pclient, TOPIC_DATA, &topic_msg);
        if (rc < 0) {
            EXAMPLE_TRACE("error occur when publish");
            rc = -1;
            break;
        }
        EXAMPLE_TRACE("packet-id=%u, publish topic msg=%s", (uint32_t)rc, msg_pub);
#endif
        IOT_MQTT_Yield(pclient, 200);
        HAL_SleepMs(100);
    } while (1);

    free(dtmp);
    dtmp = NULL;

    IOT_MQTT_Yield(pclient, 200);

    IOT_MQTT_Destroy(&pclient);

do_exit:
    return rc;
}

void mqtt_task(void *pvParameter)
{
    ESP_LOGI(TAG, "MQTT task started...");

    while (1) {
    	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

        IOT_OpenLog("mqtt");
        IOT_SetLogLevel(IOT_LOG_DEBUG);

        HAL_SetProductKey(PRODUCT_KEY);
        HAL_SetDeviceName(DEVICE_NAME);
        HAL_SetDeviceSecret(DEVICE_SECRET);

        ESP_LOGI(TAG, "MQTT client example begin");

        self_mqtt_client();

        IOT_DumpMemoryStats(IOT_LOG_DEBUG);
        IOT_CloseLog();

        ESP_LOGI(TAG, "MQTT client example end");
    }
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
#ifdef USE_SMARTCONFIG
    	xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, NULL);
#else
    	esp_wifi_connect();
#endif
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );

#ifndef USE_SMARTCONFIG
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
#endif

    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );

#ifndef USE_SMARTCONFIG
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
#endif

    ESP_ERROR_CHECK( esp_wifi_start() );
}

/**
 * 使用UART0进行数据通信
 */
void user_uart_init(void)
{
#ifdef USE_UART0_SEND_DATA
	uart_config_t uart_config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE
	};
	uart_param_config(EX_UART_NUM, &uart_config);
	uart_driver_install(EX_UART_NUM, BUF_SIZE * 2, BUF_SIZE * 2, 100, &uart0_queue);
#endif
}

void app_main()
{
    ESP_ERROR_CHECK( nvs_flash_init() );

    initialise_wifi();
    user_uart_init();
    xTaskCreate(&mqtt_task, "mqtt_task", 8192, NULL, 5, NULL);
}
