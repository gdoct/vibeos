#ifndef VIBEOS_CONFIG_H
#define VIBEOS_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * System configuration service (ROADMAP: "Config + service-managed init").
 *
 * The kernel reads /config/system.conf at boot into an in-memory key/value
 * store and lets subsystems query it (hostname, motd, log level, …). The file
 * is a small YAML-ish format: `key: value` per line, `#` comments, blank lines
 * ignored, dotted keys for grouping (e.g. `net.ip`). Values are taken literally
 * after trimming surrounding whitespace and optional single/double quotes.
 *
 * config_reload() re-reads the file, so a userspace tool can edit /config and
 * have the live settings update without a reboot.
 */

#define CONFIG_PATH      "/config/system.conf"

void        config_init(void);                 /* load CONFIG_PATH (call after fs_mount) */
int         config_reload(void);               /* re-read; returns entry count or <0 */
const char *config_get(const char *key);       /* value or NULL */
const char *config_get_def(const char *key, const char *def);
int         config_get_int(const char *key, int def);
int         config_get_bool(const char *key, int def);   /* true/yes/on/1 -> 1 */

/* Iterate the store (for /proc/config and the sysconf tool). */
int         config_count(void);
int         config_entry(int i, const char **key, const char **val);

#ifdef __cplusplus
}
#endif

#endif
