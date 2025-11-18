
#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gba/gba.h"
#include "gba/event.h"


#define ROM_PATH    "../assets/test.bin"
#define BIOS_PATH   "../assets/gba_bios.bin"
#define SAVE_PATH   "pokemon.sav"
#define WINDOW_SCALE 3

// Key mappings
#define SDL_KEY_UP      SDLK_w
#define SDL_KEY_DOWN    SDLK_s
#define SDL_KEY_LEFT    SDLK_a
#define SDL_KEY_RIGHT   SDLK_d
#define SDL_KEY_A       SDLK_p
#define SDL_KEY_B       SDLK_l
#define SDL_KEY_L       SDLK_e
#define SDL_KEY_R       SDLK_o
#define SDL_KEY_START   SDLK_RETURN
#define SDL_KEY_SELECT  SDLK_BACKSPACE
#define SDL_KEY_QUIT    SDLK_ESCAPE


// ============================================================================
// Global State
// ============================================================================

struct {
    struct gba *gba;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *framebuffer_tex;
    SDL_AudioDeviceID audio_device;
    pthread_t gba_thread;
    bool running;
    int audio_freq;
} app;

// ============================================================================
// Audio Callback
// ============================================================================

void audio_callback(void *userdata, uint8_t *raw_stream, int raw_len) {
    int16_t *stream = (int16_t *)raw_stream;
    size_t len = raw_len / (2 * sizeof(int16_t));
    
    gba_shared_audio_rbuffer_lock(app.gba);
    for (size_t i = 0; i < len; ++i) {
        uint32_t sample = gba_shared_audio_rbuffer_pop_sample(app.gba);
        stream[i * 2 + 0] = (int16_t)((sample >> 16) & 0xFFFF); 
        stream[i * 2 + 1] = (int16_t)(sample & 0xFFFF);         
    }
    gba_shared_audio_rbuffer_release(app.gba);
}

// ============================================================================
// File Loading
// ============================================================================

void *load_file(const char *path, size_t *size_out) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", path);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);
    
    void *data = calloc(1, size);
    if (fread(data, 1, size, f) != size) {
        fprintf(stderr, "Failed to read %s\n", path);
        free(data);
        fclose(f);
        return NULL;
    }
    
    fclose(f);
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
    msg.config.audio_frequency = GBA_CYCLES_PER_SECOND / app.audio_freq;
    msg.config.backup_storage.type = BACKUP_FLASH128;  // Auto-detect or hardcode
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
// Input Handling
// ============================================================================

void handle_key(SDL_Keycode key, bool pressed) {
    switch (key) {
        case SDL_KEY_UP:     gba_send_key(KEY_UP, pressed); break;
        case SDL_KEY_DOWN:   gba_send_key(KEY_DOWN, pressed); break;
        case SDL_KEY_LEFT:   gba_send_key(KEY_LEFT, pressed); break;
        case SDL_KEY_RIGHT:  gba_send_key(KEY_RIGHT, pressed); break;
        case SDL_KEY_A:      gba_send_key(KEY_A, pressed); break;
        case SDL_KEY_B:      gba_send_key(KEY_B, pressed); break;
        case SDL_KEY_L:      gba_send_key(KEY_L, pressed); break;
        case SDL_KEY_R:      gba_send_key(KEY_R, pressed); break;
        case SDL_KEY_START:  gba_send_key(KEY_START, pressed); break;
        case SDL_KEY_SELECT: gba_send_key(KEY_SELECT, pressed); break;
        case SDL_KEY_QUIT:   if (pressed) app.running = false; break;
    }
}

void handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                app.running = false;
                break;
            case SDL_KEYDOWN:
                handle_key(event.key.keysym.sym, true);
                break;
            case SDL_KEYUP:
                handle_key(event.key.keysym.sym, false);
                break;
        }
    }
}

// ============================================================================
// Initialization
// ============================================================================

bool init_sdl(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    
    // Create window
    app.window = SDL_CreateWindow(
        "Hades GBA Emulator",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        240 * WINDOW_SCALE,
        160 * WINDOW_SCALE,
        SDL_WINDOW_SHOWN
    );
    if (!app.window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    
    // Create software renderer
    app.renderer = SDL_CreateRenderer(
        app.window,
        -1,
        SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC
    );
    if (!app.renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    
    // Create framebuffer texture
    app.framebuffer_tex = SDL_CreateTexture(
        app.renderer,
        SDL_PIXELFORMAT_RGB565,  // Match PPU output format
        SDL_TEXTUREACCESS_STREAMING,
        240,
        160
    );

    if (!app.framebuffer_tex) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    
    // Setup audio
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = 48000;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 2048;
    want.callback = audio_callback;
    
    app.audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!app.audio_device) {
        fprintf(stderr, "SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    
    app.audio_freq = have.freq;
    
    printf("Audio: %d Hz, %d channels\n", have.freq, have.channels);
    return true;
}

void cleanup(void) {
    if (app.audio_device) {
        SDL_CloseAudioDevice(app.audio_device);
    }
    if (app.framebuffer_tex) {
        SDL_DestroyTexture(app.framebuffer_tex);
    }
    if (app.renderer) {
        SDL_DestroyRenderer(app.renderer);
    }
    if (app.window) {
        SDL_DestroyWindow(app.window);
    }
    SDL_Quit();
}

// ============================================================================
// Main Loop
// ============================================================================

int main(int argc, char *argv[]) {
    memset(&app, 0, sizeof(app));
    app.running = true;
    
    printf("Hades GBA Emulator - Minimal Build\n");
    printf("===================================\n");
    
    // Initialize SDL
    if (!init_sdl()) {
        return EXIT_FAILURE;
    }
    
    // Create GBA emulator
    app.gba = gba_create();
    if (!app.gba) {
        fprintf(stderr, "Failed to create GBA\n");
        cleanup();
        return EXIT_FAILURE;
    }
    SDL_PauseAudioDevice(app.audio_device, 0);

    // Start GBA thread
    pthread_create(&app.gba_thread, NULL, (void *(*)(void *))gba_run, app.gba);
    
    // Load ROM and start emulation
    if (!gba_load_and_start(ROM_PATH, BIOS_PATH, SAVE_PATH)) {
        app.running = false;
    } else {
        gba_send_run();
    }
    
    printf("\nControls:\n");
    printf("  WASD - D-Pad\n");
    printf("  P/L  - A/B\n");
    printf("  E/O  - L/R\n");
    printf("  Enter/Backspace - Start/Select\n");
    printf("  ESC  - Quit\n\n");
    
    // Main loop
    while (app.running) {
        handle_events();
        
        // Copy framebuffer from GBA
        gba_shared_framebuffer_lock(app.gba);
        SDL_UpdateTexture(
            app.framebuffer_tex,
            NULL,
            app.gba->shared_data.framebuffer.data,
            240 * 2  // GBA_SCREEN_WIDTH * sizeof(uint16_t) for RGB565
        );

        gba_shared_framebuffer_release(app.gba);
        
        // Render
        SDL_RenderClear(app.renderer);
        SDL_RenderCopy(app.renderer, app.framebuffer_tex, NULL, NULL);
        SDL_RenderPresent(app.renderer);
        
        SDL_Delay(1);
    }
    
    // Cleanup
    printf("Shutting down...\n");
    gba_send_exit();
    pthread_join(app.gba_thread, NULL);
    gba_delete(app.gba);
    cleanup();
    
    return EXIT_SUCCESS;
}