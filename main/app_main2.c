#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
#include <app_wifi.h>
#include <app_hap_setup_payload.h>
#include "esp_event.h"
// libraries for time keeping
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "nvs_flash.h"
#include "dht.h"
#include "hap.h"

#define DHT_PIN GPIO_NUM_4
#define TAG "HomeKit"

static const uint16_t temp_TASK_PRIORITY = 1;
static const uint16_t temp_TASK_STACKSIZE = 4 * 1024;
static const char *temp_TASK_NAME = "hap_garage";

static hap_acc_t *accessory;
static float BME68xtemperature = 28.33;

// Wi-Fi Event Handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP");
    }
}

/* Mandatory identify routine for the accessory.
 * In a real accessory, something like LED blink should be implemented
 * got visual identification
 */
static int light_identify(hap_acc_t *ha)
{
    ESP_LOGI(TAG, "Accessory identified");
    return HAP_SUCCESS;
}

float temperature_sensor_task(hap_char_t *hc, hap_status_t *status_code, void *serv_priv, void *read_priv) {
    float temperature=28.0;
    if (hap_req_get_ctrl_id(read_priv)) {
        ESP_LOGI(TAG, "temp sensor received read from %s", hap_req_get_ctrl_id(read_priv));
    }

    if (!strcmp(hap_char_get_type_uuid(hc), HAP_CHAR_UUID_CURRENT_TEMPERATURE))
    {
        hap_val_t new_val;
        new_val.f = BME68xtemperature;
        hap_char_update_val(hc, &new_val);
        *status_code = HAP_STATUS_SUCCESS;
        ESP_LOGI(TAG,"temp status updated to %0.01f", new_val.f);
    }

    return HAP_SUCCESS;
}

/*
 * An optional HomeKit Event handler which can be used to track HomeKit
 * specific events.
 */
static void temp_hap_event_handler(void* arg, esp_event_base_t event_base, int32_t event, void *data)
{
    switch(event) {
        case HAP_EVENT_PAIRING_STARTED :
            ESP_LOGI(TAG, "Pairing Started");
            break;
        case HAP_EVENT_PAIRING_ABORTED :
            ESP_LOGI(TAG, "Pairing Aborted");
            break;
        case HAP_EVENT_CTRL_PAIRED :
            ESP_LOGI(TAG, "Controller %s Paired. Controller count: %d",
                        (char *)data, hap_get_paired_controller_count());
            break;
        case HAP_EVENT_CTRL_UNPAIRED :
            ESP_LOGI(TAG, "Controller %s Removed. Controller count: %d",
                        (char *)data, hap_get_paired_controller_count());
            break;
        case HAP_EVENT_CTRL_CONNECTED :
            ESP_LOGI(TAG, "Controller %s Connected", (char *)data);
            break;
        case HAP_EVENT_CTRL_DISCONNECTED :
            ESP_LOGI(TAG, "Controller %s Disconnected", (char *)data);
            break;
        case HAP_EVENT_ACC_REBOOTING : {
            char *reason = (char *)data;
            ESP_LOGI(TAG, "Accessory Rebooting (Reason: %s)",  reason ? reason : "null");
            break;
        }
        default:
            /* Silently ignore unknown events */
            break;
    }
}

static void homekit_setup(void *arg) {
    hap_init(HAP_TRANSPORT_WIFI);

    hap_acc_cfg_t cfg = {
        .name = "ESP32-C3 Temperature Monitor",
        .manufacturer = "Espressif",
        .model = "ESP32-C3",
        .serial_num = "001122334455",
        .fw_rev = "1.0.0",
        .pv = "1.1.0",
        .identify_routine = NULL,
        .cid = HAP_CID_SENSOR,
    };

    accessory = hap_acc_create(&cfg);

    /* Add a dummy Product Data */
    uint8_t product_data[] = {'E','S','P','3','2','H','A','P'};
    hap_acc_add_product_data(accessory, product_data, sizeof(product_data));

    /* Add Wi-Fi Transport service required for HAP Spec R16 */
    hap_acc_add_wifi_transport_service(accessory, 0);

    //hap_serv_t *temp_service = hap_serv_create(HAP_SERV_UUID_TEMPERATURE_SENSOR);
    hap_serv_t *temp_service = NULL;
    /*if (!temp_service) {
        ESP_LOGE(TAG, "Failed to create Temperature Service");
        goto light_err;
    }*/

    float temperature = 0.01;
    ESP_LOGI(TAG, "Creating temperature service (current temp: %0.01fC)", temperature);
    /* Create the temp Service. Include the "name" since this is a user visible service  */
    temp_service = hap_serv_temperature_sensor_create(temperature);
    hap_serv_add_char(temp_service, hap_char_name_create("ESP Temperature Sensor"));

    /* Set the read callback for the service */
    hap_serv_set_read_cb(temp_service, temperature_sensor_task);

    hap_acc_add_serv(accessory, temp_service);

    /* Add the Accessory to the HomeKit Database */
    hap_add_accessory(accessory);

    /* Register an event handler for HomeKit specific events */
    esp_event_handler_register(HAP_EVENT, ESP_EVENT_ANY_ID, &temp_hap_event_handler, NULL);

    /* Query the controller count (just for information) */
    ESP_LOGI(TAG, "Accessory is paired with %d controllers", hap_get_paired_controller_count());

    #ifdef CONFIG_HOMEKIT_USE_HARDCODED_SETUP_CODE
    /* Unique Setup code of the format xxx-xx-xxx. Default: 111-22-333 */
    hap_set_setup_code(CONFIG_HOMEKIT_SETUP_CODE);
    /* Unique four character Setup Id. Default: ES32 */
    hap_set_setup_id(CONFIG_HOMEKIT_SETUP_ID);
#ifdef CONFIG_APP_WIFI_USE_WAC_PROVISIONING
    app_hap_setup_payload(CONFIG_HOMEKIT_SETUP_CODE, CONFIG_HOMEKIT_SETUP_ID, true, cfg.cid);
#else
    app_hap_setup_payload(CONFIG_HOMEKIT_SETUP_CODE, CONFIG_HOMEKIT_SETUP_ID, false, cfg.cid);
#endif
#endif

/* mfi is not supported */
    hap_enable_mfi_auth(HAP_MFI_AUTH_NONE);
    /* Initialize Wi-Fi */
    app_wifi_init();

    /* After all the initializations are done, start the HAP core */
    hap_start();
    /* Start Wi-Fi */
    app_wifi_start(portMAX_DELAY);

    /* The task ends here. The read/write callbacks will be invoked by the HAP Framework */
    vTaskDelete(NULL);

}

void app_main() {
    /*esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();

    //dht_init(DHT_PIN, true); // Initialize the DHT sensor*/

    //homekit_setup();
    ESP_LOGI(TAG, "[APP] Startup...");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());


    ESP_LOGI(TAG, "[APP] Creating main thread...");

    xTaskCreate(homekit_setup, temp_TASK_NAME, temp_TASK_STACKSIZE, NULL, temp_TASK_PRIORITY, NULL);

    
}


// Wi-Fi Initialization
void wifi_init(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "Bond",
            .password = "pRnvaNvI@2916",
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

