#include "hid_bridge.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "usb_hid_host.h"
#include "ble_hid_device.h"

static const char *TAG = "HID_BRIDGE";
static QueueHandle_t s_hid_report_queue = NULL;
static TaskHandle_t s_hid_bridge_task_handle = NULL;
static bool s_hid_bridge_initialized = false;
static bool s_hid_bridge_running = false;
static void hid_bridge_task(void *arg);

esp_err_t hid_bridge_init(void)
{
    if (s_hid_bridge_initialized) {
        ESP_LOGW(TAG, "HID bridge already initialized");
        return ESP_OK;
    }

    s_hid_report_queue = xQueueCreate(10, sizeof(usb_hid_report_t));
    if (s_hid_report_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create HID report queue");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = usb_hid_host_init(s_hid_report_queue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize USB HID host: %s", esp_err_to_name(ret));
        vQueueDelete(s_hid_report_queue);
        s_hid_report_queue = NULL;
        return ret;
    }

    ret = ble_hid_device_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE HID device: %s", esp_err_to_name(ret));
        usb_hid_host_deinit();
        vQueueDelete(s_hid_report_queue);
        s_hid_report_queue = NULL;
        return ret;
    }

    s_hid_bridge_initialized = true;
    ESP_LOGI(TAG, "HID bridge initialized");
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

    esp_err_t ret = ble_hid_device_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize BLE HID device: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = usb_hid_host_deinit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinitialize USB HID host: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_hid_report_queue != NULL) {
        vQueueDelete(s_hid_report_queue);
        s_hid_report_queue = NULL;
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

    BaseType_t task_created = xTaskCreatePinnedToCore(
        hid_bridge_task,
        "hid_bridge",
        8192,
        NULL,
        5,
        &s_hid_bridge_task_handle,
        1
    );
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

// HID Usage Pages
#define HID_USAGE_PAGE_GENERIC_DESKTOP  0x01
#define HID_USAGE_PAGE_KEYBOARD         0x07
#define HID_USAGE_PAGE_BUTTON          0x09

// HID Usages
#define HID_USAGE_KEYBOARD             0x06
#define HID_USAGE_MOUSE               0x02
#define HID_USAGE_X                   0x30
#define HID_USAGE_Y                   0x31
#define HID_USAGE_WHEEL               0x38

static esp_err_t process_keyboard_report(usb_hid_report_t *report) {
    keyboard_report_t ble_kb_report = {0};
    
    for (int i = 0; i < report->num_fields; i++) {
        usb_hid_field_t *field = &report->fields[i];
        
        if (field->attr.usage_page == HID_USAGE_PAGE_KEYBOARD) {
            // Handle keyboard keys
            if (field->values && field->attr.report_count > 0) {
                ble_kb_report.keycode = field->values[0]; // First key pressed
            }
        } else if (field->attr.usage_page == HID_USAGE_PAGE_BUTTON) {
            // Handle keyboard modifiers
            if (field->values && field->attr.report_count > 0) {
                ble_kb_report.modifier = field->values[0];
            }
        }
    }
    
    ESP_LOGD(TAG, "Forwarding keyboard report: mod=0x%02x key=0x%02x",
             ble_kb_report.modifier, ble_kb_report.keycode);
    return ble_hid_device_send_keyboard_report(&ble_kb_report);
}

static esp_err_t process_mouse_report(usb_hid_report_t *report) {
    mouse_report_t ble_mouse_report = {0};

    for (int i = 0; i < report->num_fields; i++) {
        usb_hid_field_t *field = &report->fields[i];

        if (field->attr.usage_page == HID_USAGE_PAGE_BUTTON) {
            // Handle mouse buttons
            if (field->values && field->attr.report_count > 0) {
                ble_mouse_report.buttons = field->values[0];
            }
        } else if (field->attr.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
            // Handle mouse movement and wheel
            if (field->values && field->attr.report_count > 0) {
                switch (field->attr.usage) {
                    case HID_USAGE_X:
                        ble_mouse_report.x = field->values[0];
                        break;
                    case HID_USAGE_Y:
                        ble_mouse_report.y = field->values[0];
                        break;
                    case HID_USAGE_WHEEL:
                        ble_mouse_report.wheel = field->values[0];
                        break;
                }
            }
        }
    }

    ESP_LOGD(TAG, "Forwarding mouse report: btn=0x%02x x=%d y=%d wheel=%d",
             ble_mouse_report.buttons, ble_mouse_report.x,
             ble_mouse_report.y, ble_mouse_report.wheel);
    return ble_hid_device_send_mouse_report(&ble_mouse_report);
}

esp_err_t hid_bridge_process_report(usb_hid_report_t *report)
{
    if (!s_hid_bridge_initialized) {
        ESP_LOGE(TAG, "HID bridge not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (report == NULL) {
        ESP_LOGE(TAG, "Report is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_hid_device_connected()) {
        ESP_LOGD(TAG, "BLE HID device not connected, starting advertising");
        ble_hid_device_start_advertising();
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    if (report->num_fields > 0) {
        const usb_hid_field_t *first_field = &report->fields[0];
        
        if (first_field->attr.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
            switch (first_field->attr.usage) {
                case HID_USAGE_KEYBOARD:
                    ret = process_keyboard_report(report);
                    break;
                    
                case HID_USAGE_MOUSE:
                    ret = process_mouse_report(report);
                    break;
                    
                default:
                    ESP_LOGW(TAG, "Unhandled generic desktop usage: 0x%04x", 
                            first_field->attr.usage);
                    break;
            }
        } else if (first_field->attr.usage_page == HID_USAGE_PAGE_KEYBOARD) {
            ret = process_keyboard_report(report);
        } else {
            ESP_LOGW(TAG, "Unhandled usage page: 0x%04x", 
                    first_field->attr.usage_page);
        }
    }

    return ret;
}

static void hid_bridge_task(void *arg)
{
    ESP_LOGI(TAG, "HID bridge task started");
    usb_hid_report_t report;
    while (1) {
        if (xQueueReceive(s_hid_report_queue, &report, portMAX_DELAY) == pdTRUE) {
            if (usb_hid_host_device_connected()) {
                hid_bridge_process_report(&report);
            }
        }
    }
}
