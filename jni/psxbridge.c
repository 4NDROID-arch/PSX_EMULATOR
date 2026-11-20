#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "libretro.h"

#define LOG_TAG "VIZO_PSX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define AUDIO_CAPACITY 262144
#define STATE_MAGIC 0x56505358u
#define CHEAT_COUNT 30

static void *core_handle = NULL;
static ANativeWindow *native_window = NULL;
static pthread_mutex_t core_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t video_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

static char system_dir[1024], save_dir[1024], memcard_path[1024];
static unsigned frames_since_memcard_save = 0;
static int initialized = 0, game_loaded = 0;
static unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static float core_fps = 50.0f;
static int core_sample_rate = 44100;

static uint16_t joypad_state[2];
static int16_t audio_ring[AUDIO_CAPACITY];
static unsigned audio_read_pos = 0, audio_write_pos = 0;
static int audio_enabled = 1;

static void (*retro_set_environment_fn)(retro_environment_t);
static void (*retro_set_video_refresh_fn)(retro_video_refresh_t);
static void (*retro_set_audio_sample_fn)(retro_audio_sample_t);
static void (*retro_set_audio_sample_batch_fn)(retro_audio_sample_batch_t);
static void (*retro_set_input_poll_fn)(retro_input_poll_t);
static void (*retro_set_input_state_fn)(retro_input_state_t);
static void (*retro_init_fn)(void);
static void (*retro_deinit_fn)(void);
static bool (*retro_load_game_fn)(const struct retro_game_info *);
static void (*retro_unload_game_fn)(void);
static void (*retro_run_fn)(void);
static void (*retro_get_system_av_info_fn)(struct retro_system_av_info *);
static size_t (*retro_serialize_size_fn)(void);
static bool (*retro_serialize_fn)(void *, size_t);
static bool (*retro_unserialize_fn)(const void *, size_t);
static void (*retro_cheat_reset_fn)(void);
static void (*retro_cheat_set_fn)(unsigned, bool, const char *);
static void *(*retro_get_memory_data_fn)(unsigned);
static size_t (*retro_get_memory_size_fn)(unsigned);

static bool cheat_enabled[CHEAT_COUNT];

static const char *cheat_codes[CHEAT_COUNT] = {
    "30061948 0040",
    "800618EC 6300",
    "8000C000 0004\n8000C002 8E03\n8000C004 0000\n8000C006 349B\n8000C008 0063\n8000C00A 3405\n8000C00C 0165\n8000C00E A365\n8000C010 0000\n8000C012 3405\n8000C014 59FD\n8000C016 0800\n800167F0 3000\n800167F2 0800",
    "800242FA 1000",
    "80034520 FFFF",
    "80008360 000C\n80008362 3C02\n80008364 00A8\n80008366 AE42\n80008368 0010\n8000836A 3C02\n8000836C 8070\n8000836E 0800\n8001FEEC 20D8\n8001FEEE 0800",
    "8009E808 6300",
    "8009E584 6300",
    "8009E88C 6300",
    "8009E77C 6300",
    "8009E0F8 6300",
    "8009E5AC 6300",
    "8009E54C 6300",
    "8009E828 6300",
    "8009E64C 6300",
    "8009E778 6300",
    "8009E59C 6300",
    "8009E198 6300",
    "8009E508 6300",
    "8009E538 6300",
    "8009E7D0 6300",
    "8009E1A0 6300",
    "8009E0D0 6300",
    "8009E190 6300",
    "8009E0E0 6300",
    "8009E620 6300",
    "8009E610 6300",
    "8009E5F0 6300",
    "8009E818 6300",
    "8009E750 6300"
};

static void clear_audio(void) {
    pthread_mutex_lock(&audio_mutex);
    audio_read_pos = audio_write_pos = 0;
    pthread_mutex_unlock(&audio_mutex);
}

static void push_audio(const int16_t *data, unsigned count) {
    pthread_mutex_lock(&audio_mutex);
    if (!audio_enabled) {
        pthread_mutex_unlock(&audio_mutex);
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned next = (audio_write_pos + 1) % AUDIO_CAPACITY;
        if (next == audio_read_pos)
            audio_read_pos = (audio_read_pos + 1) % AUDIO_CAPACITY;
        audio_ring[audio_write_pos] = data[i];
        audio_write_pos = next;
    }
    pthread_mutex_unlock(&audio_mutex);
}

