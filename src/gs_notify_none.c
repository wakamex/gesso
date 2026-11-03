// gs_notify where gesso has no notifications for the system (Android, for now): gs_notify_init says false.
#include "gs_notify.h"

bool gs_notify_init(const char *name, const char *id, const char *icon, void (*clicked)(const char *tag)) { return false; }
bool gs_notify_show(const char *tag, const char *title, const char *body) { return false; }
void gs_notify_close(const char *tag) {}
