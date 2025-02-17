/**
 * @file    vfs_user.c
 * @brief   Implementation of vfs_user.h
 *
 * DAPLink Interface Firmware
 * Copyright (c) 2009-2020, ARM Limited, All Rights Reserved
 * Copyright 2019, Cypress Semiconductor Corporation
 * or a subsidiary of Cypress Semiconductor Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdbool.h>
#include <ctype.h>
#include <string.h>

#include "vfs_user.h"
#include "vfs_manager.h"
#include "error.h"
#include "util.h"
#include "settings.h"
#include "daplink.h"
#include "version_git.h"
#include "info.h"
#include "gpio.h"           // for gpio_get_sw_reset
#include "flash_intf.h"     // for flash_intf_target
#include "cortex_m.h"
#include "target_board.h"
#include "flash_manager.h"

//! @brief Size in bytes of the virtual disk.
//!
//! Must be bigger than 4x the flash size of the biggest supported
//! device.  This is to accomodate for hex file programming.
#define VFS_DISK_SIZE (MB(64))

//! @brief Constants for magic action or config files.
//!
//! The "magic files" are files with a special name that if created on the USB MSC volume, will
//! cause an event. There are two classes of magic files: action files and config files. The former
//! causes a given action to take place, while the latter changes a persistent configuration setting
//! to a predetermined value.
//!
//! See #s_magic_file_info for the mapping of filenames to these enums.
typedef enum _magic_file {
    kDAPLinkModeActionFile,     //!< Switch between interface and bootloader.
    kTestAssertActionFile,      //!< Force an assertion for testing.
    kRefreshActionFile,         //!< Force a remount.
    kEraseActionFile,           //!< Erase the target flash.
    kAutoResetConfigFile,       //!< Enable reset after flash.
    kHardResetConfigFile,       //!< Disable reset after flash.
    kAutomationOnConfigFile,    //!< Enable automation.
    kAutomationOffConfigFile,   //!< Disable automation.
    kOverflowOnConfigFile,      //!< Enable UART overflow reporting.
    kOverflowOffConfigFile,     //!< Disable UART overflow reporting.
    kMSDOnConfigFile,           //!< Enable USB MSC. Uh....
    kMSDOffConfigFile,          //!< Disable USB MSC.
    kImageCheckOnConfigFile,    //!< Enable Incompatible target image detection.
    kImageCheckOffConfigFile,   //!< Disable Incompatible target image detection.
    kPageEraseActionFile,       //!< Enable page programming and sector erase for drag and drop.
    kChipEraseActionFile,       //!< Enable page programming and chip erase for drag and drop.
} magic_file_t;

//! @brief Mapping from filename string to magic file enum.
typedef struct _magic_file_info {
    const char *name;   //!< Name of the magic file, must be in 8.3 format.
    magic_file_t which; //!< Enum for the file.
} magic_file_info_t;

static const vfs_filename_t assert_file = "ASSERT  TXT";

//! @brief Table of magic files and their names.
static const magic_file_info_t s_magic_file_info[] = {
        { daplink_mode_file_name, kDAPLinkModeActionFile },
        { "ASSERT  ACT", kTestAssertActionFile      },
        { "REFRESH ACT", kRefreshActionFile         },
        { "ERASE   ACT", kEraseActionFile           },
        { "AUTO_RSTCFG", kAutoResetConfigFile       },
        { "HARD_RSTCFG", kHardResetConfigFile       },
        { "AUTO_ON CFG", kAutomationOnConfigFile    },
        { "AUTO_OFFCFG", kAutomationOffConfigFile   },
        { "OVFL_ON CFG", kOverflowOnConfigFile      },
        { "OVFL_OFFCFG", kOverflowOffConfigFile     },
        { "MSD_ON  CFG", kMSDOnConfigFile           },
        { "MSD_OFF CFG", kMSDOffConfigFile          },
        { "COMP_ON CFG", kImageCheckOnConfigFile    },
        { "COMP_OFFCFG", kImageCheckOffConfigFile   },
        { "PAGE_ON ACT", kPageEraseActionFile       },
        { "PAGE_OFFACT", kChipEraseActionFile       },
    };

static char assert_buf[64 + 1];
static uint16_t assert_line;
static assert_source_t assert_source;
static uint32_t remount_count;

static uint32_t get_file_size(vfs_read_cb_t read_func);

static uint32_t read_file_mbed_htm(uint32_t sector_offset, uint8_t *data, uint32_t num_sectors);
static uint32_t read_file_details_txt(uint32_t sector_offset, uint8_t *data, uint32_t num_sectors);
static uint32_t read_file_fail_txt(uint32_t sector_offset, uint8_t *data, uint32_t num_sectors);
static uint32_t read_file_assert_txt(uint32_t sector_offset, uint8_t *data, uint32_t num_sectors);
static uint32_t read_file_need_bl_txt(uint32_t sector_offset, uint8_t *data, uint32_t num_sectors);

static uint32_t update_details_txt_file(uint8_t *data, uint32_t datasize, uint32_t start);
static void erase_target(void);

static uint32_t expand_info(uint8_t *buf, uint32_t bufsize);

__WEAK void vfs_user_build_filesystem_hook(){}

void vfs_user_build_filesystem()
{
    uint32_t file_size;
    vfs_file_t file_handle;
    // Setup the filesystem based on target parameters
    vfs_init(get_daplink_drive_name(), VFS_DISK_SIZE);
    // MBED.HTM
    file_size = get_file_size(read_file_mbed_htm);
    vfs_create_file(get_daplink_url_name(), read_file_mbed_htm, 0, file_size);
    // DETAILS.TXT
    file_size = get_file_size(read_file_details_txt);
    vfs_create_file("DETAILS TXT", read_file_details_txt, 0, file_size);

    // FAIL.TXT
    if (vfs_mngr_get_transfer_status() != ERROR_SUCCESS) {
        file_size = get_file_size(read_file_fail_txt);
        vfs_create_file("FAIL    TXT", read_file_fail_txt, 0, file_size);
    }

    // ASSERT.TXT
    if (config_ram_get_assert(assert_buf, sizeof(assert_buf), &assert_line, &assert_source)) {
        file_size = get_file_size(read_file_assert_txt);
        file_handle = vfs_create_file(assert_file, read_file_assert_txt, 0, file_size);
        vfs_file_set_attr(file_handle, (vfs_file_attr_bit_t)0); // Remove read only attribute
    }

    // NEED_BL.TXT
    volatile uint32_t bl_start = DAPLINK_ROM_BL_START; // Silence warnings about null pointer
    volatile uint32_t if_start = DAPLINK_ROM_IF_START; // Silence warnings about null pointer

    if (daplink_is_interface() &&
            (DAPLINK_ROM_BL_SIZE > 0) &&
            (0 == memcmp((void *)bl_start, (void *)if_start, DAPLINK_MIN_WRITE_SIZE))) {
        // If the bootloader contains a copy of the interfaces vector table
        // then an error occurred when updating so warn that the bootloader is
        // missing.
        file_size = get_file_size(read_file_need_bl_txt);
        vfs_create_file("NEED_BL TXT", read_file_need_bl_txt, 0, file_size);
    }

    vfs_user_build_filesystem_hook();
}

// Default file change hook.
__WEAK bool vfs_user_file_change_handler_hook(const vfs_filename_t filename, vfs_file_change_t change,
        vfs_file_t file, vfs_file_t new_file_data)
{
    return false;
}

// Default magic file hook.
__WEAK bool vfs_user_magic_file_hook(const vfs_filename_t filename, bool *do_remount)
{
    return false;
}

// Callback to handle changes to the root directory.  Should be used with vfs_set_file_change_callback
void vfs_user_file_change_handler(const vfs_filename_t filename, vfs_file_change_t change, vfs_file_t file, vfs_file_t new_file_data)
{
    // Call file changed hook. If it returns true, then it handled the request and we have nothing
    // more to do.
    if (vfs_user_file_change_handler_hook(filename, change, file, new_file_data)) {
        return;
    }

    // Allow settings to be changed if automation mode is
    // enabled or if the user is holding the reset button
    bool btn_pressed = gpio_get_reset_btn();

    if (!btn_pressed && !config_get_automation_allowed()) {
        return;
    }

    if (VFS_FILE_CHANGED == change) {
        // Unused
    }

    else if (VFS_FILE_CREATED == change) {
        bool do_remount = true; // Almost all magic files cause a remount.
        int32_t which_magic_file = -1;

        // Let the hook examine the filename. If it returned false then look for the standard
        // magic files.
        if (!vfs_user_magic_file_hook(filename, &do_remount)) {
            // Compare the new file's name to our table of magic filenames.
            for (int32_t i = 0; i < ARRAY_SIZE(s_magic_file_info); ++i) {
                if (!memcmp(filename, s_magic_file_info[i].name, sizeof(vfs_filename_t))) {
                    which_magic_file = s_magic_file_info[i].which;
                }
            }

            // Check if we matched a magic filename and handle it.
            if (which_magic_file != -1) {
                switch (which_magic_file) {
                    case kDAPLinkModeActionFile:
                        if (daplink_is_interface()) {
                            config_ram_set_hold_in_bl(true);
                        } else {
                            // Do nothing - bootloader will go to interface by default
                        }
                        break;
                    case kTestAssertActionFile:
                        // Test asserts
                        util_assert(0);
                        do_remount = false;
                        break;
                    case kRefreshActionFile:
                        // Remount to update the drive
                        break;
                    case kEraseActionFile:
                        erase_target();
                        break;
                    case kAutoResetConfigFile:
                        config_set_auto_rst(true);
                        break;
                    case kHardResetConfigFile:
                        config_set_auto_rst(false);
                        break;
                    case kAutomationOnConfigFile:
                        config_set_automation_allowed(true);
                        break;
                    case kAutomationOffConfigFile:
                        config_set_automation_allowed(false);
                        break;
                    case kOverflowOnConfigFile:
                        config_set_overflow_detect(true);
                        break;
                    case kOverflowOffConfigFile:
                        config_set_overflow_detect(false);
                        break;
                    case kMSDOnConfigFile:
                        config_ram_set_disable_msd(false);
                        break;
                    case kMSDOffConfigFile:
                        config_ram_set_disable_msd(true);
                        break;
                    case kImageCheckOnConfigFile:
                        config_set_detect_incompatible_target(true);
                        break;
                    case kImageCheckOffConfigFile:
                        config_set_detect_incompatible_target(false);
                        break;
                    case kPageEraseActionFile:
                        config_ram_set_page_erase(true);
                        break;
                    case kChipEraseActionFile:
                        config_ram_set_page_erase(false);
                        break;
                    default:
                        util_assert(false);
                }
            }
            else {
                do_remount = false;
            }
        }

        // Remount if requested.
        if (do_remount) {
            vfs_mngr_fs_remount();
        }
    }

    else if (VFS_FILE_DELETED == change) {
        if (!memcmp(filename, assert_file, sizeof(vfs_filename_t))) {
            // Clear assert and remount to update the drive
            util_assert_clear();
            vfs_mngr_fs_remount();
        }
    }
}

void vfs_user_disconnecting()
{
    // Reset if programming was successful  //TODO - move to flash layer
    if (daplink_is_bootloader() && (ERROR_SUCCESS == vfs_mngr_get_transfer_status())) {
        SystemReset();
    }

    // If hold in bootloader has been set then reset after usb is disconnected
    if (daplink_is_interface() && (config_ram_get_hold_in_bl() || config_ram_get_disable_msd()==1)) {
        SystemReset();
    }

    remount_count++;
}

// Get the filesize from a filesize callback.
// The file data must be null terminated for this to work correctly.
static uint32_t get_file_size(vfs_read_cb_t read_func)
{
    // Determine size of the file by faking a read
    return read_func(0, NULL, 0);
}

#ifndef EXPANSION_BUFFER_SIZE
#define EXPANSION_BUFFER_SIZE 128
#endif

uint32_t expand_string_in_region(uint8_t *buf, uint32_t size, uint32_t start, uint32_t pos, const char *input) {
    char str_buf[EXPANSION_BUFFER_SIZE];
    memset(str_buf, 0, sizeof(str_buf));
    for(uint32_t i = 0; (i < (sizeof(str_buf) - 1)) && 0 != input[i]; i++) {
        str_buf[i] = input[i];
    }
    uint32_t l = expand_info((uint8_t *)str_buf, sizeof(str_buf));

    return util_write_in_region(buf, size, start, pos, str_buf, l);
}

uint32_t string_field_in_region(uint8_t *buf, uint32_t size, uint32_t start, uint32_t pos, const char *label, const char *value) {
    uint32_t l = util_write_string_in_region(buf, size, start, pos, label);
    l += util_write_in_region(buf, size, start, pos + l, ": ", 2);
    l += util_write_string_in_region(buf, size, start, pos + l, value);
    l += util_write_in_region(buf, size, start, pos + l, "\r\n", 2);
    return l;
}

uint32_t setting_in_region(uint8_t *buf, uint32_t size, uint32_t start, uint32_t pos, const char *label, uint32_t boolean) {
    return string_field_in_region(buf, size, start, pos, label, boolean ? "1" : "0");
}

uint32_t uint32_field_in_region(uint8_t *buf, uint32_t size, uint32_t start, uint32_t pos, const char *label, uint32_t value) {
    char number[11];
    uint32_t digits = util_write_uint32(number, value);
    number[digits] = 0;
    return string_field_in_region(buf, size, start, pos, label, (char *)number);
}

uint32_t hex32_field_in_region(uint8_t *buf, uint32_t size, uint32_t start, uint32_t pos, const char *label, uint32_t value) {
    char hex[11] = { '0', 'x' };
    util_write_hex32(hex + 2, value);
    hex[10] = 0;
    return string_field_in_region(buf, size, start, pos, label, hex);
}

// File callback to be used with vfs_add_file to return file contents
static uint32_t read_file_mbed_htm(uint32_t sector_offset, uint8_t *buf, uint32_t num_sectors)
{
    uint32_t start = sector_offset * VFS_SECTOR_SIZE;
    uint32_t size = num_sectors * VFS_SECTOR_SIZE;
    uint32_t pos = 0;

    if ((sector_offset != 0) && (buf != NULL)) {
        return 0;
    }

    pos += util_write_string_in_region(buf, size, start, pos,
        "<!doctype html>\r\n"
        "<!-- mbed Platform Website and Authentication Shortcut -->\r\n"
        "<html>\r\n"
        "<head>\r\n"
        "<meta charset=\"utf-8\">\r\n"
        "<title>mbed Website Shortcut</title>\r\n"
        "</head>\r\n"
        "<body>\r\n"
        "<script>\r\n"
        "window.location.replace(\"");
    pos += expand_string_in_region(buf, size, start, pos, "@R");
    pos += util_write_string_in_region(buf, size, start, pos, "\");\r\n"
        "</script>\r\n"
        "</body>\r\n"
        "</html>\r\n");

    return pos;
}

// File callback to be used with vfs_add_file to return file contents
static uint32_t read_file_details_txt(uint32_t sector_offset, uint8_t *data, uint32_t num_sectors)
{
    return update_details_txt_file(data, num_sectors * VFS_SECTOR_SIZE, sector_offset * VFS_SECTOR_SIZE);
}

// Text representation of each error type, starting from the rightmost bit
static const char* const error_type_names[] = {
    "internal",
    "transient",
    "user",
    "target",
    "interface"
};

COMPILER_ASSERT(1 << ARRAY_SIZE(error_type_names) == ERROR_TYPE_MASK + 1);

// File callback to be used with vfs_add_file to return file contents
static uint32_t read_file_fail_txt(uint32_t sector_offset, uint8_t *buf, uint32_t num_sectors)
{
    uint32_t start = sector_offset * VFS_SECTOR_SIZE;
    uint32_t size = num_sectors * VFS_SECTOR_SIZE;
    uint32_t pos = 0;
    error_t status = vfs_mngr_get_transfer_status();
    const char *contents = error_get_string(status);
    error_type_t type = error_get_type(status);

    if ((sector_offset != 0) && (buf != NULL)) {
        return 0;
    }

    pos += string_field_in_region(buf, size, start, pos, "error", contents);
    pos += util_write_string_in_region(buf, size, start, pos, "type: ");

    // Write each applicable error type, separated by commas
    int index = 0;
    bool first = true;
    while (type && index < ARRAY_SIZE(error_type_names)) {
        if (!first) {
            pos += util_write_in_region(buf, size, start, pos, ", ", 2);
        }
        if (type & 1) {
            pos += util_write_string_in_region(buf, size, start, pos, error_type_names[index]);
            first = false;
        }
        index++;
        type >>= 1;
    }

    pos += util_write_in_region(buf, size, start, pos, "\r\n", 2);
    return pos;
}

// File callback to be used with vfs_add_file to return file contents
static uint32_t read_file_assert_txt(uint32_t sector_offset, uint8_t *buf, uint32_t num_sectors)
{
    uint32_t start = sector_offset * VFS_SECTOR_SIZE;
    uint32_t size = num_sectors * VFS_SECTOR_SIZE;
    uint32_t pos = 0;
    const char *source_str;
    uint32_t * hexdumps = 0;
    uint8_t valid_hexdumps = 0;
    uint8_t index = 0;

    if ((sector_offset != 0) && (buf != NULL)) {
        return 0;
    }

    if (ASSERT_SOURCE_BL == assert_source) {
        source_str = "Bootloader";
    } else if (ASSERT_SOURCE_APP == assert_source) {
        source_str = "Application";
    } else {
        source_str = 0;
    }

    pos += util_write_string_in_region(buf, size, start, pos, "Assert\r\n");
    pos += string_field_in_region(buf, size, start, pos, "File", assert_buf);
    pos += uint32_field_in_region(buf, size, start, pos, "Line", assert_line);

    if (source_str != 0) {
        pos += string_field_in_region(buf, size, start, pos, "Source", source_str);
    }

    valid_hexdumps = config_ram_get_hexdumps(&hexdumps);
    if ((valid_hexdumps > 0) && (hexdumps != 0)) {
        //print hexdumps
        pos += util_write_string_in_region(buf, size, start, pos, "Hexdumps\r\n");
        while ((index < valid_hexdumps) && ((pos + 10) < VFS_SECTOR_SIZE)) { //hexdumps + newline is always 10 characters
            char hex[10] = { 0, 0, 0, 0, 0, 0, 0, 0, '\r', '\n' };
            util_write_hex32(hex, hexdumps[index++]);
            pos += util_write_in_region(buf, size, start, pos, hex, 10);
        }
    }

    return pos;
}

// File callback to be used with vfs_add_file to return file contents
static uint32_t read_file_need_bl_txt(uint32_t sector_offset, uint8_t *data, uint32_t num_sectors)
{
    const char *contents = "A bootloader update was started but unable to complete.\r\n"
                           "Reload the bootloader to fix this error message.\r\n";
    uint32_t size = strlen(contents);

    if (data != NULL) {
        if (sector_offset != 0) {
            return 0;
        }

        memcpy(data, contents, size);
    }

    return size;
}

#if defined(__CC_ARM)
#define COMPILER_DESCRIPTION "armcc"
#elif (defined(__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050))
#define COMPILER_DESCRIPTION "armclang"
#elif defined(__GNUC__)
#define COMPILER_DESCRIPTION "gcc"
#endif

#if (GIT_LOCAL_MODS)
#define LOCAL_MODS ", local mods"
#else
#define LOCAL_MODS ""
#endif

#define BUILD_TIME "Build Time: " __DATE__ " " __TIME__ "\r\n"
static uint32_t update_details_txt_file(uint8_t *buf, uint32_t size, uint32_t start)
{
    uint32_t pos = 0;

    pos += util_write_string_in_region(buf, size, start, pos,
        "# DAPLink Firmware - see https://daplink.io\r\n"
        // Build ID
        "Build ID: " GIT_DESCRIPTION " (" COMPILER_DESCRIPTION LOCAL_MODS ")\r\n");
    pos += util_write_string_in_region(buf, size, start, pos, "# DAPLink Firmware by MuseLab - see muselab-tech.com\r\n");
	pos += util_write_string_in_region(buf, size, start, pos, BUILD_TIME);
		
    // Unique ID
    pos += expand_string_in_region(buf, size, start, pos, "Unique ID: @U\r\n");
    // HIC ID
    pos += expand_string_in_region(buf, size, start, pos, "HIC ID: @D\r\n");
    // Settings
    pos += setting_in_region(buf, size, start, pos, "Auto Reset", config_get_auto_rst());
    pos += setting_in_region(buf, size, start, pos, "Automation allowed", config_get_automation_allowed());
    pos += setting_in_region(buf, size, start, pos, "Overflow detection", config_get_overflow_detect());
    pos += setting_in_region(buf, size, start, pos, "Incompatible image detection", config_get_detect_incompatible_target());
    pos += setting_in_region(buf, size, start, pos, "Page erasing", config_ram_get_page_erase());

    // Current mode and version
#if defined(DAPLINK_BL)
    pos += util_write_string_in_region(buf, size, start, pos, "Daplink Mode: Bootloader\r\n");
    pos += expand_string_in_region(buf, size, start, pos, "Bootloader Version: @V\r\n");

    if (info_get_interface_present()) {
        char version[6] = { 0, 0, 0, 0, '\r', '\n' };
        pos += util_write_string_in_region(buf, size, start, pos, "Interface Version: ");
        util_write_uint32_zp(version, info_get_interface_version(), 4);
        pos += util_write_in_region(buf, size, start, pos, version, 6);
    }
#elif defined(DAPLINK_IF)
    pos += util_write_string_in_region(buf, size, start, pos, "Daplink Mode: Interface\r\n");
    pos += expand_string_in_region(buf, size, start, pos, "Interface Version: @V\r\n");

#if DAPLINK_ROM_BL_SIZE != 0
    if (info_get_bootloader_present()) {
        char version[6] = { 0, 0, 0, 0, '\r', '\n' };
        pos += util_write_string_in_region(buf, size, start, pos, "Bootloader Version: ");
        util_write_uint32_zp(version, info_get_bootloader_version(), 4);
        pos += util_write_in_region(buf, size, start, pos, version, 6);
    }
#endif
#endif

    pos += util_write_string_in_region(buf, size, start, pos,
        // Full commit hash
        "Git SHA: " GIT_COMMIT_SHA "\r\n"
        // Local modifications when making the build
#if GIT_LOCAL_MODS
        "Local Mods: 1\r\n"
#else
        "Local Mods: 0\r\n"
#endif
        // Supported USB endpoints
        "USB Interfaces: "
#ifdef MSC_ENDPOINT
        "MSD"
#endif
#ifdef CDC_ENDPOINT
        ", CDC"
#endif
#ifdef HID_ENDPOINT
        ", HID"
#endif
#if (WEBUSB_INTERFACE)
        ", WebUSB"
#endif
        "\r\n");

#if DAPLINK_ROM_BL_SIZE != 0
    // CRC of the bootloader (if there is one)
    if (info_get_bootloader_present()) {
        pos += hex32_field_in_region(buf, size, start, pos, "Bootloader CRC", info_get_crc_bootloader());
    }
#endif

    // CRC of the interface
    pos += hex32_field_in_region(buf, size, start, pos, "Interface CRC", info_get_crc_interface());

    // Number of remounts that have occurred
    pos += uint32_field_in_region(buf, size, start, pos, "Remount count", remount_count);

    //Target URL
    pos += expand_string_in_region(buf, size, start, pos, "URL: @R\r\n");

    return pos;
}

// Fill buf with the contents of the mbed redirect file by
// expanding the special characters in mbed_redirect_file.
static uint32_t expand_info(uint8_t *buf, uint32_t bufsize)
{
    uint8_t *orig_buf = buf;
    uint8_t *insert_string;

    do {
        // Look for key or the end of the string
        while ((*buf != '@') && (*buf != 0)) {
            buf++;
        }

        // If key was found then replace it
        if ('@' == *buf) {
            switch (*(buf + 1)) {
                case 'm':
                case 'M':   // MAC address
                    insert_string = (uint8_t *)info_get_mac();
                    break;

                case 'u':
                case 'U':   // UUID
                    insert_string = (uint8_t *)info_get_unique_id();
                    break;

                case 'b':
                case 'B':   // Board ID
                    insert_string = (uint8_t *)info_get_board_id();
                    break;

                case 'h':
                case 'H':   // Host ID
                    insert_string = (uint8_t *)info_get_host_id();
                    break;

                case 't':
                case 'T':   // Target ID
                    insert_string = (uint8_t *)info_get_target_id();
                    break;

                case 'd':
                case 'D':   // HIC
                    insert_string = (uint8_t *)info_get_hic_id();
                    break;

                case 'v':
                case 'V':   // Firmware version
                    insert_string = (uint8_t *)info_get_version();
                    break;

                case 'r':
                case 'R':   // URL replacement
                    insert_string = (uint8_t *)get_daplink_target_url();
                    break;

                default:
                    insert_string = (uint8_t *)"ERROR";
                    break;
            }

            // Remove strip_count characters from the start of buf and then insert
            // insert_string at the new start of buf.
            uint32_t buf_len = strlen((const char *)buf);
            uint32_t str_len = strlen((const char *)insert_string);
            //buffer overflow check on insert
            if( (buf + str_len + buf_len - 2) < (orig_buf+bufsize)){
                // push out string
                memmove(buf + str_len, buf + 2, buf_len - 2);
                // insert
                memcpy(buf, insert_string, str_len);
            }else{
                //stop the string expansion and leave as it is
                buf += buf_len;
                break;
            }

        }
    } while (*buf != '\0');

    return (buf - orig_buf);
}

// Initialize flash algo, erase flash, uninit algo
static void erase_target(void)
{
    flash_intf_target->init();
    flash_intf_target->erase_chip();
    flash_intf_target->uninit();
}
