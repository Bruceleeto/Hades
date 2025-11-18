/******************************************************************************\
**
**  This file is part of the Hades GBA Emulator, and is made available under
**  the terms of the GNU General Public License version 2.
**
**  Copyright (C) 2021-2024 - The Hades Authors
**
\******************************************************************************/

#include <string.h>
#include "hades.h"
#include "gba/gba.h"
#include "gba/core.h"
#include "gba/core/helpers.h"
#include "gba/memory.h"
#include "gba/gpio.h"


/*
** Region        Bus   Read      Write     Cycles   Note
** ==================================================
** BIOS ROM      32    8/16/32   -         1/1/1
** Work RAM 32K  32    8/16/32   8/16/32   1/1/1
** I/O           32    8/16/32   8/16/32   1/1/1
** OAM           32    8/16/32   16/32     1/1/1    a
** Work RAM 256K 16    8/16/32   8/16/32   3/3/6    b
** Palette RAM   16    8/16/32   16/32     1/1/2    a
** VRAM          16    8/16/32   16/32     1/1/2    a
** GamePak ROM   16    8/16/32   -         5/5/8    b/c
** GamePak Flash 16    8/16/32   16/32     5/5/8    b/c
** GamePak SRAM  8     8         8         5        b
**
** Timing Notes:
**
**  a   Plus 1 cycle if GBA accesses video memory at the same time.
**  b   Default waitstate settings, see System Control chapter.
**  c   Separate timings for sequential, and non-sequential accesses.
**
** Source: GBATek
*/
static uint32_t access_time16[2][16] = {
    [NON_SEQUENTIAL]    = { 1, 1, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
    [SEQUENTIAL]        = { 1, 1, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
};

static uint32_t access_time32[2][16] = {
    [NON_SEQUENTIAL]    = { 1, 1, 6, 1, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
    [SEQUENTIAL]        = { 1, 1, 6, 1, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
};

static uint32_t gamepak_nonseq_waitstates[4] = { 4, 3, 2, 8 };

/*
** Set the waitstates for ROM/SRAM memory according to the content of REG_WAITCNT.
*/
void
mem_update_waitstates(
    struct gba const *gba
) {
    struct io const *io;
    uint32_t x;

    io = &gba->io;

    // 16 bit, non seq
    access_time16[NON_SEQUENTIAL][CART_0_REGION_1] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws0_nonseq];
    access_time16[NON_SEQUENTIAL][CART_0_REGION_2] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws0_nonseq];
    access_time16[NON_SEQUENTIAL][CART_1_REGION_1] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws1_nonseq];
    access_time16[NON_SEQUENTIAL][CART_1_REGION_2] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws1_nonseq];
    access_time16[NON_SEQUENTIAL][CART_2_REGION_1] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws2_nonseq];
    access_time16[NON_SEQUENTIAL][CART_2_REGION_2] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws2_nonseq];
    access_time16[NON_SEQUENTIAL][SRAM_REGION]     = 1 + gamepak_nonseq_waitstates[io->waitcnt.sram];

    // 16 bit, seq
    access_time16[SEQUENTIAL][CART_0_REGION_1] = 1 + (io->waitcnt.ws0_seq ? 1 : 2);
    access_time16[SEQUENTIAL][CART_0_REGION_2] = 1 + (io->waitcnt.ws0_seq ? 1 : 2);
    access_time16[SEQUENTIAL][CART_1_REGION_1] = 1 + (io->waitcnt.ws1_seq ? 1 : 4);
    access_time16[SEQUENTIAL][CART_1_REGION_2] = 1 + (io->waitcnt.ws1_seq ? 1 : 4);
    access_time16[SEQUENTIAL][CART_2_REGION_1] = 1 + (io->waitcnt.ws2_seq ? 1 : 8);
    access_time16[SEQUENTIAL][CART_2_REGION_2] = 1 + (io->waitcnt.ws2_seq ? 1 : 8);
    access_time16[SEQUENTIAL][SRAM_REGION]     = 1 + gamepak_nonseq_waitstates[io->waitcnt.sram];

    // Update for 32-bit too.
    for (x = CART_0_REGION_1; x <= SRAM_REGION; ++x) {
        access_time32[NON_SEQUENTIAL][x] = access_time16[NON_SEQUENTIAL][x] + access_time16[SEQUENTIAL][x];
        access_time32[SEQUENTIAL][x] = 2 * access_time16[SEQUENTIAL][x];
    }
}