static bool environment_callback(unsigned cmd, void *data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = system_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = save_dir;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            enum retro_pixel_format f = *(enum retro_pixel_format *)data;
            if (f == RETRO_PIXEL_FORMAT_0RGB1555 ||
                f == RETRO_PIXEL_FORMAT_RGB565 ||
                f == RETRO_PIXEL_FORMAT_XRGB8888) {
                pixel_format = (unsigned)f;
                return true;
            }
            return false;
        }
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
            return false;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = false;
            return true;
        default:
            return false;
    }
}

static void video_callback(const void *data, unsigned width,
                           unsigned height, size_t pitch) {
    if (!data || !width || !height) return;

    pthread_mutex_lock(&video_mutex);
    ANativeWindow *w = native_window;
    if (w) ANativeWindow_acquire(w);
    pthread_mutex_unlock(&video_mutex);
    if (!w) return;

    ANativeWindow_setBuffersGeometry(w, (int)width, (int)height,
                                     WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer b;

    if (ANativeWindow_lock(w, &b, NULL) == 0) {
        uint8_t *dst = (uint8_t *)b.bits;
        unsigned maxY = height < (unsigned)b.height ? height : (unsigned)b.height;
        unsigned maxX = width < (unsigned)b.width ? width : (unsigned)b.width;

        for (unsigned y = 0; y < maxY; y++) {
            uint8_t *dr = dst + y * b.stride * 4;
            const uint8_t *sr = (const uint8_t *)data + y * pitch;
            for (unsigned x = 0; x < maxX; x++) {
                unsigned r, g, bl;
                if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
                    uint16_t v = ((const uint16_t *)sr)[x];
                    r = ((v >> 11) & 31) * 255 / 31;
                    g = ((v >> 5) & 63) * 255 / 63;
                    bl = (v & 31) * 255 / 31;
                } else if (pixel_format == RETRO_PIXEL_FORMAT_0RGB1555) {
                    uint16_t v = ((const uint16_t *)sr)[x];
                    r = ((v >> 10) & 31) * 255 / 31;
                    g = ((v >> 5) & 31) * 255 / 31;
                    bl = (v & 31) * 255 / 31;
                } else {
                    uint32_t v = ((const uint32_t *)sr)[x];
                    r = (v >> 16) & 255;
                    g = (v >> 8) & 255;
                    bl = v & 255;
                }
                unsigned q = x * 4;
                dr[q] = (uint8_t)r;
                dr[q + 1] = (uint8_t)g;
                dr[q + 2] = (uint8_t)bl;
                dr[q + 3] = 255;
            }
        }
        ANativeWindow_unlockAndPost(w);
    }
    ANativeWindow_release(w);
}

static void audio_sample_callback(int16_t l, int16_t r) {
    int16_t a[2] = {l, r};
    push_audio(a, 2);
}

static size_t audio_batch_callback(const int16_t *data, size_t frames) {
    if (data && frames) push_audio(data, (unsigned)(frames * 2));
    return frames;
}

static void input_poll_callback(void) {
}

static int16_t input_state_callback(unsigned port, unsigned device,
                                    unsigned index, unsigned id) {
    if (port >= 2 || device != RETRO_DEVICE_JOYPAD || index || id >= 16)
        return 0;
    pthread_mutex_lock(&input_mutex);
    int v = (joypad_state[port] & (1u << id)) ? 1 : 0;
    pthread_mutex_unlock(&input_mutex);
    return (int16_t)v;
}

static int ensure_directory(const char *path) {
    if (!path || !path[0]) return 0;
    char temp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(temp)) return 0;
    memcpy(temp, path, len + 1);

    while (len > 1 && temp[len - 1] == '/') temp[--len] = '\0';
    for (size_t i = 1; i <= len; i++) {
        if (temp[i] == '/' || temp[i] == '\0') {
            char saved = temp[i];
            temp[i] = '\0';
            if (mkdir(temp, 0775) != 0 && errno != EEXIST) {
                LOGE("Cannot create directory %s: errno=%d", temp, errno);
                temp[i] = saved;
                return 0;
            }
            temp[i] = saved;
        }
    }
    return 1;
}

