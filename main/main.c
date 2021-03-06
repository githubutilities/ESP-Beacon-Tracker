// Copyright 2015-2017 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// cd build
// ../../toolchain/esp-idf/components/esptool_py/esptool/esptool.py --port /dev/ttyUSB1 write_flash --flash_mode dio --flash_freq 40m --flash_size detect 0x1000 bootloader/bootloader.bin 0x10000 BLE_Tracker.bin 0x8000 partitions.bin

/****************************************************************************
*
* This file is for gatt client. It can scan ble device, connect one device.
* Run the gatt_server demo, the client demo will automatically connect to the gatt_server demo.
* Client demo will enable gatt_server's notify after connection. Then the two devices will exchange
* data.
*
****************************************************************************/

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "controller.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "mqtt.h"

#include "bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

#include "fota.h"

#define TAG_TRACKER "TRACKER"
#define TAG_WIFI "WIFI"
#define TAG_MQTT "MQTT"
#define REMOTE_SERVICE_UUID        0x00FF
#define REMOTE_NOTIFY_CHAR_UUID    0xFF01
#define PROFILE_NUM      1
#define PROFILE_A_APP_ID 0
#define INVALID_HANDLE   0

#define SCAN_FREQUENCY_MS 30000
#define SCAN_DURATION_S   3


static bool connect    = false;
static bool get_server = false;
static esp_gattc_char_elem_t *char_elem_result   = NULL;
static esp_gattc_descr_elem_t *descr_elem_result = NULL;

mqtt_client *mqtt_c = NULL;


// FreeRTOS event group to signal when we are connected & ready to send data
EventGroupHandle_t network_event_group;
const int WIFI_CONNECTED = BIT0;
const int MQTT_CONNECTED = BIT1;

// Declare static functions
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);


static esp_bt_uuid_t remote_filter_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = REMOTE_SERVICE_UUID,},
};

static esp_bt_uuid_t remote_filter_char_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = REMOTE_NOTIFY_CHAR_UUID,},
};

static esp_bt_uuid_t notify_descr_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,},
};

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type              = BLE_SCAN_TYPE_PASSIVE,
    .own_addr_type          = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy     = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval          = 0x50,
    .scan_window            = 0x30
};

struct gattc_profile_inst {
    esp_gattc_cb_t gattc_cb;
    uint16_t gattc_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_start_handle;
    uint16_t service_end_handle;
    uint16_t char_handle;
    esp_bd_addr_t remote_bda;
};

/* 
 * Called when MQTT is connected
 */
void connected_cb( mqtt_client *self, mqtt_event_data_t *params ) {
    ESP_LOGI( TAG_MQTT, "Connected" );
    xEventGroupSetBits( network_event_group, MQTT_CONNECTED );
    mqtt_client *client = (mqtt_client *) self;
    mqtt_subscribe( client, "/fota/firmware", 0 );
}

/* 
 * Called when MQTT is disconnected
 */
void disconnected_cb( mqtt_client *self, mqtt_event_data_t *params ) {
    ESP_LOGW( TAG_MQTT, "Disconnected" );
    xEventGroupClearBits( network_event_group, MQTT_CONNECTED );
}

/*
 * TODO
 */
void reconnect_cb( mqtt_client *self, mqtt_event_data_t *params ) {
}

/*
 * Called after each topic subscription
 */
void subscribe_cb( mqtt_client *self, mqtt_event_data_t *params ) {
    ESP_LOGI( TAG_MQTT, "Subscribe OK" );
}

/*
 * Called each time a message is published
 */
void publish_cb( mqtt_client *self, mqtt_event_data_t *params ) {
    ESP_LOGI( TAG_MQTT, "Published" );
}

/*
 * Called for each message received on subscribed topics
 */