/*
** Calculate and add to the current cycle counter the amount of cycles needed for as many bus accesses
** are needed to transfer a data of the given size and access type.
*/
void
mem_access(
    struct gba *gba,
    uint32_t addr,
    uint32_t size,  // In bytes
    enum access_types access_type
) {
    uint32_t cycles;
    uint32_t page;

    addr = align_on(addr, size);
    page = (addr >> 24) & 0xF;

    if (unlikely(page >= CART_REGION_START && page <= CART_REGION_END && !(addr & 0x1FFFF))) {
        access_type = NON_SEQUENTIAL;
    }

    if (size <= sizeof(uint16_t)) {
        cycles = access_time16[access_type][page];
    } else {
        cycles = access_time32[access_type][page];
    }

    gba->memory.gamepak_bus_in_use = (page >= CART_REGION_START && page <= CART_REGION_END);
    if (gba->memory.gamepak_bus_in_use && gba->memory.pbuffer.enabled && !gba->core.is_dma_running) {
        mem_prefetch_buffer_access(gba, addr, cycles);
    } else {
        core_idle_for(gba, cycles);
    }
}

void
mem_prefetch_buffer_access(
    struct gba *gba,
    uint32_t addr,
    uint32_t intended_cycles
) {
    struct prefetch_buffer *pbuffer;

    pbuffer = &gba->memory.pbuffer;

    if (pbuffer->tail == addr) {
        if (pbuffer->size == 0) { // Finish to fetch if it isn't done yet
            gba->memory.gamepak_bus_in_use = false;
            core_idle_for(gba, pbuffer->countdown);

            pbuffer->tail += pbuffer->insn_len;
            --pbuffer->size;
        } else {
            pbuffer->tail += pbuffer->insn_len;
            --pbuffer->size;

            gba->memory.gamepak_bus_in_use = false;
            core_idle(gba);
        }
    } else {
        // Do it first or it'll screw our pbuffer settings
        core_idle_for(gba, intended_cycles);

        if (gba->core.cpsr.thumb) {
            pbuffer->insn_len = sizeof(uint16_t);
            pbuffer->capacity = 8;
            pbuffer->reload = access_time16[SEQUENTIAL][(addr >> 24) & 0xF];
        } else {
            pbuffer->insn_len = sizeof(uint32_t);
            pbuffer->capacity = 4;
            pbuffer->reload = access_time32[SEQUENTIAL][(addr >> 24) & 0xF];
        }

        pbuffer->countdown = pbuffer->reload;
        pbuffer->tail = addr + pbuffer->insn_len;
        pbuffer->head = pbuffer->tail;
        pbuffer->size = 0;
    }
}

void
mem_prefetch_buffer_step(
    struct gba *gba,
    uint32_t cycles
) {
    struct prefetch_buffer *pbuffer;

    pbuffer = &gba->memory.pbuffer;

    while (cycles >= pbuffer->countdown && pbuffer->size < pbuffer->capacity) {
        cycles -= pbuffer->countdown;
        pbuffer->head += pbuffer->insn_len;
        pbuffer->countdown = pbuffer->reload;
        ++pbuffer->size;
    }

    if (pbuffer->size < pbuffer->capacity) {
        pbuffer->countdown -= cycles;
    }
}

