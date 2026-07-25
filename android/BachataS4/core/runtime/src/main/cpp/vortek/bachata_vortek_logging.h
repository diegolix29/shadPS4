#ifndef BACHATA_VORTEK_LOGGING_H
#define BACHATA_VORTEK_LOGGING_H

#include <android/log.h>
#include <stdio.h>

#define BACHATA_VORTEK_LOG_TAG "Bachata.Vortek"

#define BACHATA_VORTEK_LOGI(...) \
    __android_log_print(ANDROID_LOG_INFO, BACHATA_VORTEK_LOG_TAG, __VA_ARGS__)
#define BACHATA_VORTEK_LOGE(...) \
    __android_log_print(ANDROID_LOG_ERROR, BACHATA_VORTEK_LOG_TAG, __VA_ARGS__)
#define BACHATA_VORTEK_LOGW(...) \
    __android_log_print(ANDROID_LOG_WARN, BACHATA_VORTEK_LOG_TAG, __VA_ARGS__)

/* Also mirror to stderr for host-side capture when present. */
#define BACHATA_VORTEK_LOG(fmt, ...)                                       \
    do {                                                                   \
        BACHATA_VORTEK_LOGI(fmt, ##__VA_ARGS__);                           \
        fprintf(stderr, "[Bachata.Vortek] " fmt "\n", ##__VA_ARGS__);      \
    } while (0)

#define BACHATA_VORTEK_ERR(fmt, ...)                                       \
    do {                                                                   \
        BACHATA_VORTEK_LOGE(fmt, ##__VA_ARGS__);                           \
        fprintf(stderr, "[Bachata.Vortek] " fmt "\n", ##__VA_ARGS__);      \
    } while (0)

#endif
