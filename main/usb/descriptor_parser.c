/**
 * @file descriptor_parser.c
 * @brief USB HID Report Descriptor Parser implementation
 */

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "descriptor_parser.h"

static const char *TAG = "usb_hid_parser";

void parse_report_descriptor(const uint8_t *desc, const size_t length, const uint8_t interface_num, report_map_t *report_map) {
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
    
    // Initialize report map
    report_map->num_reports = 0;
    report_map->collection_depth = 0;
    
    // Initialize first report info
    report_info_t *current_report = &report_map->reports[0];
    report_map->report_ids[0] = 0;
    report_map->num_reports = 1;
    current_report->num_fields = 0;
    current_report->total_bits = 0;
    current_report->usage_stack_pos = 0;

    for (size_t i = 0; i < length;) {
        const uint8_t item = desc[i++];
        const uint8_t item_size = item & 0x3;
        const uint8_t item_type = (item >> 2) & 0x3;
        const uint8_t item_tag = (item >> 4) & 0xF;
        
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
                        if (current_report && current_report->num_fields < MAX_REPORT_FIELDS) {
                            bool is_constant = (data & 0x01) != 0;
                            bool is_variable = (data & 0x02) != 0;
                            is_relative = (data & 0x04) != 0;
                            bool is_array = !is_variable;

                            // If we have a report ID, find or create the corresponding report info
                            if (current_report_id != 0) {
                                int report_index = -1;
                                for (int j = 0; j < report_map->num_reports; j++) {
                                    if (report_map->report_ids[j] == current_report_id) {
                                        report_index = j;
                                        current_report = &report_map->reports[j];
                                        break;
                                    }
                                }
                                if (report_index == -1) {
                                    if (report_map->num_reports >= MAX_REPORTS_PER_INTERFACE) {
                                        ESP_LOGE(TAG, "Too many reports for interface %d", interface_num);
                                        continue;
                                    }
                                    report_index = report_map->num_reports;
                                    report_map->report_ids[report_index] = current_report_id;
                                    current_report = &report_map->reports[report_index];
                                    current_report->num_fields = 0;
                                    current_report->total_bits = 0;
                                    current_report->usage_stack_pos = 0;
                                    report_map->num_reports++;
                                }
                            }

                            if (is_constant) {
                                // Handle constant/padding field
                                report_field_info_t *field = &current_report->fields[current_report->num_fields];
                                field->attr.usage_page = current_usage_page;
                                field->attr.usage = 0;
                                field->attr.report_size = report_size;
                                field->attr.report_count = report_count;
                                field->attr.logical_min = 0;
                                field->attr.logical_max = 0;
                                field->attr.constant = true;
                                field->attr.variable = false;
                                field->attr.relative = false;
                                field->attr.array = false;
                                field->bit_offset = current_report->total_bits;
                                field->bit_size = report_size * report_count;
                                current_report->total_bits += report_size * report_count;
                                current_report->num_fields++;
                            } else if (is_array && has_usage_range) {
                                // Handle array with usage range
                                report_field_info_t *field = &current_report->fields[current_report->num_fields];
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
                                field->attr.array = true;
                                field->bit_offset = current_report->total_bits;
                                field->bit_size = report_size * report_count;
                                current_report->total_bits += report_size * report_count;
                                current_report->num_fields++;
                            } else if (has_usage_range && is_variable) {
                                // Handle variable items with usage range
                                const uint16_t range_size = usage_maximum - usage_minimum + 1;
                                for (uint8_t j = 0; j < report_count && j < range_size && current_report->num_fields < MAX_REPORT_FIELDS; j++) {
                                    report_field_info_t *field = &current_report->fields[current_report->num_fields];
                                    field->attr.usage_page = current_usage_page;
                                    field->attr.usage = usage_minimum + j;
                                    field->attr.report_size = report_size;
                                    field->attr.report_count = 1;
                                    field->attr.logical_min = logical_min;
                                    field->attr.logical_max = logical_max;
                                    field->attr.constant = false;
                                    field->attr.variable = true;
                                    field->attr.relative = is_relative;
                                    field->attr.array = false;
                                    field->bit_offset = current_report->total_bits;
                                    field->bit_size = report_size;
                                    current_report->total_bits += report_size;
                                    current_report->num_fields++;
                                }
                            } else {
                                // Handle individual usages
                                for (uint8_t j = 0; j < report_count && current_report->num_fields < MAX_REPORT_FIELDS; j++) {
                                    report_field_info_t *field = &current_report->fields[current_report->num_fields];
                                    field->attr.usage_page = current_usage_page;
                                    
                                    if (current_report->usage_stack_pos > j) {
                                        field->attr.usage = current_report->usage_stack[j];
                                    } else if (current_report->usage_stack_pos > 0) {
                                        field->attr.usage = current_report->usage_stack[current_report->usage_stack_pos - 1];
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
                                    field->attr.array = !is_variable;
                                    field->bit_offset = current_report->total_bits;
                                    field->bit_size = report_size;
                                    current_report->total_bits += report_size;
                                    current_report->num_fields++;
                                }
                            }

                            // Reset usage tracking after field processing
                            current_report->usage_stack_pos = 0;
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
                        if (current_report && current_report->usage_stack_pos < MAX_REPORT_FIELDS) {
                            current_report->usage_stack[current_report->usage_stack_pos++] = data;
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

    // Log number of fields for each report
    for (int i = 0; i < report_map->num_reports; i++) {
        ESP_LOGI(TAG, "Report ID %d has %d fields", report_map->report_ids[i], report_map->reports[i].num_fields);
    }
}

int extract_field_value(const uint8_t *data, const uint16_t bit_offset, const uint16_t bit_size) {
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

        const uint8_t bits_to_read = MIN(8 - bit_shift, bits_remaining);
        const uint8_t mask = ((1 << bits_to_read) - 1);
        const int byte_value = (current_byte >> bit_shift) & mask;
        const uint8_t shift_amount = bit_size - bits_remaining;
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