/*
** Determine the value returned by the BUS during an invalid memory access.
**
** Most of this is taken from GBATek, section "GBA Unpredictable Things".
*/
uint32_t
mem_openbus_read(
    struct gba const *gba,
    uint32_t addr
) {
    uint32_t val;
    uint32_t shift;

    shift = addr & 0x3;

    // On first access, open-bus during DMA transfers returns the last prefetched instruction.
    // On subsequent transfers it returns the the last transfered data.
    if (gba->memory.was_last_access_from_dma) {
        return gba->memory.dma_bus >> (8 * shift);
    }

    if (gba->core.cpsr.thumb) {
        uint32_t pc;

        pc = gba->core.pc;
        switch (pc >> 24) {
            case EWRAM_REGION:
            case PALRAM_REGION:
            case VRAM_REGION:
            case CART_0_REGION_1 ... CART_2_REGION_2: {
                val = gba->core.prefetch[1];
                val |= (gba->core.prefetch[1]) << 16;
                break;
            };
            case BIOS_REGION:
            case OAM_REGION: {
                if ((pc & 0x2) == 0) { // 4-byte aligned PC
                    val = gba->core.prefetch[1];
                    val |= (gba->core.prefetch[1]) << 16; // ???
                } else {
                    val = gba->core.prefetch[0];
                    val |= (gba->core.prefetch[1]) << 16;
                }
                break;
            };
            case IWRAM_REGION: {
                if ((pc & 0x2) == 0) { // 4-byte aligned PC
                    val = gba->core.prefetch[1];
                    val |= (gba->core.prefetch[0]) << 16;
                } else {
                    val = gba->core.prefetch[0];
                    val |= (gba->core.prefetch[1]) << 16;
                }
                break;
            };
                default: {
                    panic(HS_MEMORY, "Reading the open bus from an impossible page: %u", (unsigned int)(pc >> 24));
                    break;
                };
        }
    } else {
        val = gba->core.prefetch[1];
    }

    return (val >> (8 * shift));
}

