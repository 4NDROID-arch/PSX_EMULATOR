#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#define LOG_TAG "VIZO_PSX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define AUDIO_CAPACITY 262144

static void *core_handle = NULL;
static ANativeWindow *native_window = NULL;

static pthread_mutex_t video_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t input_mutex = PTHREAD_MUTEX_INITIALIZER;

static char system_dir[1024];
static char save_dir[1024];

static int initialized = 0;
static int game_loaded = 0;

static unsigned pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;

static float core_fps = 60.0f;
static int core_sample_rate = 44100;

static uint16_t joypad_state[2];

static int16_t audio_ring[AUDIO_CAPACITY];
static unsigned audio_read_pos = 0;
static unsigned audio_write_pos = 0;

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

static void clear_audio(void) {
    pthread_mutex_lock(&audio_mutex);
    audio_read_pos = 0;
    audio_write_pos = 0;
    pthread_mutex_unlock(&audio_mutex);
}

static void push_audio(const int16_t *data, unsigned count) {
    pthread_mutex_lock(&audio_mutex);

    for (unsigned i = 0; i < count; i++) {
        unsigned next = (audio_write_pos + 1) % AUDIO_CAPACITY;

        if (next == audio_read_pos) {
            audio_read_pos = (audio_read_pos + 1) % AUDIO_CAPACITY;
        }

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
            enum retro_pixel_format format =
                *(enum retro_pixel_format *)data;

            if (format == RETRO_PIXEL_FORMAT_0RGB1555 ||
                format == RETRO_PIXEL_FORMAT_RGB565 ||
                format == RETRO_PIXEL_FORMAT_XRGB8888) {

                pixel_format = format;
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

static void video_callback(
    const void *data,
    unsigned width,
    unsigned height,
    size_t pitch
) {
    if (!data || width == 0 || height == 0) {
        return;
    }

    pthread_mutex_lock(&video_mutex);

    ANativeWindow *window = native_window;

    if (window) {
        ANativeWindow_acquire(window);
    }

    pthread_mutex_unlock(&video_mutex);

    if (!window) {
        return;
    }

    ANativeWindow_setBuffersGeometry(
        window,
        (int)width,
        (int)height,
        WINDOW_FORMAT_RGBA_8888
    );

    ANativeWindow_Buffer buffer;

    if (ANativeWindow_lock(window, &buffer, NULL) == 0) {

        uint8_t *dst = (uint8_t *)buffer.bits;

        for (unsigned y = 0; y < height && y < (unsigned)buffer.height; y++) {

            uint8_t *dst_row =
                dst + y * buffer.stride * 4;

            const uint8_t *src_row =
                (const uint8_t *)data + y * pitch;

            for (unsigned x = 0; x < width && x < (unsigned)buffer.width; x++) {

                unsigned r = 0;
                unsigned g = 0;
                unsigned b = 0;

                if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {

                    uint16_t p =
                        ((const uint16_t *)src_row)[x];

                    r = ((p >> 11) & 31) * 255 / 31;
                    g = ((p >> 5) & 63) * 255 / 63;
                    b = (p & 31) * 255 / 31;

                } else if (pixel_format == RETRO_PIXEL_FORMAT_0RGB1555) {

                    uint16_t p =
                        ((const uint16_t *)src_row)[x];

                    r = ((p >> 10) & 31) * 255 / 31;
                    g = ((p >> 5) & 31) * 255 / 31;
                    b = (p & 31) * 255 / 31;

                } else {

                    uint32_t p =
                        ((const uint32_t *)src_row)[x];

                    r = (p >> 16) & 255;
                    g = (p >> 8) & 255;
                    b = p & 255;
                }

                unsigned pos = x * 4;

                dst_row[pos] = (uint8_t)r;
                dst_row[pos + 1] = (uint8_t)g;
                dst_row[pos + 2] = (uint8_t)b;
                dst_row[pos + 3] = 255;
            }
        }

        ANativeWindow_unlockAndPost(window);
    }

    ANativeWindow_release(window);
}

static void audio_sample_callback(int16_t left, int16_t right) {
    int16_t samples[2];

    samples[0] = left;
    samples[1] = right;

    push_audio(samples, 2);
}

static size_t audio_batch_callback(
    const int16_t *data,
    size_t frames
) {
    if (data && frames > 0) {
        push_audio(data, (unsigned)(frames * 2));
    }

    return frames;
}

static void input_poll_callback(void) {
}

static int16_t input_state_callback(
    unsigned port,
    unsigned device,
    unsigned index,
    unsigned id
) {
    if (port >= 2 ||
        device != RETRO_DEVICE_JOYPAD ||
        index != 0 ||
        id >= 16) {
        return 0;
    }

    pthread_mutex_lock(&input_mutex);

    int result =
        (joypad_state[port] & (1u << id)) ? 1 : 0;

    pthread_mutex_unlock(&input_mutex);

    return (int16_t)result;
}

static void close_core(void) {

    if (game_loaded && retro_unload_game_fn) {
        retro_unload_game_fn();
    }

    game_loaded = 0;

    if (initialized && retro_deinit_fn) {
        retro_deinit_fn();
    }

    initialized = 0;

    if (core_handle) {
        dlclose(core_handle);
        core_handle = NULL;
    }

    pthread_mutex_lock(&video_mutex);

    if (native_window) {
        ANativeWindow_release(native_window);
        native_window = NULL;
    }

    pthread_mutex_unlock(&video_mutex);

    clear_audio();

    pthread_mutex_lock(&input_mutex);
    joypad_state[0] = 0;
    joypad_state[1] = 0;
    pthread_mutex_unlock(&input_mutex);

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
}

#define LOAD_FUNCTION(name, symbol) \
    do { \
        *(void **)(&name) = dlsym(core_handle, symbol); \
        if (!(name)) { \
            LOGE("Missing symbol: %s", symbol); \
            goto failed; \
        } \
    } while (0)

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_initialize(
    JNIEnv *env,
    jobject obj,
    jstring corePath,
    jstring systemDirectory,
    jstring saveDirectory
) {
    close_core();

    if (!corePath || !systemDirectory || !saveDirectory) {
        return JNI_FALSE;
    }

    const char *core =
        (*env)->GetStringUTFChars(env, corePath, NULL);

    const char *sys =
        (*env)->GetStringUTFChars(env, systemDirectory, NULL);

    const char *save =
        (*env)->GetStringUTFChars(env, saveDirectory, NULL);

    if (!core || !sys || !save) {
        if (core) (*env)->ReleaseStringUTFChars(env, corePath, core);
        if (sys) (*env)->ReleaseStringUTFChars(env, systemDirectory, sys);
        if (save) (*env)->ReleaseStringUTFChars(env, saveDirectory, save);
        return JNI_FALSE;
    }

    snprintf(system_dir, sizeof(system_dir), "%s", sys);
    snprintf(save_dir, sizeof(save_dir), "%s", save);

    core_handle = dlopen(core, RTLD_NOW | RTLD_LOCAL);

    (*env)->ReleaseStringUTFChars(env, corePath, core);
    (*env)->ReleaseStringUTFChars(env, systemDirectory, sys);
    (*env)->ReleaseStringUTFChars(env, saveDirectory, save);

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

    *(void **)(&retro_get_system_av_info_fn) =
        dlsym(core_handle, "retro_get_system_av_info");

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

        if (info.timing.fps > 1.0) {
            core_fps = (float)info.timing.fps;
        }

        if (info.timing.sample_rate > 1000.0) {
            core_sample_rate = (int)info.timing.sample_rate;
        }
    }

    LOGI("Core initialized. FPS: %.2f", core_fps);

    return JNI_TRUE;

failed:
    close_core();
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_setSurface(
    JNIEnv *env,
    jobject obj,
    jobject surface
) {
    ANativeWindow *new_window = NULL;

    if (surface) {
        new_window =
            ANativeWindow_fromSurface(env, surface);
    }

    pthread_mutex_lock(&video_mutex);

    if (native_window) {
        ANativeWindow_release(native_window);
    }

    native_window = new_window;

    pthread_mutex_unlock(&video_mutex);
}

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_loadGame(
    JNIEnv *env,
    jobject obj,
    jstring gamePath
) {
    if (!initialized || !gamePath || !retro_load_game_fn) {
        return JNI_FALSE;
    }

    if (game_loaded && retro_unload_game_fn) {
        retro_unload_game_fn();
        game_loaded = 0;
    }

    const char *path =
        (*env)->GetStringUTFChars(env, gamePath, NULL);

    if (!path) {
        return JNI_FALSE;
    }

    struct retro_game_info game;

    memset(&game, 0, sizeof(game));

    game.path = path;
    game.data = NULL;
    game.size = 0;
    game.meta = NULL;

    clear_audio();

    bool result = retro_load_game_fn(&game);

    (*env)->ReleaseStringUTFChars(env, gamePath, path);

    if (result) {
        game_loaded = 1;
        LOGI("Game loaded");
        return JNI_TRUE;
    }

    LOGE("Game loading failed");
    return JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_runFrame(
    JNIEnv *env,
    jobject obj
) {
    if (initialized && game_loaded && retro_run_fn) {
        retro_run_fn();
    }
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_setButton(
    JNIEnv *env,
    jobject obj,
    jint id,
    jboolean pressed
) {
    if (id < 0 || id >= 16) {
        return;
    }

    pthread_mutex_lock(&input_mutex);

    if (pressed) {
        joypad_state[0] |= (uint16_t)(1u << id);
    } else {
        joypad_state[0] &= (uint16_t)~(1u << id);
    }

    pthread_mutex_unlock(&input_mutex);
}

JNIEXPORT jint JNICALL
Java_com_psx_crash_PSXBridge_readAudio(
    JNIEnv *env,
    jobject obj,
    jshortArray output
) {
    if (!output) {
        return 0;
    }

    jsize capacity = (*env)->GetArrayLength(env, output);

    if (capacity <= 0) {
        return 0;
    }

    jshort *buffer =
        (*env)->GetShortArrayElements(env, output, NULL);

    if (!buffer) {
        return 0;
    }

    unsigned count = 0;

    pthread_mutex_lock(&audio_mutex);

    while (count < (unsigned)capacity &&
           audio_read_pos != audio_write_pos) {

        buffer[count++] = audio_ring[audio_read_pos];

        audio_read_pos =
            (audio_read_pos + 1) % AUDIO_CAPACITY;
    }

    pthread_mutex_unlock(&audio_mutex);

    (*env)->ReleaseShortArrayElements(
        env,
        output,
        buffer,
        0
    );

    return (jint)count;
}

JNIEXPORT jfloat JNICALL
Java_com_psx_crash_PSXBridge_getFPS(
    JNIEnv *env,
    jobject obj
) {
    return (jfloat)core_fps;
}

JNIEXPORT jint JNICALL
Java_com_psx_crash_PSXBridge_getSampleRate(
    JNIEnv *env,
    jobject obj
) {
    return (jint)core_sample_rate;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_shutdown(
    JNIEnv *env,
    jobject obj
) {
    close_core();
}
