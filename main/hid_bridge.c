#include "hid_bridge.h"
#include <string.h>
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "usb/usb_hid_host.h"
#include "ble_hid_device.h"
#include "web/wifi_manager.h"
#include "utils/storage.h"

#define HID_QUEUE_SIZE 4
#define HID_QUEUE_ITEM_SIZE sizeof(usb_hid_report_t)

static const char *TAG = "HID_BRIDGE";
static StaticQueue_t s_hid_report_queue_struct;
static uint8_t s_hid_report_queue_storage[HID_QUEUE_SIZE * HID_QUEUE_ITEM_SIZE];
static QueueHandle_t s_hid_report_queue = NULL;
static StaticTimer_t s_inactivity_timer_struct;
static StaticSemaphore_t s_ble_stack_mutex_struct;
static TaskHandle_t s_hid_bridge_task_handle = NULL;
static TimerHandle_t s_inactivity_timer = NULL;
static SemaphoreHandle_t s_ble_stack_mutex = NULL;
static bool s_hid_bridge_initialized = false;
static bool s_hid_bridge_running = false;
static bool s_ble_stack_active = true;
static void hid_bridge_task(void *arg);
static void inactivity_timer_callback(TimerHandle_t xTimer);

static int s_inactivity_timeout_ms = 30 * 1000;
static bool s_enable_sleep = true;

static void inactivity_timer_callback(TimerHandle_t xTimer)
{
    if (xSemaphoreTake(s_ble_stack_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to take BLE stack mutex in inactivity timer");
        return;
    }
    
    if (!s_ble_stack_active) {
        xSemaphoreGive(s_ble_stack_mutex);
        return;
    }

    if (!usb_hid_host_device_connected() || !ble_hid_device_connected()) {
        xSemaphoreGive(s_ble_stack_mutex);
        return;
    }

    if (is_wifi_connected()) {
        ESP_LOGI(TAG, "Web stack is active, keeping BLE stack running");
        xSemaphoreGive(s_ble_stack_mutex);
        return;
    }
    
    if (!s_enable_sleep) {
        ESP_LOGI(TAG, "Sleep is disabled in settings, keeping BLE stack running");
        xSemaphoreGive(s_ble_stack_mutex);
        return;
    }
    
    ESP_LOGI(TAG, "No USB HID events for a while, stopping BLE stack");
    
    s_ble_stack_active = false;
    esp_err_t ret = ble_hid_device_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize BLE HID device: %s", esp_err_to_name(ret));
        s_ble_stack_active = true;
    } else {
        ESP_LOGI(TAG, "BLE stack stopped");
        xSemaphoreGive(s_ble_stack_mutex);
        // esp_light_sleep_start();
        return;
    }
    
    xSemaphoreGive(s_ble_stack_mutex);
}