/*
** Internal read functions without timing
*/
static inline __attribute__((always_inline)) uint8_t mem_read8_internal(struct gba *gba, uint32_t addr) {
    uint32_t region = addr >> 24;
    
    switch (region) {
        case BIOS_REGION: {
            if (addr <= BIOS_END) {
                uint32_t shift = 8 * (addr & 0b11);
                if (gba->core.pc <= BIOS_END) {
                    uint32_t aligned = addr & ~3;
                    gba->memory.bios_bus = *(uint32_t *)((uint8_t *)gba->memory.bios + aligned);
                }
                return gba->memory.bios_bus >> shift;
            } else {
                logln(HS_MEMORY, "Invalid BIOS read of size 1 from 0x%08x", addr);
                return mem_openbus_read(gba, addr);
            }
        }
        
        case EWRAM_REGION:
            return gba->memory.ewram[addr & EWRAM_MASK];
            
        case IWRAM_REGION:
            return gba->memory.iwram[addr & IWRAM_MASK];
            
        case IO_REGION:
            return mem_io_read8(gba, addr);
            
        case PALRAM_REGION:
            return gba->memory.palram[addr & PALRAM_MASK];
            
        case VRAM_REGION:
            return gba->memory.vram[addr & ((addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2)];
            
        case OAM_REGION:
            return gba->memory.oam[addr & OAM_MASK];
            
        case CART_REGION_START ... CART_REGION_END: {
            if (unlikely(
                (gba->memory.backup_storage.type == BACKUP_EEPROM_4K || 
                 gba->memory.backup_storage.type == BACKUP_EEPROM_64K) &&
                (addr & gba->memory.backup_storage.chip.eeprom.mask) == 
                 gba->memory.backup_storage.chip.eeprom.range
            )) {
                return mem_eeprom_read8(gba);
            } else if (unlikely(addr >= GPIO_REG_START && addr <= GPIO_REG_END && gba->gpio.readable)) {
                return gpio_read_u8(gba, addr);
            } else if (unlikely((addr & 0x00FFFFFF) >= gba->memory.rom_size)) {
                return (addr >> (1 + 8 * (addr & 1))) & 0xFF;
            } else {
                return gba->memory.rom[addr & CART_MASK];
            }
        }
        
        case SRAM_REGION:
        case SRAM_MIRROR_REGION:
            return mem_backup_storage_read8(gba, addr);
            
        default:
            logln(HS_MEMORY, "Invalid read of size 1 from 0x%08x", addr);
            return mem_openbus_read(gba, addr);
    }
}

static inline __attribute__((always_inline)) uint16_t mem_read16_internal(struct gba *gba, uint32_t addr) {
    addr &= ~1;  // Align to 16-bit
    uint32_t region = addr >> 24;
    
    switch (region) {
        case BIOS_REGION: {
            if (addr <= BIOS_END) {
                uint32_t shift = 8 * (addr & 0b11);
                if (gba->core.pc <= BIOS_END) {
                    uint32_t aligned = addr & ~3;
                    gba->memory.bios_bus = *(uint32_t *)((uint8_t *)gba->memory.bios + aligned);
                }
                return gba->memory.bios_bus >> shift;
            } else {
                logln(HS_MEMORY, "Invalid BIOS read of size 2 from 0x%08x", addr);
                return mem_openbus_read(gba, addr);
            }
        }
        
        case EWRAM_REGION:
            return *(uint16_t *)&gba->memory.ewram[addr & EWRAM_MASK];
            
        case IWRAM_REGION:
            return *(uint16_t *)&gba->memory.iwram[addr & IWRAM_MASK];
            
        case IO_REGION:
            return (mem_io_read8(gba, addr + 0) <<  0) |
                   (mem_io_read8(gba, addr + 1) <<  8);
            
        case PALRAM_REGION:
            return *(uint16_t *)&gba->memory.palram[addr & PALRAM_MASK];
            
        case VRAM_REGION:
            return *(uint16_t *)&gba->memory.vram[addr & ((addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2)];
            
        case OAM_REGION:
            return *(uint16_t *)&gba->memory.oam[addr & OAM_MASK];
            
        case CART_REGION_START ... CART_REGION_END: {
            if (unlikely(
                (gba->memory.backup_storage.type == BACKUP_EEPROM_4K || 
                 gba->memory.backup_storage.type == BACKUP_EEPROM_64K) &&
                (addr & gba->memory.backup_storage.chip.eeprom.mask) == 
                 gba->memory.backup_storage.chip.eeprom.range
            )) {
                return mem_eeprom_read8(gba);
            } else if (unlikely(addr >= GPIO_REG_START && addr <= GPIO_REG_END && gba->gpio.readable)) {
                return gpio_read_u8(gba, addr);
            } else if (unlikely((addr & 0x00FFFFFF) >= gba->memory.rom_size)) {
                return (addr >> 1) & 0xFFFF;
            } else {
                return *(uint16_t *)&gba->memory.rom[addr & CART_MASK];
            }
        }
        
        case SRAM_REGION:
        case SRAM_MIRROR_REGION: {
            uint8_t byte = mem_backup_storage_read8(gba, addr);
            return byte | (byte << 8);  // SRAM repeats the byte
        }
            
        default:
            logln(HS_MEMORY, "Invalid read of size 2 from 0x%08x", addr);
            return mem_openbus_read(gba, addr);
    }
}

static inline __attribute__((always_inline)) uint32_t mem_read32_internal(struct gba *gba, uint32_t addr) {
    addr &= ~3;  // Align to 32-bit
    uint32_t region = addr >> 24;
    
    switch (region) {
        case BIOS_REGION: {
            if (addr <= BIOS_END) {
                uint32_t shift = 8 * (addr & 0b11);
                if (gba->core.pc <= BIOS_END) {
                    uint32_t aligned = addr & ~3;
                    gba->memory.bios_bus = *(uint32_t *)((uint8_t *)gba->memory.bios + aligned);
                }
                return gba->memory.bios_bus >> shift;
            } else {
                logln(HS_MEMORY, "Invalid BIOS read of size 4 from 0x%08x", addr);
                return mem_openbus_read(gba, addr);
            }
        }
        
        case EWRAM_REGION:
            return *(uint32_t *)&gba->memory.ewram[addr & EWRAM_MASK];
            
        case IWRAM_REGION:
            return *(uint32_t *)&gba->memory.iwram[addr & IWRAM_MASK];
            
        case IO_REGION:
            return (mem_io_read8(gba, addr + 0) <<  0) |
                   (mem_io_read8(gba, addr + 1) <<  8) |
                   (mem_io_read8(gba, addr + 2) << 16) |
                   (mem_io_read8(gba, addr + 3) << 24);
            
        case PALRAM_REGION:
            return *(uint32_t *)&gba->memory.palram[addr & PALRAM_MASK];
            
        case VRAM_REGION:
            return *(uint32_t *)&gba->memory.vram[addr & ((addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2)];
            
        case OAM_REGION:
            return *(uint32_t *)&gba->memory.oam[addr & OAM_MASK];
            
        case CART_REGION_START ... CART_REGION_END: {
            if (unlikely(
                (gba->memory.backup_storage.type == BACKUP_EEPROM_4K || 
                 gba->memory.backup_storage.type == BACKUP_EEPROM_64K) &&
                (addr & gba->memory.backup_storage.chip.eeprom.mask) == 
                 gba->memory.backup_storage.chip.eeprom.range
            )) {
                return mem_eeprom_read8(gba);
            } else if (unlikely(addr >= GPIO_REG_START && addr <= GPIO_REG_END && gba->gpio.readable)) {
                return gpio_read_u8(gba, addr);
            } else if (unlikely((addr & 0x00FFFFFF) >= gba->memory.rom_size)) {
                return ((addr >> 1) & 0xFFFF) | ((((addr + 2) >> 1) & 0xFFFF) << 16);
            } else {
                return *(uint32_t *)&gba->memory.rom[addr & CART_MASK];
            }
        }
        
        case SRAM_REGION:
        case SRAM_MIRROR_REGION: {
            uint8_t byte = mem_backup_storage_read8(gba, addr);
            return byte * 0x01010101;  // SRAM repeats the byte
        }
            
        default:
            logln(HS_MEMORY, "Invalid read of size 4 from 0x%08x", addr);
            return mem_openbus_read(gba, addr);
    }
}

/*
** Internal write functions without timing
*/
static inline __attribute__((always_inline)) void mem_write8_internal(struct gba *gba, uint32_t addr, uint8_t val) {
    uint32_t region = addr >> 24;
    
    switch (region) {
        case BIOS_REGION:
            // Ignore writes to BIOS
            break;
            
        case EWRAM_REGION:
            gba->memory.ewram[addr & EWRAM_MASK] = val;
            break;
            
        case IWRAM_REGION:
            gba->memory.iwram[addr & IWRAM_MASK] = val;
            break;
            
        case IO_REGION:
            mem_io_write8(gba, addr, val);
            break;
            
        case PALRAM_REGION: {
            // 8-bit writes to PALRAM write to both upper and lower bytes
            addr &= ~1;
            gba->memory.palram[addr & PALRAM_MASK] = val;
            gba->memory.palram[(addr + 1) & PALRAM_MASK] = val;
            break;
        }
            
        case VRAM_REGION: {
            uint32_t vram_addr = addr & 0x1FFFF;
            // Ignore 8-bit writes to OBJ VRAM
            if ((gba->io.dispcnt.bg_mode <= 2 && vram_addr < 0x10000) ||
                (gba->io.dispcnt.bg_mode >= 3 && vram_addr < 0x14000)) {
                addr &= ~1;
                gba->memory.vram[addr & ((addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2)] = val;
                gba->memory.vram[(addr + 1) & (((addr + 1) & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2)] = val;
            }
            break;
        }
            
        case OAM_REGION:
            // Ignore 8-bit writes to OAM
            break;
            
        case CART_REGION_START ... CART_REGION_END: {
            if ((gba->memory.backup_storage.type == BACKUP_EEPROM_4K || 
                 gba->memory.backup_storage.type == BACKUP_EEPROM_64K) &&
                (addr & gba->memory.backup_storage.chip.eeprom.mask) == 
                 gba->memory.backup_storage.chip.eeprom.range) {
                mem_eeprom_write8(gba, val & 1);
            } else if (addr >= GPIO_REG_START && addr <= GPIO_REG_END) {
                gpio_write_u8(gba, addr, val);
            }
            // Otherwise ignore writes to ROM
            break;
        }
            
        case SRAM_REGION:
        case SRAM_MIRROR_REGION:
            mem_backup_storage_write8(gba, addr, val);
            break;
            
        default:
            logln(HS_MEMORY, "Invalid write of size 1 to 0x%08x", addr);
            break;
    }
}

static inline __attribute__((always_inline)) void mem_write16_internal(struct gba *gba, uint32_t addr, uint16_t val) {
    addr &= ~1;  // Align to 16-bit
    uint32_t region = addr >> 24;
    
    switch (region) {
        case BIOS_REGION:
            // Ignore writes to BIOS
            break;
            
        case EWRAM_REGION:
            *(uint16_t *)&gba->memory.ewram[addr & EWRAM_MASK] = val;
            break;
            
        case IWRAM_REGION:
            *(uint16_t *)&gba->memory.iwram[addr & IWRAM_MASK] = val;
            break;
            
        case IO_REGION:
            mem_io_write8(gba, addr + 0, (uint8_t)(val >> 0));
            mem_io_write8(gba, addr + 1, (uint8_t)(val >> 8));
            break;
            
        case PALRAM_REGION:
            *(uint16_t *)&gba->memory.palram[addr & PALRAM_MASK] = val;
            break;
            
        case VRAM_REGION:
            *(uint16_t *)&gba->memory.vram[addr & ((addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2)] = val;
            break;
            
        case OAM_REGION:
            *(uint16_t *)&gba->memory.oam[addr & OAM_MASK] = val;
            break;
            
        case CART_REGION_START ... CART_REGION_END: {
            if ((gba->memory.backup_storage.type == BACKUP_EEPROM_4K || 
                 gba->memory.backup_storage.type == BACKUP_EEPROM_64K) &&
                (addr & gba->memory.backup_storage.chip.eeprom.mask) == 
                 gba->memory.backup_storage.chip.eeprom.range) {
                mem_eeprom_write8(gba, val & 1);
            } else if (addr >= GPIO_REG_START && addr <= GPIO_REG_END) {
                gpio_write_u8(gba, addr, val);
            }
            // Otherwise ignore writes to ROM
            break;
        }
            
        case SRAM_REGION:
        case SRAM_MIRROR_REGION:
            // 16-bit writes to SRAM are rotated if unaligned
            mem_backup_storage_write8(gba, addr, val >> (8 * (addr % 2)));
            break;
            
        default:
            logln(HS_MEMORY, "Invalid write of size 2 to 0x%08x", addr);
            break;
    }
}

static inline __attribute__((always_inline)) void mem_write32_internal(struct gba *gba, uint32_t addr, uint32_t val) {
    addr &= ~3;  // Align to 32-bit
    uint32_t region = addr >> 24;
    
    switch (region) {
        case BIOS_REGION:
            // Ignore writes to BIOS
            break;
            
        case EWRAM_REGION:
            *(uint32_t *)&gba->memory.ewram[addr & EWRAM_MASK] = val;
            break;
            
        case IWRAM_REGION:
            *(uint32_t *)&gba->memory.iwram[addr & IWRAM_MASK] = val;
            break;
            
        case IO_REGION:
            mem_io_write8(gba, addr + 0, (uint8_t)(val >>  0));
            mem_io_write8(gba, addr + 1, (uint8_t)(val >>  8));
            mem_io_write8(gba, addr + 2, (uint8_t)(val >> 16));
            mem_io_write8(gba, addr + 3, (uint8_t)(val >> 24));
            break;
            
        case PALRAM_REGION:
            *(uint32_t *)&gba->memory.palram[addr & PALRAM_MASK] = val;
            break;
            
        case VRAM_REGION:
            *(uint32_t *)&gba->memory.vram[addr & ((addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2)] = val;
            break;
            
        case OAM_REGION:
            *(uint32_t *)&gba->memory.oam[addr & OAM_MASK] = val;
            break;
            
        case CART_REGION_START ... CART_REGION_END: {
            if ((gba->memory.backup_storage.type == BACKUP_EEPROM_4K || 
                 gba->memory.backup_storage.type == BACKUP_EEPROM_64K) &&
                (addr & gba->memory.backup_storage.chip.eeprom.mask) == 
                 gba->memory.backup_storage.chip.eeprom.range) {
                mem_eeprom_write8(gba, val & 1);
            } else if (addr >= GPIO_REG_START && addr <= GPIO_REG_END) {
                gpio_write_u8(gba, addr, val);
            }
            // Otherwise ignore writes to ROM
            break;
        }
            
        case SRAM_REGION:
        case SRAM_MIRROR_REGION:
            // 32-bit writes to SRAM are rotated if unaligned
            mem_backup_storage_write8(gba, addr, val >> (8 * (addr % 4)));
            break;
            
        default:
            logln(HS_MEMORY, "Invalid write of size 4 to 0x%08x", addr);
            break;
    }
}

/*
** Public API - Read functions
*/
uint8_t
mem_read8_raw(
    struct gba *gba,
    uint32_t addr
) {
    return mem_read8_internal(gba, addr);
}

uint8_t
mem_read8(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint8_t));
#endif

    mem_access(gba, addr, sizeof(uint8_t), access_type);
    return mem_read8_internal(gba, addr);
}

uint16_t
mem_read16_raw(
    struct gba *gba,
    uint32_t addr
) {
    return mem_read16_internal(gba, addr);
}

uint16_t
mem_read16(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint16_t));
#endif

    mem_access(gba, addr, sizeof(uint16_t), access_type);
    return mem_read16_internal(gba, addr);
}

uint32_t
mem_read16_ror(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
    uint32_t rotate;
    uint32_t value;

#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint16_t));
#endif

    mem_access(gba, addr, sizeof(uint16_t), access_type);

    rotate = (addr & 0b1) * 8;
    value = mem_read16_internal(gba, addr);

    /* Unaligned 16-bits loads are supposed to be unpredictable, but in practise the GBA rotates them */
    return (ror32(value, rotate));
}

uint32_t
mem_read32_raw(
    struct gba *gba,
    uint32_t addr
) {
    return mem_read32_internal(gba, addr);
}

uint32_t
mem_read32(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint32_t));
#endif

    mem_access(gba, addr, sizeof(uint32_t), access_type);
    return mem_read32_internal(gba, addr);
}

uint32_t
mem_read32_ror(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
    uint32_t rotate;
    uint32_t value;

#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint32_t));
#endif

    mem_access(gba, addr, sizeof(uint32_t), access_type);

    rotate = (addr % 4) << 3;
    value = mem_read32_internal(gba, addr);

    return (ror32(value, rotate));
}

/*
** Public API - Write functions
*/
void
mem_write8_raw(
    struct gba *gba,
    uint32_t addr,
    uint8_t val
) {
    mem_write8_internal(gba, addr, val);
}

void
mem_write8(
    struct gba *gba,
    uint32_t addr,
    uint8_t val,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_write_watchpoints(gba, addr, sizeof(uint8_t), val);
#endif

    mem_access(gba, addr, sizeof(uint8_t), access_type);
    mem_write8_internal(gba, addr, val);
}

void
mem_write16_raw(
    struct gba *gba,
    uint32_t addr,
    uint16_t val
) {
    mem_write16_internal(gba, addr, val);
}

void
mem_write16(
    struct gba *gba,
    uint32_t addr,
    uint16_t val,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_write_watchpoints(gba, addr, sizeof(uint16_t), val);
#endif

    mem_access(gba, addr, sizeof(uint16_t), access_type);
    mem_write16_internal(gba, addr, val);
}

void
mem_write32_raw(
    struct gba *gba,
    uint32_t addr,
    uint32_t val
) {
    mem_write32_internal(gba, addr, val);
}

void
mem_write32(
    struct gba *gba,
    uint32_t addr,
    uint32_t val,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_write_watchpoints(gba, addr, sizeof(uint32_t), val);
#endif

    mem_access(gba, addr, sizeof(uint32_t), access_type);
    mem_write32_internal(gba, addr, val);
}