void data_cb( mqtt_client *self, mqtt_event_data_t *params ) {
    mqtt_event_data_t *event_data = (mqtt_event_data_t *) params;

    if ( event_data->data_offset == 0 ) { // TODO - Why ?
        char *topic = malloc( event_data->topic_length + 1 );
        memcpy( topic, event_data->topic, event_data->topic_length );
        topic[event_data->topic_length] = 0; //  TODO - Why ?
        ESP_LOGI( TAG_MQTT, "Published on topic: %s", topic );

        char *data = malloc( event_data->data_length + 1 );
        memcpy( data, event_data->data, event_data->data_length );
        data[event_data->data_length] = 0; //  TODO - Why ?
        /*
        ESP_LOGI( TAG_MQTT, "Published data[%d/%d bytes] - %s",
                    event_data->data_length + event_data->data_offset,
                    event_data->data_total_length, data
                );
        */
        if ( strcmp( topic, "/fota/firmware" ) == 0 ) {
            fota_update( data );
        }
        free( data ); // TODO - Avoid on OTA ?
        free( topic );
    }
}

mqtt_settings settings = {
    .host = CONFIG_MQTT_SERVER,
//#if defined(CONFIG_MQTT_SECURITY_ON)
// TODO select CONFIG_MQTT_SECURITY_ON via Menuconfig
    .port = CONFIG_MQTT_PORT,
/*#else    .port = 1883, #endif */
    .client_id = CONFIG_ESP_NAME,
    .username = CONFIG_MQTT_USERNAME,
    .password = CONFIG_MQTT_PASSWORD,
    .clean_session = 0,
    .keepalive = 120,
    .lwt_topic = "/lwt",
    .lwt_msg = "offline",
    .lwt_qos = 0,
    .lwt_retain = 0,
    .connected_cb = connected_cb,
    .disconnected_cb = disconnected_cb,
    // TODO .reconnect_cb = reconnect_cb,
    .subscribe_cb = subscribe_cb,
    .publish_cb = publish_cb,
    .data_cb = data_cb
};

