#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dc/pvr.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include "gba/gba.h"
#include "gba/event.h"
#include <kos/fs.h>

#define ROM_PATH    "/cd/assets/test.gba"
#define BIOS_PATH   "/cd/assets/bios.bin"
#define SAVE_PATH   "pokemon.sav"

#define TEX_WIDTH   256  // Next power of 2 from 240
#define TEX_HEIGHT  256  // Next power of 2 from 160
#define GBA_WIDTH   240
#define GBA_HEIGHT  160

// ============================================================================
// Global State
// ============================================================================

struct {
    struct gba *gba;
    maple_device_t *controller;
    pthread_t gba_thread;
    bool running;
    
    // PVR rendering
    pvr_ptr_t pvram;
    uint32_t *pvram_sq;
    
    // Previous controller state for edge detection
    uint32_t prev_buttons;
} app;

// ============================================================================
// File Loading
// ============================================================================

void *load_file(const char *path, size_t *size_out) {
    file_t f = fs_open(path, O_RDONLY);
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", path);
        return NULL;
    }
    
    size_t size = fs_total(f);
    void *data = malloc(size);
    
    if (fs_read(f, data, size) != (ssize_t)size) {
        fprintf(stderr, "Failed to read %s\n", path);
        free(data);
        fs_close(f);
        return NULL;
    }
    
    fs_close(f);
    if (size_out) *size_out = size;
    return data;
}

// ============================================================================
// GBA Control
// ============================================================================

void send_message(struct event_header *msg) {
    channel_lock(&app.gba->channels.messages);
    channel_push(&app.gba->channels.messages, msg);
    channel_release(&app.gba->channels.messages);
}

void gba_send_key(enum keys key, bool pressed) {
    struct message_key msg;
    msg.header.kind = MESSAGE_KEY;
    msg.header.size = sizeof(msg);
    msg.key = key;
    msg.pressed = pressed;
    send_message(&msg.header);
}

void gba_send_run(void) {
    struct message msg;
    msg.header.kind = MESSAGE_RUN;
    msg.header.size = sizeof(msg);
    send_message(&msg.header);
}

void gba_send_exit(void) {
    struct message msg;
    msg.header.kind = MESSAGE_EXIT;
    msg.header.size = sizeof(msg);
    send_message(&msg.header);
}

bool gba_load_and_start(const char *rom_path, const char *bios_path, const char *save_path) {
    struct message_reset msg;
    memset(&msg, 0, sizeof(msg));
    
    msg.header.kind = MESSAGE_RESET;
    msg.header.size = sizeof(msg);
    
    // Load BIOS
    size_t bios_size;
    void *bios_data = load_file(bios_path, &bios_size);
    if (!bios_data || bios_size != 0x4000) {
        fprintf(stderr, "Invalid BIOS\n");
        free(bios_data);
        return false;
    }
    msg.config.bios.data = bios_data;
    msg.config.bios.size = bios_size;
    
    // Load ROM
    size_t rom_size;
    void *rom_data = load_file(rom_path, &rom_size);
    if (!rom_data) {
        free(bios_data);
        return false;
    }
    msg.config.rom.data = rom_data;
    msg.config.rom.size = rom_size;
    
    // Load save file (optional)
    size_t save_size;
    void *save_data = load_file(save_path, &save_size);
    if (save_data) {
        msg.config.backup_storage.data = save_data;
        msg.config.backup_storage.size = save_size;
        printf("Loaded save file: %s\n", save_path);
    }
    
    // Configure settings
    msg.config.skip_bios = false;  
    msg.config.backup_storage.type = BACKUP_FLASH128;
    msg.config.gpio_device_type = GPIO_NONE;
    
    // GBA settings
    msg.config.settings.speed = 1.0f;
    msg.config.settings.fast_forward = false;
    msg.config.settings.prefetch_buffer = true;
    msg.config.settings.enable_frame_skipping = false;
    msg.config.settings.ppu.enable_oam = true;
    memset(msg.config.settings.ppu.enable_bg_layers, 1, sizeof(msg.config.settings.ppu.enable_bg_layers));
    memset(msg.config.settings.apu.enable_psg_channels, 1, sizeof(msg.config.settings.apu.enable_psg_channels));
    memset(msg.config.settings.apu.enable_fifo_channels, 1, sizeof(msg.config.settings.apu.enable_fifo_channels));
    
    send_message(&msg.header);
    
    printf("ROM loaded: %s\n", rom_path);
    return true;
}

// ============================================================================
// Controller Input Handling
// ============================================================================

