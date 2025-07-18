/**
 * Copyright (c) 2022-2024 sashz /pdaXrom.org/
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <ctype.h>
#include <libdragon.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "timerimg.h"

#include "../../fw/romfs/romfs.h"
#include "ext/shell_utils.h"
#include "n64cart.h"
#include "../build/wy700font-regular.h"
#include "main.h"
#include "usb/usbd.h"

#include "syslog.h"
#include "md5.h"
#include "imgviewer.h"

#define FILE_NAME_SCROLL_DELAY  (5)
#define KEYS_DELAY (3)

enum {
    STEP_LOGO = 0,
    STEP_ROMFS_INIT,
    STEP_LOAD_BACKGROUND,
    STEP_SAVE_GAMESAVE,
    //STEP_USB_INIT,
    STEP_FINISH
};

static const struct flash_chip flash_chip[] = {
    { 0xc2, 0x201b, 128, "MX66L1G45G" },
    { 0xef, 0x4020, 64, "W25Q512" },
    { 0xef, 0x4019, 32, "W25Q256" },
    { 0xef, 0x4018, 16, "W25Q128" },
    { 0xef, 0x4017, 8, "W25Q64" },
    { 0xef, 0x4016, 4, "W25Q32" },
    { 0xef, 0x4015, 2, "W25Q16" }
};

static const struct flash_chip *used_flash_chip = NULL;

static int scr_width;
static int scr_height;
static int scr_scale;

static struct File_Rec {
    char *name;
    size_t size;
    int scroll_pos;
    int scroll_dir;
    int scroll_delay;
} files[128];

static int num_files = 0;
static int menu_sel = 0;

static uint8_t __attribute__((aligned(16))) save_data[131072];
static sprite_t *bg_img = NULL;

static int do_step = STEP_LOGO;

bool romfs_flash_sector_erase(uint32_t offset)
{
#ifdef DEBUG_FS
    syslog(LOG_DEBUG, "%s: offset %08X", __func__, offset);
#endif
    disable_interrupts();
    flash_mode(0);
    flash_erase_sector(offset);
    flash_mode(1);
    enable_interrupts();

    return true;
}

bool romfs_flash_sector_write(uint32_t offset, uint8_t *buffer)
{
#ifdef DEBUG_FS
    syslog(LOG_DEBUG, "%s: offset %08X", __func__, offset);
#endif
    disable_interrupts();
    flash_mode(0);
    flash_write_sector(offset, buffer);
    flash_mode(1);
    enable_interrupts();

    return true;
}

bool romfs_flash_sector_read(uint32_t offset, uint8_t *buffer, uint32_t need)
{
#ifdef DEBUG_FS
    syslog(LOG_DEBUG, "%s: offset %08lX, need %ld", __func__, offset, need);
#endif
    disable_interrupts();
    flash_mode(0);
    flash_read(offset, buffer, need);
    flash_mode(1);
    enable_interrupts();

    return true;
}

static void detect_flash_chip()
{
    uint8_t rxbuf[4];

    disable_interrupts();
    flash_mode(0);
    flash_do_cmd(0x9f, NULL, rxbuf, 4);
    flash_mode(1);
    enable_interrupts();

    syslog(LOG_INFO, "Flash jedec id %02X %02X %02X", rxbuf[0], rxbuf[1], rxbuf[2]);

    uint8_t mf = rxbuf[0];
    uint16_t id = (rxbuf[1] << 8) | rxbuf[2];

    used_flash_chip = NULL;
    for (int i = 0; i < sizeof(flash_chip) / sizeof(struct flash_chip); i++) {
        if (flash_chip[i].mf == mf && flash_chip[i].id == id) {
            used_flash_chip = &flash_chip[i];
            break;
        }
    }
}

const struct flash_chip *get_flash_info()
{
    return used_flash_chip;
}

static bool get_rom_name(char *name, int size, bool *adv, uint8_t *opts)
{
    n64cart_sram_unlock();
    disable_interrupts();
    flash_mode(0);
    flash_read(((pi_io_read(N64CART_ROM_LOOKUP) >> 16) << 12) + 0x3b, (void *)name, 5);
    flash_mode(1);
    enable_interrupts();
    n64cart_sram_lock();

    for (int i = 0; i < 4; i++) {
        if (!isalnum((int)name[i])) {
            return false;
        }
    }

    if (adv) {
        if (!strncmp(name + 1, "ED", 2)) {
            *adv = true;
            if (opts) {
                *opts = name[4];
            }
        } else {
            *adv = false;
        }
    }

    snprintf(&name[4], size - 4, "-%d", name[4]);

    return true;
}

static void run_rom(display_context_t disp, const char *name, const char *addon, const int addon_offset, int addon_save_type)
{
    romfs_file file;
    uint8_t romfs_flash_buffer[ROMFS_FLASH_SECTOR];

    if (romfs_open_file(name, &file, romfs_flash_buffer) == ROMFS_NOERR) {
        uint16_t rom_lookup[ROMFS_FLASH_SECTOR * 4];

        memset(rom_lookup, 0, sizeof(rom_lookup));
        uint32_t map_size = romfs_read_map_table(rom_lookup, sizeof(rom_lookup) / 2, &file);

        n64cart_sram_unlock();
        
        for (int i = 0; i < map_size; i += 2) 
        {
            uint32_t data = (rom_lookup[i] << 16) | rom_lookup[i + 1];
            io_write(N64CART_ROM_LOOKUP + (i << 1), data);
        }
        
        n64cart_sram_lock();

        char save_name[64];
        int cic_id = 2;
        int save_type = 0;
        int rom_detected = 0;

        bool adv;
        uint8_t opts;
        
        if (get_rom_name(save_name, sizeof(save_name), &adv, &opts)) 
        {
            if (adv) 
            {
                static const uint8_t save_type_conv[16] = { 0, 3, 4, 1, 6, 5, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
                rom_detected = true;
                save_type = save_type_conv[opts >> 4];
            } 
            else 
            {
                rom_detected = get_cic_save(&save_name[1], &cic_id, &save_type);
            }
        }

        strcpy(save_name, "rom.sav");

        if (rom_detected) 
        {
            uint8_t erase_byte = 0xff;

            int save_file_size = 0;
            uint32_t pi_addr = 0;
            switch (save_type) {
            case 1:
                save_file_size = 32768;
                pi_addr = N64CART_SRAM;
                erase_byte = 0;
                break;
            case 2:
                save_file_size = 131072;
                pi_addr = N64CART_SRAM;
                erase_byte = 0;
                break;
            case 3:
                save_file_size = 512;
                n64cart_eeprom_16kbit(false);
                pi_addr = N64CART_EEPROM;
                break;
            case 4:
                save_file_size = 2048;
                n64cart_eeprom_16kbit(true);
                pi_addr = N64CART_EEPROM;
                break;
            case 5:
                save_file_size = 131072;
                pi_addr = N64CART_SRAM;
                break;
            case 6:
                save_file_size = 98304;
                pi_addr = N64CART_SRAM;
                erase_byte = 0;
                break;
            default:
                save_name[0] = '\0';
                save_file_size = 0;
                pi_addr = 0;
            }

            n64cart_sram_unlock();
            io_write(N64CART_RMRAM, pi_addr);
            io_write(N64CART_RMRAM + 4, save_file_size);

            if (strlen(save_name) > 0) 
            {
                for (int i = 0; i < sizeof(save_name); i += 4)
                    io_write(N64CART_RMRAM + 8 + i, *((uint32_t *) &save_name[i]));
                
                n64cart_sram_lock();
                romfs_file save_file;

                if (romfs_open_file(save_name, &save_file, romfs_flash_buffer) == ROMFS_NOERR) 
                {
                    int rbytes = 0;
                    while (rbytes < save_file_size) {
                        int ret = romfs_read_file(&save_data[rbytes], 4096, &save_file);
                        if (!ret) {
                            break;
                        }
                        rbytes += ret;
                    }
                    
                    if (rbytes <= 2048) {
                        // eeprom byte swap
                        for (int i = 0; i < rbytes; i += 2) {
                            uint8_t tmp = save_data[i];
                            save_data[i] = save_data[i + 1];
                            save_data[i + 1] = tmp;
                        }
                    } else {
                        // sram word swap
                        for (int i = 0; i < rbytes; i += 4) {
                            uint8_t tmp = save_data[i];
                            save_data[i] = save_data[i + 3];
                            save_data[i + 3] = tmp;
                            tmp = save_data[i + 2];
                            save_data[i + 2] = save_data[i + 1];
                            save_data[i + 1] = tmp;
                        }
                    }
                    
                    n64cart_sram_unlock();

                    for (int i = 0; i < save_file_size; i += 4)
                        io_write(pi_addr + i, *((uint32_t *) & save_data[i]));
                } 
                else 
                {
                    syslog(LOG_INFO, "No valid eeprom dump, clean eeprom data");
                    memset(save_data, erase_byte, sizeof(save_data));

                    n64cart_sram_unlock();

                    for (int i = 0; i < save_file_size; i += 4)
                        io_write(pi_addr + i, 0);
                }
            }
            n64cart_sram_lock();
        } 
        else 
        {
            n64cart_sram_unlock();
            io_write(N64CART_RMRAM, 0);
            n64cart_sram_lock();
        }

        if (save_type == 5) 
            n64cart_fram_mode();

        OS_INFO->tv_type = get_tv_type();
        OS_INFO->reset_type = RESET_COLD;
        OS_INFO->mem_size = get_memory_size();
        
        display_close();
        usbd_finish();
        disable_interrupts();
        simulate_boot(cic_id, 2);
    }
}



int main(void)
{
    usbd_start();

    scr_width = display_get_width();
    scr_height = display_get_height();

    timer_init();

    dma_wait();

    io_write(N64CART_LED_CTRL, 0);

    detect_flash_chip();
    
    do_step = STEP_ROMFS_INIT;
    
    display_init(RESOLUTION_320x240, DEPTH_32_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
   
    scr_width = display_get_width();
    scr_height = display_get_height();    
    scr_scale = 1;
    
    static display_context_t disp = 0;
    
    
    bool save_save = false;
    
    if (sys_reset_type() == RESET_WARM)
        save_save = true;
    
    if (save_save)
        bg_img = image_load(NULL, 64, 64, timer, 5990);   

    while (1) 
    {
        disp = display_get();
        graphics_fill_screen(disp, 0);
        graphics_set_color(0xeeeeee00, 0x00000000);
        
        if (save_save)
        {
            if (bg_img)
                graphics_draw_sprite(disp, 160 - 32, 120 - 32, bg_img);      
        }
        
        
        if (do_step == STEP_ROMFS_INIT) 
        {
            uint32_t flash_map_size, flash_list_size;
            romfs_get_buffers_sizes(used_flash_chip->rom_size * 1024 * 1024, &flash_map_size, &flash_list_size);

            static uint16_t *romfs_flash_map = NULL;
            static uint8_t *romfs_flash_list = NULL;

            if (!romfs_flash_map)
                romfs_flash_map = malloc(flash_map_size);

            if (!romfs_flash_list)
                romfs_flash_list = malloc(flash_list_size);

            uint32_t fw_size = n64cart_fw_size();
            
            if (!romfs_start(fw_size, used_flash_chip->rom_size * 1024 * 1024, romfs_flash_map, romfs_flash_list)) 
            {
                syslog(LOG_ERR, "Cannot start romfs!");
            } 
            else 
            {
                romfs_file file;

                num_files = 0;
                menu_sel = 0;

                if (romfs_list(&file, true) == ROMFS_NOERR) 
                {
                    do 
                    {
                        if (file.entry.attr.names.type > ROMFS_TYPE_FLASHMAP) 
                        {
                            files[num_files].name = strdup(file.entry.name);
                            files[num_files].size = file.entry.size;
                            files[num_files].scroll_pos = 0;
                            files[num_files].scroll_dir = 1;
                            files[num_files].scroll_delay = FILE_NAME_SCROLL_DELAY;
                            num_files++;
                        }
                    }
                    while (romfs_list(&file, false) == ROMFS_NOERR);
                }
            }

            do_step = STEP_SAVE_GAMESAVE;
        }
        else if (do_step == STEP_SAVE_GAMESAVE) 
        {
            char save_name[64] = { "rom.sav" };
            uint32_t save_addr = 0;
            int save_size = 0;
            do_step = STEP_FINISH;

            n64cart_sram_mode();
            n64cart_sram_unlock();
            
            if (save_save) 
            {
                save_addr = io_read(N64CART_RMRAM);
                save_size = io_read(N64CART_RMRAM + 4);
                
                if (save_addr && save_size) 
                {
                    for (int i = 0; i < save_size; i += 4) 
                        *((uint32_t *) &save_data[i]) = io_read(save_addr + i);
                }
            }
            
            io_write(N64CART_RMRAM, 0);
            n64cart_sram_lock();
            n64cart_eeprom_16kbit(true);

            if (save_save)
            {
                romfs_file save_file;
                uint8_t romfs_flash_buffer[ROMFS_FLASH_SECTOR];

                romfs_delete(save_name);
                
                if (save_size <= 2048) 
                {
                    // eeprom byte swap
                    for (int i = 0; i < save_size; i += 2) 
                    {
                        uint8_t tmp = save_data[i];
                        save_data[i] = save_data[i + 1];
                        save_data[i + 1] = tmp;
                    }
                } 
                else 
                {
                    // sram word swap
                    for (int i = 0; i < save_size; i += 4) 
                    {
                        uint8_t tmp = save_data[i];
                        save_data[i] = save_data[i + 3];
                        save_data[i + 3] = tmp;
                        tmp = save_data[i + 2];
                        save_data[i + 2] = save_data[i + 1];
                        save_data[i + 1] = tmp;
                    }
                }

                if (romfs_create_file(save_name, &save_file, ROMFS_MODE_READWRITE, ROMFS_TYPE_MISC, romfs_flash_buffer) == ROMFS_NOERR) 
                {
                    int bwrite = 0;
                    int ret = 0;
                    while (save_size > 0) 
                    {
                        ret = romfs_write_file(&save_data[bwrite], (save_size > 4096) ? 4096 : save_size, &save_file);
                        
                        if (!ret)
                            break;
                        
                        save_size -= ret;
                        bwrite += ret;
                    }
                    
                    romfs_close_file(&save_file);
                    
                    if (!ret)
                        romfs_delete(save_name);
                }
                

                save_addr = 0;
                save_size = 0;
                save_name[0] = 0;
            }
        }
        else
            run_rom(NULL, "rom.z64", NULL, 0, 0);
        
        display_show(disp);   
    }

    return 0;
}