/* One gatt-based profile one app_id and one gattc_if, this array will store the gattc_if returned by ESP_GATTS_REG_EVT */
static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {
        .gattc_cb = gattc_profile_event_handler,
        .gattc_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
};

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG_TRACKER, "REG_EVT");
        esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
        if (scan_ret){
            ESP_LOGE(TAG_TRACKER, "set scan params error, error code = %x", scan_ret);
        }
        break;
    case ESP_GATTC_CONNECT_EVT:{
        ESP_LOGI(TAG_TRACKER, "ESP_GATTC_CONNECT_EVT conn_id %d, if %d", p_data->connect.conn_id, gattc_if);
        gl_profile_tab[PROFILE_A_APP_ID].conn_id = p_data->connect.conn_id;
        memcpy(gl_profile_tab[PROFILE_A_APP_ID].remote_bda, p_data->connect.remote_bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG_TRACKER, "REMOTE BDA:");
        esp_log_buffer_hex(TAG_TRACKER, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, sizeof(esp_bd_addr_t));
        esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req (gattc_if, p_data->connect.conn_id);
        if (mtu_ret){
            ESP_LOGE(TAG_TRACKER, "config MTU error, error code = %x", mtu_ret);
        }
        break;
    }
    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK){
            ESP_LOGE(TAG_TRACKER, "open failed, status %d", p_data->open.status);
            break;
        }
        ESP_LOGI(TAG_TRACKER, "open success");
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status != ESP_GATT_OK){
            ESP_LOGE(TAG_TRACKER,"config mtu failed, error status = %x", param->cfg_mtu.status);
        }
        ESP_LOGI(TAG_TRACKER, "ESP_GATTC_CFG_MTU_EVT, Status %d, MTU %d, conn_id %d", param->cfg_mtu.status, param->cfg_mtu.mtu, param->cfg_mtu.conn_id);
        esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &remote_filter_service_uuid);
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        ESP_LOGI(TAG_TRACKER, "ESP_GATTC_SEARCH_RES_EVT");
        esp_gatt_srvc_id_t *srvc_id =(esp_gatt_srvc_id_t *)&p_data->search_res.srvc_id;
        if (srvc_id->id.uuid.len == ESP_UUID_LEN_16 && srvc_id->id.uuid.uuid.uuid16 == REMOTE_SERVICE_UUID) {
            ESP_LOGI(TAG_TRACKER, "service found");
            get_server = true;
            gl_profile_tab[PROFILE_A_APP_ID].service_start_handle = p_data->search_res.start_handle;
            gl_profile_tab[PROFILE_A_APP_ID].service_end_handle = p_data->search_res.end_handle;
            ESP_LOGI(TAG_TRACKER, "UUID16: %x", srvc_id->id.uuid.uuid.uuid16);
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (p_data->search_cmpl.status != ESP_GATT_OK){
            ESP_LOGE(TAG_TRACKER, "search service failed, error status = %x", p_data->search_cmpl.status);
            break;
        }
        ESP_LOGI(TAG_TRACKER, "ESP_GATTC_SEARCH_CMPL_EVT");
        if (get_server){
            uint16_t count = 0;
            esp_gatt_status_t status = esp_ble_gattc_get_attr_count( gattc_if,
                                                                     p_data->search_cmpl.conn_id,
                                                                     ESP_GATT_DB_CHARACTERISTIC,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                                     INVALID_HANDLE,
                                                                     &count);
            if (status != ESP_GATT_OK){
                ESP_LOGE(TAG_TRACKER, "esp_ble_gattc_get_attr_count error");
            }

            if (count > 0){
                char_elem_result = (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * count);
                if (!char_elem_result){
                    ESP_LOGE(TAG_TRACKER, "gattc no mem");
                }else{
                    status = esp_ble_gattc_get_char_by_uuid( gattc_if,
                                                             p_data->search_cmpl.conn_id,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                             gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                             remote_filter_char_uuid,
                                                             char_elem_result,
                                                             &count);
                    if (status != ESP_GATT_OK){
                        ESP_LOGE(TAG_TRACKER, "esp_ble_gattc_get_char_by_uuid error");
                    }

                    /*  Every service have only one char in our 'ESP_GATTS_DEMO' demo, so we used first 'char_elem_result' */
                    if (count > 0 && (char_elem_result[0].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)){
                        gl_profile_tab[PROFILE_A_APP_ID].char_handle = char_elem_result[0].char_handle;
                        esp_ble_gattc_register_for_notify (gattc_if, gl_profile_tab[PROFILE_A_APP_ID].remote_bda, char_elem_result[0].char_handle);
                    }
                }
                /* free char_elem_result */
                free(char_elem_result);
            }else{
                ESP_LOGE(TAG_TRACKER, "no char found");
            }
        }
         break;
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        ESP_LOGI(TAG_TRACKER, "ESP_GATTC_REG_FOR_NOTIFY_EVT");
        if (p_data->reg_for_notify.status != ESP_GATT_OK){
            ESP_LOGE(TAG_TRACKER, "REG FOR NOTIFY failed: error status = %d", p_data->reg_for_notify.status);
        }else{
            uint16_t count = 0;
            uint16_t notify_en = 1;
            esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count( gattc_if,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                         ESP_GATT_DB_DESCRIPTOR,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                                                         &count);
            if (ret_status != ESP_GATT_OK){
                ESP_LOGE(TAG_TRACKER, "esp_ble_gattc_get_attr_count error");
            }
            if (count > 0){
                descr_elem_result = malloc(sizeof(esp_gattc_descr_elem_t) * count);
                if (!descr_elem_result){
                    ESP_LOGE(TAG_TRACKER, "malloc error, gattc no mem");
                }else{
                    ret_status = esp_ble_gattc_get_descr_by_char_handle( gattc_if,
                                                                         gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                         p_data->reg_for_notify.handle,
                                                                         notify_descr_uuid,
                                                                         descr_elem_result,
                                                                         &count);
                    if (ret_status != ESP_GATT_OK){
                        ESP_LOGE(TAG_TRACKER, "esp_ble_gattc_get_descr_by_char_handle error");
                    }

                    /* Erery char have only one descriptor in our 'ESP_GATTS_DEMO' demo, so we used first 'descr_elem_result' */
                    if (count > 0 && descr_elem_result[0].uuid.len == ESP_UUID_LEN_16 && descr_elem_result[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG){
                        ret_status = esp_ble_gattc_write_char_descr( gattc_if,
                                                                     gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                                     descr_elem_result[0].handle,
                                                                     sizeof(notify_en),
                                                                     (uint8_t *)&notify_en,
                                                                     ESP_GATT_WRITE_TYPE_RSP,
                                                                     ESP_GATT_AUTH_REQ_NONE);
                    }

                    if (ret_status != ESP_GATT_OK){
                        ESP_LOGE(TAG_TRACKER, "esp_ble_gattc_write_char_descr error");
                    }

                    /* free descr_elem_result */
                    free(descr_elem_result);
                }
            }
            else{
                ESP_LOGE(TAG_TRACKER, "decsr not found");
            }

        }
        break;
    }
    case ESP_GATTC_NOTIFY_EVT:
        if (p_data->notify.is_notify) {
             ESP_LOGI(TAG_TRACKER, "ESP_GATTC_NOTIFY_EVT, receive notify value:");
         } else {
             ESP_LOGI(TAG_TRACKER, "ESP_GATTC_NOTIFY_EVT, receive indicate value:");
         }
        esp_log_buffer_hex(TAG_TRACKER, p_data->notify.value, p_data->notify.value_len);
        break;
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (p_data->write.status != ESP_GATT_OK){
            ESP_LOGE(TAG_TRACKER, "write descr failed, error status = %x", p_data->write.status);
            break;
        }
        ESP_LOGI(TAG_TRACKER, "write descr success ");
        uint8_t write_char_data[35];
        for (int i = 0; i < sizeof(write_char_data); ++i)
        {
            write_char_data[i] = i % 256;
        }
        esp_ble_gattc_write_char( gattc_if,
                                  gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                  gl_profile_tab[PROFILE_A_APP_ID].char_handle,
                                  sizeof(write_char_data),
                                  write_char_data,
                                  ESP_GATT_WRITE_TYPE_RSP,
                                  ESP_GATT_AUTH_REQ_NONE);
        break;
    case ESP_GATTC_SRVC_CHG_EVT: {
        esp_bd_addr_t bda;
        memcpy(bda, p_data->srvc_chg.remote_bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG_TRACKER, "ESP_GATTC_SRVC_CHG_EVT, bd_addr:");
        esp_log_buffer_hex(TAG_TRACKER, bda, sizeof(esp_bd_addr_t));
        break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT:
        if (p_data->write.status != ESP_GATT_OK){
            ESP_LOGE(TAG_TRACKER, "write char failed, error status = %x", p_data->write.status);
            break;
        }
        ESP_LOGI(TAG_TRACKER, "write char success ");
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        connect = false;
        get_server = false;
        ESP_LOGI(TAG_TRACKER, "ESP_GATTC_DISCONNECT_EVT, reason = %d", p_data->disconnect.reason);
        break;
    default:
        break;
    }
}

