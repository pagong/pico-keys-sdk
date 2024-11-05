/*
 * This file is part of the Pico Keys SDK distribution (https://github.com/polhenarejos/pico-keys-sdk).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "file.h"
#include "pico_keys.h"
#include <stdio.h>
#include "otp.h"

#ifdef PICO_RP2350
#include "pico/bootrom.h"
#include "hardware/structs/otp.h"
#include "hardware/regs/otp_data.h"
#endif
#include "random.h"

#ifdef PICO_RP2350

static bool is_empty_buffer(const uint8_t *buffer, uint16_t buffer_len) {
    for (int i = 0; i < buffer_len; i++) {
        if (buffer[i] != 0x00) {
            return false;
        }
    }
    return true;
}

static int otp_write_data_mode(uint16_t row, uint8_t *data, uint16_t len, bool is_ecc) {
    otp_cmd_t cmd = { .flags = row | (is_ecc ? OTP_CMD_ECC_BITS : 0) | OTP_CMD_WRITE_BITS };
    uint32_t ret = rom_func_otp_access(data, len, cmd);
    if (ret) {
        printf("OTP Write failed with error: %ld\n", ret);
    }
    return ret;
}

int otp_write_data(uint16_t row, uint8_t *data, uint16_t len) {
    return otp_write_data_mode(row, data, len, true);
}

int otp_write_data_raw(uint16_t row, uint8_t *data, uint16_t len) {
    return otp_write_data_mode(row, data, len, false);
}

uint8_t* otp_buffer(uint16_t row) {
    volatile uint32_t *p = ((uint32_t *)(OTP_DATA_BASE + (row*2)));
    return (uint8_t *)p;
}

uint8_t* otp_buffer_raw(uint16_t row) {
    volatile uint32_t *p = ((uint32_t *)(OTP_DATA_RAW_BASE + (row*4)));
    return (uint8_t *)p;
}

bool is_empty_otp_buffer(uint16_t row, uint16_t len) {
    return is_empty_buffer(otp_buffer(row), len);
}

static bool is_otp_locked_page(uint8_t page) {
	volatile uint32_t *p = ((uint32_t *)(OTP_DATA_BASE + ((OTP_DATA_PAGE0_LOCK0_ROW + page*2)*2)));
    return ((p[0] & 0xFFFF0000) == 0x3C3C0000 && (p[1] & 0xFF) == 0x3C);
}

static void otp_lock_page(uint8_t page) {
    if (!is_otp_locked_page(page)) {
        uint32_t value = 0x3c3c3c;
        otp_write_data_raw(OTP_DATA_PAGE0_LOCK0_ROW + page*2 + 1, (uint8_t *)&value, sizeof(value));
    }

    otp_hw->sw_lock[page] = 0b1100;
}
#endif

#ifdef ESP_PLATFORM

uint8_t _otp_key_1[32] = {0};

esp_err_t read_key_from_efuse(esp_efuse_block_t block, uint8_t *key, size_t key_len) {
    const esp_efuse_desc_t **key_desc = esp_efuse_get_key(block);

    if (!key_desc) {
        return ESP_FAIL;
    }

    return esp_efuse_read_field_blob(key_desc, key, key_len * 8);
}

#endif

const uint8_t *otp_key_1 = NULL;
void init_otp_files() {

#ifdef PICO_RP2350
    uint8_t page = OTP_KEY_1 >> 6;
    if (is_empty_otp_buffer(OTP_KEY_1, 32)) {
        uint8_t mkek[32] = {0};
        random_gen(NULL, mkek, sizeof(mkek));
        otp_write_data(OTP_KEY_1, mkek, sizeof(mkek));
    }
    else {
        DEBUG_DATA(otp_buffer(OTP_KEY_1), 32);
    }
    otp_key_1 = otp_buffer(OTP_KEY_1);

    otp_lock_page(page);
#ifdef ENABLE_SECURE_BOOT_FIRMWARE
    uint8_t BOOTKEY[] = "\xe1\xd1\x6b\xa7\x64\xab\xd7\x12\xd4\xef\x6e\x3e\xdd\x74\x4e\xd5\x63\x8c\x26\xb\x77\x1c\xf9\x81\x51\x11\xb\xaf\xac\x9b\xc8\x71";
#ifndef SECURE_BOOT_BOOTKEY_INDEX
#define SECURE_BOOT_BOOTKEY_INDEX 0
#endif
    if (is_empty_otp_buffer(OTP_DATA_BOOTKEY0_0_ROW + 0x10*SECURE_BOOT_BOOTKEY_INDEX, 32)) {
        otp_write_data(OTP_DATA_BOOTKEY0_0_ROW + 0x10*SECURE_BOOT_BOOTKEY_INDEX, BOOTKEY, sizeof(BOOTKEY));
    }

    uint8_t *boot_flags1 = otp_buffer_raw(OTP_DATA_BOOT_FLAGS1_ROW);
    uint8_t flagsb1[] = { boot_flags1[0] | (1 << (SECURE_BOOT_BOOTKEY_INDEX + OTP_DATA_BOOT_FLAGS1_KEY_VALID_LSB)), boot_flags1[1], boot_flags1[2], 0x00 };
#ifdef ENABLE_SECURE_BOOT_LOCK_FIRMWARE
    flagsb1[1] |= ((OTP_DATA_BOOT_FLAGS1_KEY_INVALID_BITS >> OTP_DATA_BOOT_FLAGS1_KEY_INVALID_LSB) & (~(1 << SECURE_BOOT_BOOTKEY_INDEX)));
#endif
    otp_write_data_raw(OTP_DATA_BOOT_FLAGS1_ROW, flagsb1, sizeof(flagsb1));
    otp_write_data_raw(OTP_DATA_BOOT_FLAGS1_R1_ROW, flagsb1, sizeof(flagsb1));
    otp_write_data_raw(OTP_DATA_BOOT_FLAGS1_R2_ROW, flagsb1, sizeof(flagsb1));

    uint8_t *crit1 = otp_buffer_raw(OTP_DATA_CRIT1_ROW);
    uint8_t flagsc1[] = { crit1[0] | (1 << OTP_DATA_CRIT1_SECURE_BOOT_ENABLE_LSB), crit1[1], crit1[2], 0x00 };
#ifdef ENABLE_SECURE_BOOT_LOCK_FIRMWARE
    flagsc1[0] |= (1 << OTP_DATA_CRIT1_DEBUG_DISABLE_LSB);
    flagsc1[0] |= (1 << OTP_DATA_CRIT1_GLITCH_DETECTOR_ENABLE_LSB);
    flagsc1[0] |= (3 << OTP_DATA_CRIT1_GLITCH_DETECTOR_SENS_LSB);
#endif
    otp_write_data_raw(OTP_DATA_CRIT1_ROW, flagsc1, sizeof(flagsc1));
    otp_write_data_raw(OTP_DATA_CRIT1_R1_ROW, flagsc1, sizeof(flagsc1));
    otp_write_data_raw(OTP_DATA_CRIT1_R2_ROW, flagsc1, sizeof(flagsc1));
    otp_write_data_raw(OTP_DATA_CRIT1_R3_ROW, flagsc1, sizeof(flagsc1));
    otp_write_data_raw(OTP_DATA_CRIT1_R4_ROW, flagsc1, sizeof(flagsc1));
    otp_write_data_raw(OTP_DATA_CRIT1_R5_ROW, flagsc1, sizeof(flagsc1));
    otp_write_data_raw(OTP_DATA_CRIT1_R6_ROW, flagsc1, sizeof(flagsc1));
    otp_write_data_raw(OTP_DATA_CRIT1_R7_ROW, flagsc1, sizeof(flagsc1));

#ifdef ENABLE_SECURE_BOOT_LOCK_FIRMWARE
    uint8_t *page1 = otp_buffer_raw(OTP_DATA_PAGE1_LOCK1_ROW);
    uint8_t page1v = page1[0] | (OTP_DATA_PAGE1_LOCK1_LOCK_BL_VALUE_READ_ONLY << OTP_DATA_PAGE1_LOCK1_LOCK_BL_LSB);
    uint8_t flagsp1[] = { page1v, page1v, page1v, 0x00 };
    otp_write_data_raw(OTP_DATA_PAGE1_LOCK1_ROW, flagsp1, sizeof(flagsp1));
    uint8_t *page2 = otp_buffer_raw(OTP_DATA_PAGE2_LOCK1_ROW);
    uint8_t page2v = page2[0] | (OTP_DATA_PAGE2_LOCK1_LOCK_BL_VALUE_READ_ONLY << OTP_DATA_PAGE2_LOCK1_LOCK_BL_LSB);
    uint8_t flagsp2[] = { page2v, page2v, page2v, 0x00 };
    otp_write_data_raw(OTP_DATA_PAGE2_LOCK1_ROW, flagsp2, sizeof(flagsp2));
#endif

#endif

#elif defined(ESP_PLATFORM)
    if (esp_efuse_key_block_unused(OTP_KEY_1)) {
        uint8_t mkek[32] = {0};
        random_gen(NULL, mkek, sizeof(mkek));
        DEBUG_DATA(mkek, 32);
        esp_err_t ret = esp_efuse_write_key(OTP_KEY_1, ESP_EFUSE_KEY_PURPOSE_USER, mkek, sizeof(mkek));
        if (ret != ESP_OK) {
            printf("Error writing OTP key 1 [%d]\n", ret);
        }
        ret = esp_efuse_set_key_dis_write(OTP_KEY_1);
        if (ret != ESP_OK) {
            printf("Error setting OTP key 1 to read only [%d]\n", ret);
        }
        ret = esp_efuse_set_keypurpose_dis_write(OTP_KEY_1);
        if (ret != ESP_OK) {
            printf("Error setting OTP key 1 purpose to read only [%d]\n", ret);
        }
    }
    esp_err_t ret = read_key_from_efuse(OTP_KEY_1, _otp_key_1, sizeof(_otp_key_1));
    if (ret != ESP_OK) {
        printf("Error reading OTP key 1 [%d]\n", ret);
    }
    else {
        DEBUG_DATA(_otp_key_1, 32);
    }
    otp_key_1 = _otp_key_1;

#endif
}