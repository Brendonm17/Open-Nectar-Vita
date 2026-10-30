/* printf/fprintf/puts redirect to the Vita log file. Force-included. */
#ifndef NECTAR_VITA_LOG_H
#define NECTAR_VITA_LOG_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
int vita_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int vita_log_fprintf(FILE* stream, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int vita_log_puts(const char* s);
#ifdef __cplusplus
}
namespace std {
using ::vita_log_printf;
using ::vita_log_fprintf;
using ::vita_log_puts;
}
#endif
#define printf(...)  vita_log_printf(__VA_ARGS__)
#define fprintf(...) vita_log_fprintf(__VA_ARGS__)
#define puts(s)      vita_log_puts(s)
#endif /* NECTAR_VITA_LOG_H */