static void esp_ble_gap_start_scanning_wrapper( void * pvParameters )
{
    TickType_t xLastWakeTime;
   
    // Initialise the xLastWakeTime variable with the current time.
    xLastWakeTime = xTaskGetTickCount( );

    while( 1 )
    {
        esp_ble_gap_start_scanning( SCAN_DURATION_S );
        // Wait for the next cycle.
        vTaskDelayUntil( &xLastWakeTime, SCAN_FREQUENCY_MS/portTICK_PERIOD_MS );
    }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    uint8_t *adv_name = NULL;
    uint8_t adv_name_len = 0;
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT: {
        // TODO infinite timeout on return value
        xEventGroupWaitBits(network_event_group, WIFI_CONNECTED | MQTT_CONNECTED, false, true, 0xFFFF);
        ESP_LOGW(TAG_TRACKER, "Starting scan");
        // Create FreeRTOS task
        // TODO - Task stack size
        
        xTaskCreatePinnedToCore(
                &esp_ble_gap_start_scanning_wrapper,  /* Function to call            */
                "scanning_wrapper",                   /* Name - 16 char max          */
                1000,                                 /* Allocated stacks in words   */
                NULL,                                 /* Parameters                  */
                5,                                    /* Priority (Low: 0, High: TBC)*/
                NULL,                                 /* Task handle                 */
                1                                     /* Assigned to app core        */
            );
        break;
    }
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        //scan start complete event to indicate scan start successfully or failed
        if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG_TRACKER, "scan start failed, error status = %x", param->scan_start_cmpl.status);
            break;
        }
        ESP_LOGI(TAG_TRACKER, "scan start success");

        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        esp_ble_gap_cb_param_t *scan_result = (esp_ble_gap_cb_param_t *)param;
        switch (scan_result->scan_rst.search_evt) {
            case ESP_GAP_SEARCH_INQ_RES_EVT:
                esp_log_buffer_hex(TAG_TRACKER, scan_result->scan_rst.bda, 6);
                adv_name = esp_ble_resolve_adv_data(scan_result->scan_rst.ble_adv,
                                                    ESP_BLE_AD_TYPE_NAME_CMPL, &adv_name_len);
                /* From Wikipedia
                *
                * Byte 3: Length: 0x1a
                * Byte 4: Type: 0xff (Custom Manufacturer Packet)
                * Byte 5-6: Manufacturer ID : 0x4c00 (Apple)
                * Byte 7: SubType: 0x2 (iBeacon)
                * Byte 8: SubType Length: 0x15
                * Byte 9-24: Proximity UUID
                * Byte 25-26: Major
                * Byte 27-28: Minor
                * Byte 29: Signal Power
                *
                * Manufacturer IDs
                * https://www.bluetooth.com/specifications/assigned-numbers/company-identifiers
                */
                char payload[512]; // TODO Length
                char name[255];    // Use malloc
                int alt = 0;       // Fix to use Android Beacon Simulator
                if (adv_name_len == 0) {
                    name[0] = '\0';
                } else {
                    name[adv_name_len] = '\0';
                    memcpy(name, (char *)adv_name, adv_name_len);
                }
                if (scan_result->scan_rst.adv_data_len == 30) {
                    alt = 3;
                }
                // TODO split sprintf and/or use memcpy
                sprintf(payload, "{\"EspName\":\"%s\",\"Name\":\"%s\",\"NameLen\":\"%d\",\"RSSI\":\"%d\",\"Length\":\"%d\",\"Type\":\"%02X\",\"ManufacturerID\":\"%02X%02X\",\"Subtype\":\"%02X\",\"SubLength\":\"%02X\",\"UUID\":\"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X\",\"Major\":\"%02X%02X\",\"Minor\":\"%02X%02X\",\"bda\":\"%02X%02X%02X%02X%02X%02X\",\"DeviceType\":\"%d\",\"AdvDataLen\":\"%d\"}",
                        settings.client_id,
                        name,
                        adv_name_len,
                        scan_result->scan_rst.rssi,
                        scan_result->scan_rst.ble_adv[0+alt],
                        scan_result->scan_rst.ble_adv[1+alt],
                        scan_result->scan_rst.ble_adv[2+alt],
                        scan_result->scan_rst.ble_adv[3+alt],
                        scan_result->scan_rst.ble_adv[4+alt],
                        scan_result->scan_rst.ble_adv[5+alt],
                        scan_result->scan_rst.ble_adv[6+alt],
                        scan_result->scan_rst.ble_adv[7+alt],
                        scan_result->scan_rst.ble_adv[8+alt],
                        scan_result->scan_rst.ble_adv[9+alt],
                        scan_result->scan_rst.ble_adv[10+alt],
                        scan_result->scan_rst.ble_adv[11+alt],
                        scan_result->scan_rst.ble_adv[12+alt],
                        scan_result->scan_rst.ble_adv[13+alt],
                        scan_result->scan_rst.ble_adv[14+alt],
                        scan_result->scan_rst.ble_adv[15+alt],
                        scan_result->scan_rst.ble_adv[16+alt],
                        scan_result->scan_rst.ble_adv[17+alt],
                        scan_result->scan_rst.ble_adv[18+alt],
                        scan_result->scan_rst.ble_adv[19+alt],
                        scan_result->scan_rst.ble_adv[20+alt],
                        scan_result->scan_rst.ble_adv[21+alt],
                        scan_result->scan_rst.ble_adv[22+alt],
                        scan_result->scan_rst.ble_adv[23+alt],
                        scan_result->scan_rst.ble_adv[24+alt],
                        scan_result->scan_rst.ble_adv[25+alt],
                        scan_result->scan_rst.bda[0],
                        scan_result->scan_rst.bda[1],
                        scan_result->scan_rst.bda[2],
                        scan_result->scan_rst.bda[3],
                        scan_result->scan_rst.bda[4],
                        scan_result->scan_rst.bda[5],
                        scan_result->scan_rst.dev_type,
                        scan_result->scan_rst.adv_data_len
                );
                ESP_LOGW(TAG_TRACKER, "JSON: %s", payload);
                mqtt_publish(mqtt_c, "/test", payload, strlen(payload), 0, 0);

                ESP_LOGI(TAG_TRACKER, "\n");
                break;
            case ESP_GAP_SEARCH_INQ_CMPL_EVT:
                break;
            default:
                break;
            }
        break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS){
            ESP_LOGE(TAG_TRACKER, "scan stop failed, error status = %x", param->scan_stop_cmpl.status);
            break;
        }
        ESP_LOGI(TAG_TRACKER, "stop scan successfully");
        
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS){
            ESP_LOGE(TAG_TRACKER, "adv stop failed, error status = %x", param->adv_stop_cmpl.status);
            break;
        }
        ESP_LOGI(TAG_TRACKER, "stop adv successfully");
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
         ESP_LOGI(TAG_TRACKER, "update connetion params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.min_int,
                  param->update_conn_params.max_int,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    /* If event is register event, store the gattc_if for each profile */
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
        } else {
            ESP_LOGI(TAG_TRACKER, "reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }

    /* If the gattc_if equal to profile A, call profile A cb handler,
     * so here call each profile's callback */
    do {
        int idx;
        for (idx = 0; idx < PROFILE_NUM; idx++) {
            if (gattc_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
                    gattc_if == gl_profile_tab[idx].gattc_if) {
                if (gl_profile_tab[idx].gattc_cb) {
                    gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
                }
            }
        }
    } while (0);
}


static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    static uint8_t ipLastByte;
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGW(TAG_WIFI, "STAT_START");
        esp_wifi_connect();
        break;
    case SYSTEM_EVENT_STA_GOT_IP: 
        ipLastByte = (uint8_t)(event->event_info.got_ip.ip_info.ip.addr >> 24 & 0xFF);
        ESP_LOGW(TAG_WIFI, "WIFI connected - IP: .%d", ipLastByte);
        xEventGroupSetBits(network_event_group, WIFI_CONNECTED);
        // /!\ Careful, might be more than client_id size;
        itoa(ipLastByte, settings.client_id + strlen(settings.client_id), 10 );
	    mqtt_c = mqtt_start(&settings);
	break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        xEventGroupClearBits(network_event_group, WIFI_CONNECTED | MQTT_CONNECTED);
	mqtt_stop();
	mqtt_c = NULL;
	esp_wifi_connect();
        ESP_LOGW(TAG_WIFI, "STA_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}


static void initialise_wifi(void)
{
    tcpip_adapter_init();
    network_event_group = xEventGroupCreate(); // TODO Move it
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_LOGW(TAG_WIFI, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}




void app_main()
{
    // Initialize NVS.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    initialise_wifi();

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG_TRACKER, "%s initialize controller failed, error code = %x\n", __func__, ret);
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret) {
        ESP_LOGE(TAG_TRACKER, "%s enable controller failed, error code = %x\n", __func__, ret);
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG_TRACKER, "%s init bluetooth failed, error code = %x\n", __func__, ret);
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG_TRACKER, "%s enable bluetooth failed, error code = %x\n", __func__, ret);
        return;
    }

    //register the  callback function to the gap module
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret){
        ESP_LOGE(TAG_TRACKER, "%s gap register failed, error code = %x\n", __func__, ret);
        return;
    }

    //register the callback function to the gattc module
    ret = esp_ble_gattc_register_callback(esp_gattc_cb);
    if(ret){
        ESP_LOGE(TAG_TRACKER, "%s gattc register failed, error code = %x\n", __func__, ret);
        return;
    }

    ret = esp_ble_gattc_app_register(PROFILE_A_APP_ID);
    if (ret){
        ESP_LOGE(TAG_TRACKER, "%s gattc app register failed, error code = %x\n", __func__, ret);
    }
    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret){
        ESP_LOGE(TAG_TRACKER, "set local  MTU failed, error code = %x", local_mtu_ret);
    }

}

