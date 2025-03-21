/**
 * @file usb_hid_host.c
 * @brief USB HID Host implementation with full report descriptor support
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb_hid_host.h"
#include "../utils/task_monitor.h"

static const char *TAG = "usb_hid_host";
static QueueHandle_t g_report_queue = NULL;
static QueueHandle_t g_event_queue = NULL;
static bool g_device_connected = false;
static TaskHandle_t g_usb_events_task_handle = NULL;
static TaskHandle_t g_event_task_handle = NULL;
static TaskHandle_t g_stats_task_handle = NULL;
static uint32_t s_current_rps = 0;

#define USB_STATS_INTERVAL_SEC 2

// FreeRTOS synchronization primitives
static portMUX_TYPE g_report_maps_spinlock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t g_report_maps_mutex;

#define MAX_REPORT_FIELDS 48
#define MAX_COLLECTION_DEPTH 8

// Double buffering for report processing
static volatile bool g_isr_buffer_index = 0;
static usb_hid_report_t g_isr_reports[2];
static usb_hid_field_t g_isr_fields[2][MAX_REPORT_FIELDS];
static int g_isr_field_values[2][MAX_REPORT_FIELDS];

typedef struct {
    usb_hid_field_attr_t attr;
    uint16_t bit_offset;
    uint16_t bit_size;
} report_field_info_t;

typedef struct {
    report_field_info_t fields[MAX_REPORT_FIELDS];
    uint8_t num_fields;
    uint16_t total_bits;
    uint8_t report_id;
    uint16_t usage_stack[MAX_REPORT_FIELDS];
    uint8_t usage_stack_pos;
    uint16_t collection_stack[MAX_COLLECTION_DEPTH];
    uint8_t collection_depth;
} report_map_t;

// Map of interface number to report map
static report_map_t g_interface_report_maps[USB_HOST_MAX_INTERFACES] = {0};

// Define named struct types for events
typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
    void *arg;
} device_event_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_interface_event_t event;
    void *arg;
} interface_event_t;

typedef struct {
    enum {
        APP_EVENT_HID_DEVICE,
        APP_EVENT_INTERFACE
    } event_group;
    union {
        device_event_t device_event;
        interface_event_t interface_event;
    };
} hid_event_queue_t;

static void usb_lib_task(void *arg);
static void hid_host_event_task(void *arg);
static void usb_stats_task(void *arg);
static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, hid_host_driver_event_t event, void *arg);
static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, hid_host_interface_event_t event, void *arg);
static void process_device_event(hid_host_device_handle_t hid_device_handle, hid_host_driver_event_t event, void *arg);
static void process_interface_event(hid_host_device_handle_t hid_device_handle, hid_host_interface_event_t event, void *arg);

esp_err_t usb_hid_host_init(QueueHandle_t report_queue) {
    ESP_LOGI(TAG, "Initializing USB HID Host");
    if (report_queue == NULL) {
        ESP_LOGE(TAG, "Invalid report queue parameter");
        return ESP_ERR_INVALID_ARG;
    }

    g_report_queue = report_queue;
    g_report_maps_mutex = xSemaphoreCreateMutex();
    if (g_report_maps_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create report maps mutex");
        return ESP_ERR_NO_MEM;
    }

    g_device_connected = false;
    g_event_queue = xQueueCreate(16, sizeof(hid_event_queue_t));
    if (g_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));

    BaseType_t task_created = xTaskCreatePinnedToCore(hid_host_event_task, "hid_events", 3400, NULL, 13, &g_event_task_handle, 1);
    if (task_created != pdTRUE) {
        vQueueDelete(g_event_queue);
        return ESP_ERR_NO_MEM;
    }

    task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 2600, NULL, 12, &g_usb_events_task_handle, 1);
    if (task_created != pdTRUE) {
        vTaskDelete(g_event_task_handle);
        vQueueDelete(g_event_queue);
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    task_created = xTaskCreatePinnedToCore(usb_stats_task, "usb_stats", 2048, NULL, 5, &g_stats_task_handle, 1);
    if (task_created != pdTRUE) {
        vTaskDelete(g_event_task_handle);
        vTaskDelete(g_usb_events_task_handle);
        vQueueDelete(g_event_queue);
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }

    const hid_host_driver_config_t hid_host_config = {
        .create_background_task = true,
        .task_priority = 12,
        .stack_size = 3200,
        .core_id = 1,
        .callback = hid_host_device_callback,
        .callback_arg = NULL
    };

    esp_err_t ret = hid_host_install(&hid_host_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install HID host driver: %d", ret);
        vTaskDelete(g_usb_events_task_handle);
        usb_host_uninstall();
        return ret;
    }

    ESP_LOGI(TAG, "USB HID Host initialized successfully");
    return ESP_OK;
}

esp_err_t usb_hid_host_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing USB HID Host");
    esp_err_t ret = hid_host_uninstall();
    if (ret != ESP_OK) {
        return ret;
    }

    if (g_event_task_handle != NULL) {
        vTaskDelete(g_event_task_handle);
        g_event_task_handle = NULL;
    }
    if (g_usb_events_task_handle != NULL) {
        vTaskDelete(g_usb_events_task_handle);
        g_usb_events_task_handle = NULL;
    }

    if (g_stats_task_handle != NULL) {
        vTaskDelete(g_stats_task_handle);
        g_stats_task_handle = NULL;
    }

    ret = usb_host_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to uninstall USB host: %d", ret);
    }

    if (g_event_queue != NULL) {
        vQueueDelete(g_event_queue);
        g_event_queue = NULL;
    }

    vSemaphoreDelete(g_report_maps_mutex);
    g_report_queue = NULL;
    g_device_connected = false;
    
    ESP_LOGI(TAG, "USB HID Host deinitialized");
    return ret;
}

bool usb_hid_host_device_connected(void) {
    return g_device_connected;
}

static void parse_report_descriptor(const uint8_t *desc, size_t length, uint8_t interface_num) {
    if (interface_num >= USB_HOST_MAX_INTERFACES) {
        ESP_LOGE(TAG, "Interface number %d exceeds maximum", interface_num);
        return;
    }

    if (xSemaphoreTake(g_report_maps_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take report maps mutex");
        return;
    }

    report_map_t *report_map = &g_interface_report_maps[interface_num];
    uint16_t current_usage_page = 0;
    uint8_t report_size = 0;
    uint8_t report_count = 0;
    int logical_min = 0;
    int logical_max = 0;
    uint16_t current_usage = 0;
    uint16_t usage_minimum = 0;
    uint16_t usage_maximum = 0;
    bool has_usage_range = false;
    uint8_t current_report_id = 0;
    bool is_relative = false;
    
    report_map->num_fields = 0;
    report_map->total_bits = 0;
    report_map->usage_stack_pos = 0;
    report_map->collection_depth = 0;
    report_map->report_id = 0;

    for (size_t i = 0; i < length;) {
        uint8_t item = desc[i++];
        uint8_t item_size = item & 0x3;
        uint8_t item_type = (item >> 2) & 0x3;
        uint8_t item_tag = (item >> 4) & 0xF;
        
        uint32_t data = 0;
        if (item_size > 0) {
            for (uint8_t j = 0; j < item_size && i < length; j++) {
                data |= desc[i++] << (j * 8);
            }
        }

        switch (item_type) {
            case 0: // Main
                switch (item_tag) {
                    case 8: // Input
                    case 9: // Output
                        if (report_map->num_fields < MAX_REPORT_FIELDS) {
                            bool is_constant = (data & 0x01) != 0;
                            bool is_variable = (data & 0x02) != 0;
                            is_relative = (data & 0x04) != 0;
                            bool is_array = !is_variable;

                            // Update report ID if one is set
                            if (current_report_id != 0) {
                                report_map->report_id = current_report_id;
                            }

                            if (is_constant) {
                                // Handle constant/padding field
                                report_field_info_t *field = &report_map->fields[report_map->num_fields];
                                field->attr.usage_page = current_usage_page;
                                field->attr.usage = 0;
                                field->attr.report_size = report_size;
                                field->attr.report_count = report_count;
                                field->attr.logical_min = 0;
                                field->attr.logical_max = 0;
                                field->attr.constant = true;
                                field->attr.variable = false;
                                field->attr.relative = false;
                                field->bit_offset = report_map->total_bits;
                                field->bit_size = report_size * report_count;
                                report_map->total_bits += report_size * report_count;
                                report_map->num_fields++;
                            } else if (is_array && has_usage_range) {
                                // Handle array with usage range
                                report_field_info_t *field = &report_map->fields[report_map->num_fields];
                                field->attr.usage_page = current_usage_page;
                                field->attr.usage = usage_minimum;
                                field->attr.usage_maximum = usage_maximum;
                                field->attr.report_size = report_size;
                                field->attr.report_count = report_count;
                                field->attr.logical_min = logical_min;
                                field->attr.logical_max = logical_max;
                                field->attr.constant = false;
                                field->attr.variable = false;
                                field->attr.relative = is_relative;
                                field->bit_offset = report_map->total_bits;
                                field->bit_size = report_size * report_count;
                                report_map->total_bits += report_size * report_count;
                                report_map->num_fields++;
                            } else if (has_usage_range && is_variable) {
                                // Handle variable items with usage range
                                uint16_t range_size = usage_maximum - usage_minimum + 1;
                                for (uint8_t j = 0; j < report_count && j < range_size && report_map->num_fields < MAX_REPORT_FIELDS; j++) {
                                    report_field_info_t *field = &report_map->fields[report_map->num_fields];
                                    field->attr.usage_page = current_usage_page;
                                    field->attr.usage = usage_minimum + j;
                                    field->attr.report_size = report_size;
                                    field->attr.report_count = 1;
                                    field->attr.logical_min = logical_min;
                                    field->attr.logical_max = logical_max;
                                    field->attr.constant = false;
                                    field->attr.variable = true;
                                    field->attr.relative = is_relative;
                                    field->bit_offset = report_map->total_bits;
                                    field->bit_size = report_size;
                                    report_map->total_bits += report_size;
                                    report_map->num_fields++;
                                }
                            } else {
                                // Handle individual usages
                                for (uint8_t j = 0; j < report_count && report_map->num_fields < MAX_REPORT_FIELDS; j++) {
                                    report_field_info_t *field = &report_map->fields[report_map->num_fields];
                                    field->attr.usage_page = current_usage_page;
                                    
                                    if (report_map->usage_stack_pos > j) {
                                        field->attr.usage = report_map->usage_stack[j];
                                    } else if (report_map->usage_stack_pos > 0) {
                                        field->attr.usage = report_map->usage_stack[report_map->usage_stack_pos - 1];
                                    } else {
                                        field->attr.usage = current_usage;
                                    }
                                    
                                    field->attr.report_size = report_size;
                                    field->attr.report_count = 1;
                                    field->attr.logical_min = logical_min;
                                    field->attr.logical_max = logical_max;
                                    field->attr.constant = false;
                                    field->attr.variable = is_variable;
                                    field->attr.relative = is_relative;
                                    field->bit_offset = report_map->total_bits;
                                    field->bit_size = report_size;
                                    report_map->total_bits += report_size;
                                    report_map->num_fields++;
                                }
                            }

                            // Reset usage tracking after field processing
                            report_map->usage_stack_pos = 0;
                            has_usage_range = false;
                            usage_minimum = 0;
                            usage_maximum = 0;
                        }
                        break;
                    case 10: // Collection
                        if (report_map->collection_depth < MAX_COLLECTION_DEPTH) {
                            report_map->collection_stack[report_map->collection_depth++] = data;
                        }
                        break;
                    case 12: // End Collection
                        if (report_map->collection_depth > 0) {
                            report_map->collection_depth--;
                        }
                        break;
                }
                break;

            case 1: // Global
                switch (item_tag) {
                    case 0: // Usage Page
                        current_usage_page = data;
                        break;
                    case 1: // Logical Minimum
                        if (item_size == 1 && (data & 0x80)) {
                            logical_min = (int8_t)data;
                        } else if (item_size == 2 && (data & 0x8000)) {
                            logical_min = (int16_t)data;
                        } else {
                            logical_min = (int)data;
                        }
                        break;
                    case 2: // Logical Maximum
                        if (item_size == 1 && (data & 0x80)) {
                            logical_max = (int8_t)data;
                        } else if (item_size == 2 && (data & 0x8000)) {
                            logical_max = (int16_t)data;
                        } else {
                            logical_max = (int)data;
                        }
                        break;
                    case 7: // Report Size
                        report_size = data;
                        break;
                    case 8: // Report ID
                        current_report_id = data;
                        break;
                    case 9: // Report Count
                        report_count = data;
                        break;
                }
                break;

            case 2: // Local
                switch (item_tag) {
                    case 0: // Usage
                        if (report_map->usage_stack_pos < MAX_REPORT_FIELDS) {
                            report_map->usage_stack[report_map->usage_stack_pos++] = data;
                        }
                        current_usage = data;
                        break;
                    case 1: // Usage Minimum
                        usage_minimum = data;
                        has_usage_range = true;
                        break;
                    case 2: // Usage Maximum
                        usage_maximum = data;
                        has_usage_range = true;
                        break;
                }
                break;
        }
    }
             
    xSemaphoreGive(g_report_maps_mutex);

    // ESP_LOGI(TAG, "=== Report Descriptor Analysis for Interface %d ===", interface_num);
    // ESP_LOGI(TAG, "");
    // ESP_LOGI(TAG, "Total Fields: %d", report_map->num_fields);
    // ESP_LOGI(TAG, "Total Bits: %d", report_map->total_bits);
    // ESP_LOGI(TAG, "Report ID: %d", report_map->report_id);
    // ESP_LOGI(TAG, "");
    // ESP_LOGI(TAG, "");
    // ESP_LOGI(TAG, "Field Details:");

    // for (int i = 0; i < report_map->num_fields; i++) {
    //     ESP_LOGI(TAG, "");
    //     ESP_LOGI(TAG, "Field %d:", i + 1);

    //     report_field_info_t *field = &report_map->fields[i];

    //     const char* usage_page_name = field->attr.usage_page < sizeof(usage_page_names)/sizeof(usage_page_names[0]) &&
    //         usage_page_names[field->attr.usage_page] ? usage_page_names[field->attr.usage_page] : "Unknown";
    //     ESP_LOGI(TAG, "  Usage Page: 0x%04X (%s)", field->attr.usage_page, usage_page_name);

    //     if (!field->attr.variable && field->attr.usage_maximum > 0) {
    //         // Array field with usage range
    //         ESP_LOGI(TAG, "  Usage Range: 0x%04X-0x%04X", field->attr.usage, field->attr.usage_maximum);
    //         ESP_LOGI(TAG, "  Array Size: %d", field->attr.report_count);
    //     } else {
    //         // Regular field
    //         if (field->attr.usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
    //             const char* usage_name = field->attr.usage < sizeof(usage_names_gendesk)/sizeof(usage_names_gendesk[0]) &&
    //                usage_names_gendesk[field->attr.usage] ? usage_names_gendesk[field->attr.usage] : "Unknown";
    //             ESP_LOGI(TAG, "  Usage: 0x%04X (%s)", field->attr.usage, usage_name);
    //         } else {
    //             ESP_LOGI(TAG, "  Usage: 0x%04X", field->attr.usage);
    //         }
    //     }

    //     ESP_LOGI(TAG, "  Report Size: %d bits", (int) field->attr.report_size);
    //     ESP_LOGI(TAG, "  Variable: %s", field->attr.variable ? "Yes" : "No");
    //     ESP_LOGI(TAG, "  Constant: %s", field->attr.constant ? "Yes" : "No");
    //     ESP_LOGI(TAG, "  Logical Min: %d", field->attr.logical_min);
    //     ESP_LOGI(TAG, "  Logical Max: %d", field->attr.logical_max);
    // }

    // ESP_LOGI(TAG, "");
    // ESP_LOGI(TAG, "=== End of Report Descriptor Analysis ===");
    // ESP_LOGI(TAG, "");
}

static int extract_field_value(const uint8_t *data, uint16_t bit_offset, uint16_t bit_size) {
    if (!data || bit_size == 0 || bit_size > 32) {
        return 0;
    }

    int value = 0;
    uint16_t byte_offset = bit_offset / 8;
    uint8_t bit_shift = bit_offset % 8;
    uint16_t bits_remaining = bit_size;
    
    // Handle single bit case separately for efficiency
    if (bit_size == 1) {
        uint8_t byte_value;
        memcpy(&byte_value, &data[byte_offset], sizeof(uint8_t));
        return (byte_value >> bit_shift) & 0x01;
    }

    // Process multiple bytes
    while (bits_remaining > 0) {
        uint8_t current_byte;
        memcpy(&current_byte, &data[byte_offset], sizeof(uint8_t));
        
        uint8_t bits_to_read = MIN(8 - bit_shift, bits_remaining);
        uint8_t mask = ((1 << bits_to_read) - 1);
        int byte_value = (current_byte >> bit_shift) & mask;
        
        uint8_t shift_amount = bit_size - bits_remaining;
        value |= (byte_value << shift_amount);
        
        bits_remaining -= bits_to_read;
        byte_offset++;
        bit_shift = 0;
    }

    // Sign extend if the value is negative (MSB is 1)
    if (bit_size < 32 && (value & (1 << (bit_size - 1)))) {
        value |= ~((1 << bit_size) - 1);
    }

    return value;
}

static void process_report(hid_host_device_handle_t hid_device_handle, const uint8_t *data, size_t length, uint8_t report_id, uint8_t interface_num) {
    s_current_rps++;
    if (!data || !g_report_queue || length == 0 || length > 64 || interface_num >= USB_HOST_MAX_INTERFACES) {
        ESP_LOGE(TAG, "Invalid parameters: data=%p, queue=%p, len=%d, iface=%u", data, g_report_queue, length, interface_num);
        return;
    }

    if (report_id != 0) {
        data++;
        length--;
    }

    // portENTER_CRITICAL_ISR(&g_report_maps_spinlock);

    const uint8_t current_buffer = g_isr_buffer_index;
    usb_hid_report_t *report = &g_isr_reports[current_buffer];
    usb_hid_field_t *fields = g_isr_fields[current_buffer];
    int *field_values = g_isr_field_values[current_buffer];

    report_map_t *report_map = &g_interface_report_maps[interface_num];
    report->report_id = report_id;
    report->type = USB_HID_FIELD_TYPE_INPUT;
    report->num_fields = report_map->num_fields;
    report->raw_len = MIN(length, sizeof(report->raw));
    report->fields = fields;

    const report_field_info_t * volatile field_info = report_map->fields;
    for (uint8_t i = 0; i < report_map->num_fields; i++, field_info++) {
        field_values[i] = extract_field_value(data, field_info->bit_offset, field_info->bit_size);
        fields[i].attr = field_info->attr;
        fields[i].values = &field_values[i];
    }

    memcpy(report->raw, data, report->raw_len);
    g_isr_buffer_index = !g_isr_buffer_index;
    // portEXIT_CRITICAL_ISR(&g_report_maps_spinlock);
    xQueueSend(g_report_queue, report, pdMS_TO_TICKS(100));
}

// Static allocation for event handling
static hid_event_queue_t g_interface_event = {
    .event_group = APP_EVENT_INTERFACE
};

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg) {
    if (!g_event_queue) {
        return;
    }

    g_interface_event.interface_event = (interface_event_t){
        .handle = hid_device_handle,
        .event = event,
        .arg = arg
    };

    xQueueSend(g_event_queue, &g_interface_event, 0);
}

static hid_event_queue_t g_device_event = {
    .event_group = APP_EVENT_HID_DEVICE
};

static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg) {
    if (!g_event_queue) {
        return;
    }

    g_device_event.device_event = (device_event_t){
        .handle = hid_device_handle,
        .event = event,
        .arg = arg
    };

    xQueueSend(g_event_queue, &g_device_event, 0);
}

static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"
};

static void process_device_event(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg) {
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    if (event == HID_HOST_DRIVER_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "HID Device Connected, proto = %s, subclass = %d", hid_proto_name_str[dev_params.proto], dev_params.sub_class);

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL
        };

        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
        ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_REPORT));
        if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
        }

        // Get and parse report descriptor
        size_t desc_len;
        const uint8_t *desc = hid_host_get_report_descriptor(hid_device_handle, &desc_len);
        if (desc != NULL) {
            ESP_LOGI(TAG, "Got report descriptor, length = %d", desc_len);
            parse_report_descriptor(desc, desc_len, dev_params.iface_num);
        }

        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
        g_device_connected = true;
    } else {
        ESP_LOGI(TAG, "Unknown device event, subclass = %d, proto = %s, iface = %d",
            dev_params.sub_class, hid_proto_name_str[dev_params.proto], dev_params.iface_num);
    }
}

static uint8_t cur_if_evt_data[64] = {0};

static void process_interface_event(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg) {
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;

    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));
    // ESP_LOGD(TAG, "Interface 0x%x: HID event received", dev_params.iface_num);

    switch (event) {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, cur_if_evt_data, sizeof(cur_if_evt_data), &data_length));
            // ESP_LOGD(TAG, "Raw input report data: length=%d", data_length);
            uint8_t report_id = (data_length > 0) ? cur_if_evt_data[0] : 0;
            process_report(hid_device_handle, cur_if_evt_data, data_length, report_id, dev_params.iface_num);
            break;

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HID Device Disconnected - Interface: %d", dev_params.iface_num);
            g_device_connected = false;
            ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
            break;

        case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
            ESP_LOGW(TAG, "HID Device Transfer Error");
            break;

        default:
            ESP_LOGW(TAG, "Unhandled HID Interface Event: %d", event);
            break;
    }
}

static void hid_host_event_task(void *arg) {
    hid_event_queue_t evt;

    while (1) {
        if (g_event_queue == NULL) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (xQueueReceive(g_event_queue, &evt, portMAX_DELAY)) {
            switch (evt.event_group) {
                case APP_EVENT_HID_DEVICE:
                    process_device_event(evt.device_event.handle, evt.device_event.event, evt.device_event.arg);
                    break;
                case APP_EVENT_INTERFACE:
                    process_interface_event(evt.interface_event.handle, evt.interface_event.event, evt.interface_event.arg);
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown event group: %d", evt.event_group);
                    break;
            }
        }
    }
}

static void usb_lib_task(void *arg) {
    ESP_LOGI(TAG, "USB Library task started");
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "No more clients, freeing USB devices");
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB shutdown");
    vTaskDelay(10);
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void usb_stats_task(void *arg) {
    uint32_t prev_count = 0;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        uint32_t reports_per_sec = (s_current_rps - prev_count) / USB_STATS_INTERVAL_SEC;
        
        ESP_LOGI(TAG, "USB: %lu rps", reports_per_sec);
        
        prev_count = s_current_rps;
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(USB_STATS_INTERVAL_SEC * 1000));
    }
}