static void load_memcard_locked(void) {
    if (!memcard_path[0]) {
        LOGE("Memory card path is empty");
        return;
    }
    if (!retro_get_memory_data_fn || !retro_get_memory_size_fn) {
        LOGE("Core does not expose memory card API");
        return;
    }

    void *data = retro_get_memory_data_fn(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size_fn(RETRO_MEMORY_SAVE_RAM);
    if (!data || size == 0 || size > 4u * 1024u * 1024u) {
        LOGE("Memory card RAM unavailable: data=%p size=%u", data, (unsigned)size);
        return;
    }

    FILE *f = fopen(memcard_path, "rb");
    if (!f) {
        LOGI("No existing memory card; a new one will be created: %s", memcard_path);
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        LOGE("Memory card seek failed: %s", memcard_path);
        fclose(f);
        return;
    }
    long file_size = ftell(f);
    rewind(f);
    if (file_size < 0 || (size_t)file_size != size) {
        LOGE("Memory card size mismatch: file=%ld core=%u path=%s",
             file_size, (unsigned)size, memcard_path);
        fclose(f);
        return;
    }

    size_t read_count = fread(data, 1, size, f);
    int read_error = ferror(f);
    fclose(f);
    if (read_count != size || read_error) {
        LOGE("Memory card read failed: read=%u expected=%u",
             (unsigned)read_count, (unsigned)size);
        return;
    }
    LOGI("Memory card loaded: %s (%u bytes)", memcard_path, (unsigned)size);
}

static void save_memcard_locked(void) {
    if (!game_loaded || !memcard_path[0]) return;
    if (!retro_get_memory_data_fn || !retro_get_memory_size_fn) {
        LOGE("Cannot save memory card: core memory API unavailable");
        return;
    }

    void *data = retro_get_memory_data_fn(RETRO_MEMORY_SAVE_RAM);
    size_t size = retro_get_memory_size_fn(RETRO_MEMORY_SAVE_RAM);
    if (!data || size == 0 || size > 4u * 1024u * 1024u) {
        LOGE("Cannot save memory card: data=%p size=%u", data, (unsigned)size);
        return;
    }
    if (!ensure_directory(save_dir)) {
        LOGE("Cannot create save directory: %s", save_dir);
        return;
    }

    char temp_path[1100];
    int n = snprintf(temp_path, sizeof(temp_path), "%s.tmp", memcard_path);
    if (n <= 0 || (size_t)n >= sizeof(temp_path)) {
        LOGE("Memory card temporary path is too long");
        return;
    }

    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        LOGE("Cannot open memory card for writing: %s errno=%d", temp_path, errno);
        return;
    }

    size_t written = fwrite(data, 1, size, f);
    int flush_ok = fflush(f) == 0;
    int sync_ok = fsync(fileno(f)) == 0;
    int close_ok = fclose(f) == 0;

    if (written != size || !flush_ok || !sync_ok || !close_ok) {
        LOGE("Memory card write failed: written=%u expected=%u flush=%d sync=%d close=%d",
             (unsigned)written, (unsigned)size, flush_ok, sync_ok, close_ok);
        remove(temp_path);
        return;
    }

    if (rename(temp_path, memcard_path) != 0) {
        LOGE("Memory card rename failed: errno=%d path=%s", errno, memcard_path);
        remove(temp_path);
        return;
    }
    LOGI("Memory card saved: %s (%u bytes)", memcard_path, (unsigned)size);
}
static void make_memcard_path(const char *game_path) {
    const char *base = strrchr(game_path, '/');
    base = base ? base + 1 : game_path;
    char name[512];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';
    snprintf(memcard_path, sizeof(memcard_path), "%s/%s.srm", save_dir, name);
}

