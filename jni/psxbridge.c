#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "libretro.h"

#define LOG_TAG "VIZO_PSX"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static void *core_handle = NULL;
static int core_initialized = 0;
static int game_loaded = 0;

static void (*p_retro_set_environment)(retro_environment_t);
static void (*p_retro_set_video_refresh)(retro_video_refresh_t);
static void (*p_retro_set_audio_sample)(retro_audio_sample_t);
static void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*p_retro_set_input_poll)(retro_input_poll_t);
static void (*p_retro_set_input_state)(retro_input_state_t);
static void (*p_retro_init)(void);
static void (*p_retro_deinit)(void);
static bool (*p_retro_load_game)(const struct retro_game_info *);
static void (*p_retro_unload_game)(void);
static void (*p_retro_run)(void);

static bool environment_callback(
    unsigned command,
    void *data
) {
    switch (command) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            if (data) {
                *(bool *)data = true;
                return true;
            }
            break;

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return false;

        default:
            break;
    }

    return false;
}

static void video_callback(
    const void *data,
    unsigned width,
    unsigned height,
    size_t pitch
) {
    (void)data;
    (void)width;
    (void)height;
    (void)pitch;
}

static void audio_callback(
    int16_t left,
    int16_t right
) {
    (void)left;
    (void)right;
}

static size_t audio_batch_callback(
    const int16_t *data,
    size_t frames
) {
    (void)data;
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
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    return 0;
}

static int load_symbol(void **target, const char *name) {
    *target = dlsym(core_handle, name);

    if (!*target) {
        LOGE("Missing symbol: %s", name);
        return 0;
    }

    return 1;
}

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_initialize(
    JNIEnv *env,
    jobject object
) {
    (void)env;
    (void)object;

    if (core_initialized) {
        return JNI_TRUE;
    }

    core_handle = dlopen(
        "libpcsx_rearmed_libretro_android.so",
        RTLD_NOW
    );

    if (!core_handle) {
        LOGE("Core loading failed: %s", dlerror());
        return JNI_FALSE;
    }

    if (!load_symbol((void **)&p_retro_set_environment, "retro_set_environment") ||
        !load_symbol((void **)&p_retro_set_video_refresh, "retro_set_video_refresh") ||
        !load_symbol((void **)&p_retro_set_audio_sample, "retro_set_audio_sample") ||
        !load_symbol((void **)&p_retro_set_audio_sample_batch, "retro_set_audio_sample_batch") ||
        !load_symbol((void **)&p_retro_set_input_poll, "retro_set_input_poll") ||
        !load_symbol((void **)&p_retro_set_input_state, "retro_set_input_state") ||
        !load_symbol((void **)&p_retro_init, "retro_init") ||
        !load_symbol((void **)&p_retro_deinit, "retro_deinit") ||
        !load_symbol((void **)&p_retro_load_game, "retro_load_game") ||
        !load_symbol((void **)&p_retro_unload_game, "retro_unload_game") ||
        !load_symbol((void **)&p_retro_run, "retro_run")) {

        dlclose(core_handle);
        core_handle = NULL;
        return JNI_FALSE;
    }

    p_retro_set_environment(environment_callback);
    p_retro_set_video_refresh(video_callback);
    p_retro_set_audio_sample(audio_callback);
    p_retro_set_audio_sample_batch(audio_batch_callback);
    p_retro_set_input_poll(input_poll_callback);
    p_retro_set_input_state(input_state_callback);

    p_retro_init();

    core_initialized = 1;

    LOGI("PCSX-ReARMed initialized");

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_psx_crash_PSXBridge_loadGame(
    JNIEnv *env,
    jobject object,
    jstring path
) {
    (void)object;

    if (!core_initialized || !path) {
        return JNI_FALSE;
    }

    const char *game_path =
        (*env)->GetStringUTFChars(env, path, NULL);

    if (!game_path) {
        return JNI_FALSE;
    }

    struct retro_game_info game;
    memset(&game, 0, sizeof(game));

    game.path = game_path;

    bool result = p_retro_load_game(&game);

    (*env)->ReleaseStringUTFChars(env, path, game_path);

    game_loaded = result ? 1 : 0;

    return result ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_runFrame(
    JNIEnv *env,
    jobject object
) {
    (void)env;
    (void)object;

    if (core_initialized && game_loaded) {
        p_retro_run();
    }
}

JNIEXPORT void JNICALL
Java_com_psx_crash_PSXBridge_shutdown(
    JNIEnv *env,
    jobject object
) {
    (void)env;
    (void)object;

    if (game_loaded) {
        p_retro_unload_game();
        game_loaded = 0;
    }

    if (core_initialized) {
        p_retro_deinit();
        core_initialized = 0;
    }

    if (core_handle) {
        dlclose(core_handle);
        core_handle = NULL;
    }

    LOGI("PCSX-ReARMed shutdown");
}