esp_err_t hid_bridge_init(const bool verbose)
{
    if (s_hid_bridge_initialized) {
        ESP_LOGW(TAG, "HID bridge already initialized");
        return ESP_OK;
    }

    int sleep_timeout;
    if (storage_get_int_setting("power.sleepTimeout", &sleep_timeout) == ESP_OK) {
        s_inactivity_timeout_ms = sleep_timeout * 1000; // Convert to milliseconds
        ESP_LOGI(TAG, "Sleep timeout set to %d seconds", sleep_timeout);
    } else {
        ESP_LOGW(TAG, "Failed to get sleep timeout from settings, using default");
    }
    
    bool enable_sleep;
    if (storage_get_bool_setting("power.enableSleep", &enable_sleep) == ESP_OK) {
        s_enable_sleep = enable_sleep;
        ESP_LOGI(TAG, "Sleep %s", enable_sleep ? "enabled" : "disabled");
    } else {
        ESP_LOGW(TAG, "Failed to get enable sleep setting, using default (enabled)");
    }

    s_ble_stack_mutex = xSemaphoreCreateMutexStatic(&s_ble_stack_mutex_struct);
    if (s_ble_stack_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create BLE stack mutex");
        return ESP_ERR_NO_MEM;
    }
    
    s_ble_stack_active = true;
    s_hid_report_queue = xQueueCreateStatic(HID_QUEUE_SIZE,
        HID_QUEUE_ITEM_SIZE, s_hid_report_queue_storage, &s_hid_report_queue_struct);
    if (s_hid_report_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create HID report queue");
        vSemaphoreDelete(s_ble_stack_mutex);
        s_ble_stack_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_inactivity_timer = xTimerCreateStatic(
        "inactivity_timer",
        pdMS_TO_TICKS(s_inactivity_timeout_ms),
        pdFALSE,  // Auto-reload disabled
        NULL,
        inactivity_timer_callback,
        &s_inactivity_timer_struct
    );
    
    if (s_inactivity_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create inactivity timer");
        vQueueDelete(s_hid_report_queue);
        s_hid_report_queue = NULL;
        vSemaphoreDelete(s_ble_stack_mutex);
        s_ble_stack_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = usb_hid_host_init(s_hid_report_queue, verbose);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB HID host: %s", esp_err_to_name(ret));
        xTimerDelete(s_inactivity_timer, 0);
        vQueueDelete(s_hid_report_queue);
        s_hid_report_queue = NULL;
        return ret;
    }

    ret = ble_hid_device_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE HID device: %s", esp_err_to_name(ret));
        usb_hid_host_deinit();
        xTimerDelete(s_inactivity_timer, 0);
        vQueueDelete(s_hid_report_queue);
        s_hid_report_queue = NULL;
        return ret;
    }

    s_hid_bridge_initialized = true;
    ESP_LOGI(TAG, "HID bridge initialized");

    if (xTimerStart(s_inactivity_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start inactivity timer");
    }
    
    return ESP_OK;
}

esp_err_t hid_bridge_deinit(void)
{
    if (!s_hid_bridge_initialized) {
        ESP_LOGW(TAG, "HID bridge not initialized");
        return ESP_OK;
    }

    if (s_hid_bridge_running) {
        hid_bridge_stop();
    }

    if (s_inactivity_timer != NULL) {
        xTimerStop(s_inactivity_timer, 0);
        xTimerDelete(s_inactivity_timer, 0);
        s_inactivity_timer = NULL;
    }

    if (xSemaphoreTake(s_ble_stack_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take BLE stack mutex in deinit");
        return ESP_FAIL;
    }
    
    if (s_ble_stack_active) {
        s_ble_stack_active = false;
        esp_err_t ret = ble_hid_device_deinit();
        if (ret != ESP_OK) {
            s_ble_stack_active = true;
            ESP_LOGE(TAG, "Failed to deinitialize BLE HID device: %s", esp_err_to_name(ret));
            xSemaphoreGive(s_ble_stack_mutex);
            return ret;
        }
    }
    
    xSemaphoreGive(s_ble_stack_mutex);

    esp_err_t ret = usb_hid_host_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize USB HID host: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_hid_report_queue != NULL) {
        vQueueDelete(s_hid_report_queue);
        s_hid_report_queue = NULL;
    }
    
    if (s_ble_stack_mutex != NULL) {
        vSemaphoreDelete(s_ble_stack_mutex);
        s_ble_stack_mutex = NULL;
    }

    s_hid_bridge_initialized = false;
    ESP_LOGI(TAG, "HID bridge deinitialized");
    return ESP_OK;
}

esp_err_t hid_bridge_start(void)
{
    if (!s_hid_bridge_initialized) {
        ESP_LOGE(TAG, "HID bridge not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_hid_bridge_running) {
        ESP_LOGW(TAG, "HID bridge already running");
        return ESP_OK;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(hid_bridge_task, "hid_bridge", 2600, NULL, 14, &s_hid_bridge_task_handle, 1);
    if (task_created != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create HID bridge task");
        return ESP_ERR_NO_MEM;
    }

    s_hid_bridge_running = true;
    ESP_LOGI(TAG, "HID bridge started");
    return ESP_OK;
}

esp_err_t hid_bridge_stop(void)
{
    if (!s_hid_bridge_initialized) {
        ESP_LOGE(TAG, "HID bridge not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_hid_bridge_running) {
        ESP_LOGW(TAG, "HID bridge not running");
        return ESP_OK;
    }

    if (s_hid_bridge_task_handle != NULL) {
        vTaskDelete(s_hid_bridge_task_handle);
        s_hid_bridge_task_handle = NULL;
    }

    s_hid_bridge_running = false;
    ESP_LOGI(TAG, "HID bridge stopped");
    return ESP_OK;
}

static esp_err_t process_keyboard_report(const usb_hid_report_t *report) {
    const uint8_t expected_fields = usb_hid_host_get_num_fields(report->report_id, report->if_id);
    if (expected_fields != report->num_fields) {
        return ESP_OK;
    }

    keyboard_report_t ble_kb_report = {0};
    for (int i = 0; i < report->num_fields; i++) {
        const usb_hid_field_t *field = &report->fields[i];
        const int value = field->values[0];

        if (field->attr.usage_page == HID_USAGE_KEYPAD) {
            if (field->attr.usage >= 0xE0 && field->attr.usage <= 0xE7 && value) {
                ble_kb_report.modifier |= (1 << (field->attr.usage - 0xE0));
            } else if (field->attr.usage <= 0xA4 && value) {
                ble_kb_report.keycodes |= (1UL << field->attr.usage);
            }
        }
    }

    return ble_hid_device_send_keyboard_report(&ble_kb_report);
}

static esp_err_t process_mouse_report(const usb_hid_report_t *report) {
    const uint8_t expected_fields = usb_hid_host_get_num_fields(report->report_id, report->if_id);
    if (expected_fields != report->num_fields) {
        return ESP_OK;
    }

    mouse_report_t ble_mouse_report = {0};
    for (int i = 0; i < report->num_fields; i++) {
        const usb_hid_field_t *field = &report->fields[i];
        const int value = field->values[0];

        if (field->attr.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
            switch (field->attr.usage) {
                case HID_USAGE_X:
                    ble_mouse_report.x = value;
                    break;
                case HID_USAGE_Y:
                    ble_mouse_report.y = value;
                    break;
                case HID_USAGE_WHEEL:
                    ble_mouse_report.wheel = value;
                    break;
                default: break;
            }
        } else if (field->attr.usage_page == HID_USAGE_PAGE_BUTTONS) {
            if (field->attr.usage >= 1 && field->attr.usage <= 8 && value) {
                ble_mouse_report.buttons |= (1 << (field->attr.usage - 1));
            }
        }
    }

    return ble_hid_device_send_mouse_report(&ble_mouse_report);
}

bool hid_bridge_is_ble_paused(void)
{
    return !s_ble_stack_active && usb_hid_host_device_connected();
}

esp_err_t hid_bridge_process_report(const usb_hid_report_t *report)
{
    // ESP_LOGI(TAG, "Received HID report (%d fields)", report->num_fields);
    // for (int i = 0; i < report->num_fields; i++) {
    //     const usb_hid_field_t *field = &report->fields[i];
    //     const char *usage_page_desc = "";
    //     const char *usage_desc = "";
    //
    //     switch (field->attr.usage_page) {
    //         case HID_USAGE_PAGE_GENERIC_DESKTOP:
    //             usage_page_desc = "Generic Desktop";
    //             switch (field->attr.usage) {
    //                 case HID_USAGE_MOUSE: usage_desc = "Mouse"; break;
    //                 case HID_USAGE_KEYBOARD: usage_desc = "Buttons"; break;
    //                 case HID_USAGE_X: usage_desc = "X"; break;
    //                 case HID_USAGE_Y: usage_desc = "Y"; break;
    //                 case HID_USAGE_WHEEL: usage_desc = "Wheel"; break;
    //                 case 0x238: usage_desc = "AC Pan"; break;
    //                 default: usage_desc = "Unknown"; break;
    //             }
    //             break;
    //         case HID_USAGE_KEYPAD:
    //             usage_page_desc = "Keyboard";
    //             if (field->attr.usage >= 0xE0 && field->attr.usage <= 0xE7) {
    //                 usage_desc = "Modifier";
    //             } else if (field->attr.usage <= 0xA4) {
    //                 usage_desc = "Key";
    //             } else {
    //                 usage_desc = "Unknown";
    //             }
    //             break;
    //         default:
    //             usage_page_desc = "Unknown";
    //             usage_desc = "Unknown";
    //             break;
    //     }
    //
    //     ESP_LOGI(TAG, "Field %d: usage_page=%s (0x%x), usage=%s (0x%x), value=%d, logical_min=%d, logical_max=%d",
    //         i, usage_page_desc, field->attr.usage_page, usage_desc, field->attr.usage, field->values[0],
    //         field->attr.logical_min, field->attr.logical_max);
    // }

    if (!s_hid_bridge_initialized) {
        ESP_LOGE(TAG, "HID bridge not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (report == NULL) {
        ESP_LOGE(TAG, "Report is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (s_inactivity_timer != NULL && usb_hid_host_device_connected() && ble_hid_device_connected()) {
        xTimerReset(s_inactivity_timer, 0);
    }
    
    if (!s_ble_stack_active) {
        if (xSemaphoreTake(s_ble_stack_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Failed to take BLE stack mutex in process_report");
            return ESP_FAIL;
        }
        
        if (!s_ble_stack_active) {
            ESP_LOGI(TAG, "USB HID event received, restarting BLE stack");

            s_ble_stack_active = true;
            const esp_err_t ret = ble_hid_device_init();
            if (ret != ESP_OK) {
                s_ble_stack_active = false;
                ESP_LOGE(TAG, "Failed to initialize BLE HID device: %s", esp_err_to_name(ret));
                xSemaphoreGive(s_ble_stack_mutex);
                return ret;
            }
        }
        
        xSemaphoreGive(s_ble_stack_mutex);
    }

    if (!ble_hid_device_connected()) {
        ESP_LOGD(TAG, "BLE HID device not connected");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (report->is_mouse) {
        ret = process_mouse_report(report);
    } else if (report->is_keyboard) {
        ret = process_keyboard_report(report);
    }

    return ret;
}

static void hid_bridge_task(void *arg)
{
    ESP_LOGI(TAG, "HID bridge task started");
    usb_hid_report_t report;
    
    if (s_inactivity_timer != NULL) {
        if (xTimerStart(s_inactivity_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start inactivity timer");
        }
    }
    
    while (1) {
        if (xQueueReceive(s_hid_report_queue, &report, portMAX_DELAY) == pdTRUE) {
            hid_bridge_process_report(&report);
        }
    }
}
