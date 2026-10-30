#ifndef PANEL_PROTOCOL_H
#define PANEL_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define PANEL_MAX_LINE 24576
#define PANEL_MAX_MACHINES 128
#define PANEL_STALE_MS 10000u
#define PANEL_PARTIAL_MS 3000u
enum { UNKNOWN, RUNNING, STOPPED };
enum { HOST_UNKNOWN, HOST_OK, HOST_ERROR };
typedef struct { char name[33], kind[3]; int id, state; } Machine;
typedef struct {
    char host[33];
    char storage[33];
    int cpu_percent, host_state, demo;
    double ram_used, ram_total, disk_used, disk_total; // GiB; total <= 0 means absent.
    Machine machines[PANEL_MAX_MACHINES];
    size_t count;
} PanelData;
typedef struct {
    char line[PANEL_MAX_LINE+1];
    size_t used;
    int dropping;
    uint32_t partial_start, last_valid;
    unsigned valid, samples, rejected;
} PanelStream;
int panel_parse(const char *json, size_t length, PanelData *out);
void panel_stream_expire(PanelStream *s, uint32_t now);
int panel_stream_feed(PanelStream *s, const char *bytes, size_t n, uint32_t now, PanelData *out);
int panel_stream_stale(const PanelStream *s, uint32_t now);
#endif