static void close_core(void) {
    pthread_mutex_lock(&core_mutex);
    if (game_loaded && retro_unload_game_fn) {
        save_memcard_locked();
        retro_unload_game_fn();
    }
    game_loaded = 0;
    if (initialized && retro_deinit_fn) retro_deinit_fn();
    initialized = 0;
    if (core_handle) {
        dlclose(core_handle);
        core_handle = NULL;
    }

    retro_set_environment_fn = NULL;
    retro_set_video_refresh_fn = NULL;
    retro_set_audio_sample_fn = NULL;
    retro_set_audio_sample_batch_fn = NULL;
    retro_set_input_poll_fn = NULL;
    retro_set_input_state_fn = NULL;
    retro_init_fn = NULL;
    retro_deinit_fn = NULL;
    retro_load_game_fn = NULL;
    retro_unload_game_fn = NULL;
    retro_run_fn = NULL;
    retro_get_system_av_info_fn = NULL;
    retro_serialize_size_fn = NULL;
    retro_serialize_fn = NULL;
    retro_unserialize_fn = NULL;
    retro_cheat_reset_fn = NULL;
    retro_cheat_set_fn = NULL;
    retro_get_memory_data_fn = NULL;
    retro_get_memory_size_fn = NULL;
    memset(cheat_enabled, 0, sizeof(cheat_enabled));
    pthread_mutex_unlock(&core_mutex);

    pthread_mutex_lock(&video_mutex);
    if (native_window) {
        ANativeWindow_release(native_window);
        native_window = NULL;
    }
    pthread_mutex_unlock(&video_mutex);
    pthread_mutex_lock(&audio_mutex);
    audio_enabled = 1;
    audio_read_pos = audio_write_pos = 0;
    pthread_mutex_unlock(&audio_mutex);
    pthread_mutex_lock(&input_mutex);
    joypad_state[0] = joypad_state[1] = 0;
    pthread_mutex_unlock(&input_mutex);
}

