#pragma once

#include <glib.h>
#include <freerdp/log.h>
void drd_log_init(void);
#define TAG CLIENT_TAG("deepin")
/*
 * 为日志自动附带文件名与行号，方便排障。
 * 通过 g_log_structured_standard 注入元信息，便于统一格式化。
 */
#define DRD_LOG_DEBUG(...)                                                                          \
    g_log_structured_standard(G_LOG_DOMAIN,                                                          \
                              G_LOG_LEVEL_DEBUG,                                                     \
                              __FILE__,                                                              \
                              G_STRINGIFY(__LINE__),                                                 \
                              G_STRFUNC,                                                             \
                              __VA_ARGS__)

#define DRD_LOG_MESSAGE(...)                                                                        \
    g_log_structured_standard(G_LOG_DOMAIN,                                                          \
                              G_LOG_LEVEL_MESSAGE,                                                   \
                              __FILE__,                                                              \
                              G_STRINGIFY(__LINE__),                                                 \
                              G_STRFUNC,                                                             \
                              __VA_ARGS__)

#define DRD_LOG_WARNING(...)                                                                        \
    g_log_structured_standard(G_LOG_DOMAIN,                                                          \
                              G_LOG_LEVEL_WARNING,                                                   \
                              __FILE__,                                                              \
                              G_STRINGIFY(__LINE__),                                                 \
                              G_STRFUNC,                                                             \
                              __VA_ARGS__)

#define DRD_LOG_ERROR(...)                                                                          \
    g_log_structured_standard(G_LOG_DOMAIN,                                                          \
                              G_LOG_LEVEL_CRITICAL,                                                  \
                              __FILE__,                                                              \
                              G_STRINGIFY(__LINE__),                                                 \
                              G_STRFUNC,                                                             \
                              __VA_ARGS__)