void handle_controller_input(void) {
    if (!app.controller) {
        // Try to find a controller
        app.controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if (app.controller) {
            printf("Controller connected\n");
        }
        return;
    }
    
    // Get controller state
    cont_state_t *state = (cont_state_t *)maple_dev_status(app.controller);
    if (!state) {
        app.controller = NULL;
        return;
    }
    
    uint32_t curr_buttons = state->buttons;
    uint32_t pressed = curr_buttons & ~app.prev_buttons;
    uint32_t released = ~curr_buttons & app.prev_buttons;
    
    // Handle button presses
    if (pressed & CONT_DPAD_UP)    gba_send_key(KEY_UP, true);
    if (pressed & CONT_DPAD_DOWN)  gba_send_key(KEY_DOWN, true);
    if (pressed & CONT_DPAD_LEFT)  gba_send_key(KEY_LEFT, true);
    if (pressed & CONT_DPAD_RIGHT) gba_send_key(KEY_RIGHT, true);
    if (pressed & CONT_A)          gba_send_key(KEY_A, true);
    if (pressed & CONT_B)          gba_send_key(KEY_B, true);
    if (pressed & CONT_X)          gba_send_key(KEY_L, true);
    if (pressed & CONT_Y)          gba_send_key(KEY_R, true);
    if (pressed & CONT_START)      gba_send_key(KEY_START, true);
    
    // Handle button releases
    if (released & CONT_DPAD_UP)    gba_send_key(KEY_UP, false);
    if (released & CONT_DPAD_DOWN)  gba_send_key(KEY_DOWN, false);
    if (released & CONT_DPAD_LEFT)  gba_send_key(KEY_LEFT, false);
    if (released & CONT_DPAD_RIGHT) gba_send_key(KEY_RIGHT, false);
    if (released & CONT_A)          gba_send_key(KEY_A, false);
    if (released & CONT_B)          gba_send_key(KEY_B, false);
    if (released & CONT_X)          gba_send_key(KEY_L, false);
    if (released & CONT_Y)          gba_send_key(KEY_R, false);
    if (released & CONT_START)      gba_send_key(KEY_START, false);
    
    // Check for exit condition (A+B+X+Y+Start)
    if ((curr_buttons & CONT_RESET_BUTTONS) == CONT_RESET_BUTTONS) {
        app.running = false;
    }
    
    app.prev_buttons = curr_buttons;
}

// ============================================================================
// Rendering
// ============================================================================

void present_gba_frame(void) {
    if (!app.pvram_sq) return;
    
    // Lock and copy GBA framebuffer to VRAM
    gba_shared_framebuffer_lock(app.gba);
    uint32_t *src = (uint32_t *)app.gba->shared_data.framebuffer.data;
    
    // Convert ABGR8888 to RGB565 and copy to texture memory
    for (int y = 0; y < GBA_HEIGHT; y++) {
        uint32_t *dest_line32 = app.pvram_sq + (TEX_WIDTH / 2) * y;
        uint16_t *dest_line16 = (uint16_t *)dest_line32;
        sq_lock(dest_line32);
        for (int x = 0; x < GBA_WIDTH; x++) {
            uint32_t abgr = src[y * GBA_WIDTH + x];
            uint8_t r = abgr & 0xFF;
            uint8_t g = (abgr >> 8) & 0xFF;
            uint8_t b = (abgr >> 16) & 0xFF;
            dest_line16[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
        }
        sq_unlock();
    }
    
    gba_shared_framebuffer_release(app.gba);
    
    // Render the texture as a fullscreen quad
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;
    
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
        PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
        TEX_WIDTH, TEX_HEIGHT, app.pvram, PVR_FILTER_BILINEAR);
    
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));
    
    vert.argb = PVR_PACK_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
    vert.oargb = 0;
    vert.flags = PVR_CMD_VERTEX;
    
    // Calculate UV coordinates for actual GBA resolution within texture
    float u_max = (float)GBA_WIDTH / TEX_WIDTH;
    float v_max = (float)GBA_HEIGHT / TEX_HEIGHT;
    
    // Top-left
    vert.x = 0.0f; vert.y = 0.0f; vert.z = 1.0f;
    vert.u = 0.0f; vert.v = 0.0f;
    pvr_prim(&vert, sizeof(vert));
    
    // Top-right
    vert.x = 640.0f; vert.y = 0.0f;
    vert.u = u_max; vert.v = 0.0f;
    pvr_prim(&vert, sizeof(vert));
    
    // Bottom-left
    vert.x = 0.0f; vert.y = 480.0f;
    vert.u = 0.0f; vert.v = v_max;
    pvr_prim(&vert, sizeof(vert));
    
    // Bottom-right
    vert.x = 640.0f; vert.y = 480.0f;
    vert.u = u_max; vert.v = v_max;
    vert.flags = PVR_CMD_VERTEX_EOL;
    pvr_prim(&vert, sizeof(vert));
    
    pvr_list_finish();
    pvr_scene_finish();
}

// ============================================================================
// System Initialization
// ============================================================================

bool init_system(void) {
    // Initialize PVR
    pvr_init_defaults();
    
    // Allocate texture memory
    app.pvram = pvr_mem_malloc(TEX_WIDTH * TEX_HEIGHT * 2);
    if (!app.pvram) {
        fprintf(stderr, "Failed to allocate PVR memory\n");
        return false;
    }
    
    // Get store queue address
    app.pvram_sq = (uint32_t *)(((uintptr_t)app.pvram & 0xFFFFFF) | PVR_TA_TEX_MEM);
    
    // Initialize controller subsystem
    cont_init();
    

    return true;
}

void cleanup(void) {
    if (app.pvram) {
        pvr_mem_free(app.pvram);
    }
    
    cont_shutdown();
    pvr_shutdown();
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    memset(&app, 0, sizeof(app));
    app.running = true;
    
    printf("Hades GBA Emulator - Dreamcast KOS Build\n");
    printf("=========================================\n");
    
    if (!init_system()) {
        return EXIT_FAILURE;
    }
    
    app.gba = gba_create();
    if (!app.gba) {
        fprintf(stderr, "Failed to create GBA\n");
        cleanup();
        return EXIT_FAILURE;
    }
    
    pthread_create(&app.gba_thread, NULL, (void *(*)(void *))gba_run, app.gba);
    
    if (!gba_load_and_start(ROM_PATH, BIOS_PATH, SAVE_PATH)) {
        app.running = false;
    } else {
        gba_send_run();
    }
    
    // Main loop
    while (app.running) {
        handle_controller_input();
        present_gba_frame();
    }
    
    // Cleanup
    printf("Shutting down...\n");
    gba_send_exit();
    pthread_join(app.gba_thread, NULL);
    gba_delete(app.gba);
    cleanup();
    
    return EXIT_SUCCESS;
}