#define LOAD_FUNCTION(n, s) \
    do { \
        *(void **)(&n) = dlsym(core_handle, s); \
        if (!(n)) { LOGE("Missing symbol: %s", s); goto failed; } \
    } while (0)

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_initialize(JNIEnv *e, jobject o,
                                          jstring cp, jstring sd, jstring sv) {
    close_core();
    if (!cp || !sd || !sv) return JNI_FALSE;

    const char *c = (*e)->GetStringUTFChars(e, cp, NULL);
    const char *s = (*e)->GetStringUTFChars(e, sd, NULL);
    const char *v = (*e)->GetStringUTFChars(e, sv, NULL);
    if (!c || !s || !v) {
        if (c) (*e)->ReleaseStringUTFChars(e, cp, c);
        if (s) (*e)->ReleaseStringUTFChars(e, sd, s);
        if (v) (*e)->ReleaseStringUTFChars(e, sv, v);
        return JNI_FALSE;
    }

    snprintf(system_dir, sizeof(system_dir), "%s", s);
    snprintf(save_dir, sizeof(save_dir), "%s", v);
    if (!ensure_directory(save_dir)) {
        LOGE("Save directory is not writable: %s", save_dir);
    }
    core_handle = dlopen(c, RTLD_NOW | RTLD_LOCAL);

    (*e)->ReleaseStringUTFChars(e, cp, c);
    (*e)->ReleaseStringUTFChars(e, sd, s);
    (*e)->ReleaseStringUTFChars(e, sv, v);

    if (!core_handle) {
        LOGE("dlopen failed: %s", dlerror());
        return JNI_FALSE;
    }

    LOAD_FUNCTION(retro_set_environment_fn, "retro_set_environment");
    LOAD_FUNCTION(retro_set_video_refresh_fn, "retro_set_video_refresh");
    LOAD_FUNCTION(retro_set_audio_sample_fn, "retro_set_audio_sample");
    LOAD_FUNCTION(retro_set_audio_sample_batch_fn, "retro_set_audio_sample_batch");
    LOAD_FUNCTION(retro_set_input_poll_fn, "retro_set_input_poll");
    LOAD_FUNCTION(retro_set_input_state_fn, "retro_set_input_state");
    LOAD_FUNCTION(retro_init_fn, "retro_init");
    LOAD_FUNCTION(retro_deinit_fn, "retro_deinit");
    LOAD_FUNCTION(retro_load_game_fn, "retro_load_game");
    LOAD_FUNCTION(retro_unload_game_fn, "retro_unload_game");
    LOAD_FUNCTION(retro_run_fn, "retro_run");

    *(void **)(&retro_get_system_av_info_fn) = dlsym(core_handle, "retro_get_system_av_info");
    *(void **)(&retro_serialize_size_fn) = dlsym(core_handle, "retro_serialize_size");
    *(void **)(&retro_serialize_fn) = dlsym(core_handle, "retro_serialize");
    *(void **)(&retro_unserialize_fn) = dlsym(core_handle, "retro_unserialize");
    *(void **)(&retro_cheat_reset_fn) = dlsym(core_handle, "retro_cheat_reset");
    *(void **)(&retro_cheat_set_fn) = dlsym(core_handle, "retro_cheat_set");
    *(void **)(&retro_get_memory_data_fn) = dlsym(core_handle, "retro_get_memory_data");
    *(void **)(&retro_get_memory_size_fn) = dlsym(core_handle, "retro_get_memory_size");

    if (!retro_serialize_size_fn || !retro_serialize_fn ||
        !retro_unserialize_fn || !retro_cheat_set_fn || !retro_cheat_reset_fn) {
        LOGE("Core missing required save/cheat API");
        goto failed;
    }

    retro_set_environment_fn(environment_callback);
    retro_set_video_refresh_fn(video_callback);
    retro_set_audio_sample_fn(audio_sample_callback);
    retro_set_audio_sample_batch_fn(audio_batch_callback);
    retro_set_input_poll_fn(input_poll_callback);
    retro_set_input_state_fn(input_state_callback);
    retro_init_fn();
    initialized = 1;

    if (retro_get_system_av_info_fn) {
        struct retro_system_av_info info;
        memset(&info, 0, sizeof(info));
        retro_get_system_av_info_fn(&info);
        if (info.timing.fps > 1.0) core_fps = (float)info.timing.fps;
        if (info.timing.sample_rate > 1000.0)
            core_sample_rate = (int)info.timing.sample_rate;
    }

    LOGI("Core initialized FPS %.2f", core_fps);
    return JNI_TRUE;

failed:
    close_core();
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_setSurface(JNIEnv *e, jobject o, jobject surf) {
    ANativeWindow *nw = surf ? ANativeWindow_fromSurface(e, surf) : NULL;
    pthread_mutex_lock(&video_mutex);
    if (native_window) ANativeWindow_release(native_window);
    native_window = nw;
    pthread_mutex_unlock(&video_mutex);
}

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_loadGame(JNIEnv *e, jobject o, jstring jp) {
    if (!initialized || !jp || !retro_load_game_fn) return JNI_FALSE;
    const char *p = (*e)->GetStringUTFChars(e, jp, NULL);
    if (!p) return JNI_FALSE;

    struct retro_game_info gi;
    memset(&gi, 0, sizeof(gi));
    gi.path = p;
    make_memcard_path(p);
    clear_audio();

    pthread_mutex_lock(&core_mutex);
    if (game_loaded && retro_unload_game_fn) {
        save_memcard_locked();
        retro_unload_game_fn();
    }
    game_loaded = 0;
    bool ok = retro_load_game_fn(&gi);
    if (ok) {
        game_loaded = 1;
        frames_since_memcard_save = 0;
        load_memcard_locked();
        memset(cheat_enabled, 0, sizeof(cheat_enabled));
        if (retro_cheat_reset_fn) retro_cheat_reset_fn();
    }
    pthread_mutex_unlock(&core_mutex);
    (*e)->ReleaseStringUTFChars(e, jp, p);

    if (ok) LOGI("Game loaded");
    else LOGE("Game load failed");
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_runFrame(JNIEnv *e, jobject o) {
    pthread_mutex_lock(&core_mutex);
    if (initialized && game_loaded && retro_run_fn) {
        retro_run_fn();
        frames_since_memcard_save++;
        if (frames_since_memcard_save >= 500) {
            save_memcard_locked();
            frames_since_memcard_save = 0;
        }
    }
    pthread_mutex_unlock(&core_mutex);
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_setButton(JNIEnv *e, jobject o,
                                        jint id, jboolean down) {
    /* IDs 0-15 control Player 1; IDs 16-31 control Player 2.
       This keeps the existing Java native method signature unchanged. */
    if (id < 0 || id >= 32) return;

    unsigned port = (unsigned)(id / 16);
    unsigned button = (unsigned)(id % 16);

    pthread_mutex_lock(&input_mutex);
    if (down)
        joypad_state[port] |= (uint16_t)(1u << button);
    else
        joypad_state[port] &= (uint16_t)~(1u << button);
    pthread_mutex_unlock(&input_mutex);
}

JNIEXPORT jint JNICALL
Java_com_psx_crash_PSXBridge_readAudio(JNIEnv *e, jobject o, jshortArray out) {
    if (!out) return 0;
    jsize cap = (*e)->GetArrayLength(e, out);
    if (cap <= 0) return 0;
    jshort *buf = (*e)->GetShortArrayElements(e, out, NULL);
    if (!buf) return 0;

    unsigned n = 0;
    pthread_mutex_lock(&audio_mutex);
    while (n < (unsigned)cap && audio_read_pos != audio_write_pos) {
        buf[n++] = audio_ring[audio_read_pos];
        audio_read_pos = (audio_read_pos + 1) % AUDIO_CAPACITY;
    }
    pthread_mutex_unlock(&audio_mutex);
    (*e)->ReleaseShortArrayElements(e, out, buf, 0);
    return (jint)n;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_setAudioEnabled(JNIEnv *e, jobject o,
                                               jboolean enabled) {
    pthread_mutex_lock(&audio_mutex);
    audio_enabled = enabled ? 1 : 0;
    audio_read_pos = audio_write_pos = 0;
    pthread_mutex_unlock(&audio_mutex);
}

JNIEXPORT jfloat JNICALL
Java_com_psx_crash_PSXBridge_getFPS(JNIEnv *e, jobject o) {
    return (jfloat)core_fps;
}

JNIEXPORT jint JNICALL
Java_com_psx_crash_PSXBridge_getSampleRate(JNIEnv *e, jobject o) {
    return (jint)core_sample_rate;
}

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_setCheat(JNIEnv *e, jobject o,
                                       jint idx, jboolean enabled) {
    if (idx < 0 || idx >= CHEAT_COUNT || !initialized ||
        !game_loaded || !retro_cheat_set_fn)
        return JNI_FALSE;

    pthread_mutex_lock(&core_mutex);
    retro_cheat_set_fn((unsigned)idx, enabled ? true : false, cheat_codes[idx]);
    cheat_enabled[idx] = enabled ? true : false;
    pthread_mutex_unlock(&core_mutex);
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_saveState(JNIEnv *e, jobject o, jstring path) {
    if (!path || !initialized || !game_loaded ||
        !retro_serialize_size_fn || !retro_serialize_fn)
        return JNI_FALSE;

    const char *p = (*e)->GetStringUTFChars(e, path, NULL);
    if (!p) return JNI_FALSE;

    pthread_mutex_lock(&core_mutex);
    size_t n = retro_serialize_size_fn();
    bool ok = false;

    if (n > 0 && n < 64u * 1024u * 1024u) {
        void *data = malloc(n);
        if (data && retro_serialize_fn(data, n)) {
            FILE *f = fopen(p, "wb");
            if (f) {
                uint32_t magic = STATE_MAGIC;
                uint64_t sz = (uint64_t)n;
                ok = fwrite(&magic, sizeof(magic), 1, f) == 1 &&
                     fwrite(&sz, sizeof(sz), 1, f) == 1 &&
                     fwrite(data, 1, n, f) == n;
                if (fclose(f) != 0) ok = false;
            }
        }
        free(data);
    }

    pthread_mutex_unlock(&core_mutex);
    (*e)->ReleaseStringUTFChars(e, path, p);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_loadState(JNIEnv *e, jobject o, jstring path) {
    if (!path || !initialized || !game_loaded ||
        !retro_serialize_size_fn || !retro_unserialize_fn)
        return JNI_FALSE;

    const char *p = (*e)->GetStringUTFChars(e, path, NULL);
    if (!p) return JNI_FALSE;

    pthread_mutex_lock(&core_mutex);
    bool ok = false;
    FILE *f = fopen(p, "rb");

    if (f) {
        uint32_t magic = 0;
        uint64_t sz = 0;
        if (fread(&magic, sizeof(magic), 1, f) == 1 &&
            fread(&sz, sizeof(sz), 1, f) == 1 &&
            magic == STATE_MAGIC && sz > 0 &&
            sz < 64u * 1024u * 1024u &&
            sz == retro_serialize_size_fn()) {
            void *data = malloc((size_t)sz);
            if (data) {
                if (fread(data, 1, (size_t)sz, f) == (size_t)sz)
                    ok = retro_unserialize_fn(data, (size_t)sz);
                free(data);
            }
        }
        fclose(f);
    }

    pthread_mutex_unlock(&core_mutex);
    (*e)->ReleaseStringUTFChars(e, path, p);
    clear_audio();
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_shutdown(JNIEnv *e, jobject o) {
    close_core();
}
