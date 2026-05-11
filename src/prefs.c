#include "prefs.h"

#include "log.h"
#include "weapons.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- Name ↔ id lookups (duplicated from shotmode.c by design — this
 * module needs to stay self-contained so it can link into a future
 * `soldut-cli prefs` editor without dragging shotmode in). ---------- */

static int find_chassis_id(const char *name) {
    if (!name || !*name) return CHASSIS_TROOPER;
    for (int i = 0; i < CHASSIS_COUNT; ++i) {
        const Chassis *c = mech_chassis((ChassisId)i);
        if (c && c->name && strcasecmp(name, c->name) == 0) return i;
    }
    LOG_W("prefs: unknown chassis '%s' — keeping default", name);
    return CHASSIS_TROOPER;
}

static int find_weapon_id_local(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 32; ++i) {
        const char *n = weapon_short_name(i);
        if (!n || strcmp(n, "?") == 0) continue;
        if (strcasecmp(name, n) == 0) return i;
    }
    LOG_W("prefs: unknown weapon '%s' — keeping default", name);
    return default_id;
}

static int find_armor_id_local(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 8; ++i) {
        const Armor *a = armor_def(i);
        if (!a || !a->name) continue;
        if (strcasecmp(name, a->name) == 0) return i;
    }
    LOG_W("prefs: unknown armor '%s' — keeping default", name);
    return default_id;
}

static int find_jetpack_id_local(const char *name, int default_id) {
    if (!name || !*name) return default_id;
    for (int i = 0; i < 8; ++i) {
        const Jetpack *j = jetpack_def(i);
        if (!j || !j->name) continue;
        if (strcasecmp(name, j->name) == 0) return i;
    }
    LOG_W("prefs: unknown jetpack '%s' — keeping default", name);
    return default_id;
}

/* ---- API ---------------------------------------------------------- */

void prefs_defaults(UserPrefs *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "player");
    out->loadout = mech_default_loadout();
    out->team    = MATCH_TEAM_FFA;
    snprintf(out->connect_addr, sizeof out->connect_addr, "127.0.0.1:23073");
}

/* ---- key=value parser (lifted from config.c's apply_kv pattern,
 * minus the rotation-list handling we don't need here). ------------ */

static char *str_trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

static void apply_kv(UserPrefs *p, const char *key, char *val) {
    if (strcasecmp(key, "name") == 0) {
        snprintf(p->name, sizeof p->name, "%s", val);
    }
    else if (strcasecmp(key, "chassis") == 0) {
        p->loadout.chassis_id = find_chassis_id(val);
    }
    else if (strcasecmp(key, "primary") == 0) {
        p->loadout.primary_id = find_weapon_id_local(val, p->loadout.primary_id);
    }
    else if (strcasecmp(key, "secondary") == 0) {
        p->loadout.secondary_id = find_weapon_id_local(val, p->loadout.secondary_id);
    }
    else if (strcasecmp(key, "armor") == 0) {
        p->loadout.armor_id = find_armor_id_local(val, p->loadout.armor_id);
    }
    else if (strcasecmp(key, "jetpack") == 0) {
        p->loadout.jetpack_id = find_jetpack_id_local(val, p->loadout.jetpack_id);
    }
    else if (strcasecmp(key, "team") == 0) {
        int n = atoi(val);
        if (n >= 0 && n <= 2) p->team = n;
    }
    else if (strcasecmp(key, "connect_addr") == 0) {
        snprintf(p->connect_addr, sizeof p->connect_addr, "%s", val);
    }
    else {
        LOG_W("prefs: unknown key '%s'", key);
    }
}

bool prefs_load(UserPrefs *out, const char *path) {
    if (!out) return false;
    prefs_defaults(out);

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_I("prefs: %s not found — using defaults", path);
        return false;
    }

    char line[256];
    int  line_no = 0;
    while (fgets(line, sizeof line, f)) {
        line_no++;
        char *s = str_trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            LOG_W("prefs:%s:%d: missing '=' — skipped", path, line_no);
            continue;
        }
        *eq = '\0';
        char *key = str_trim(s);
        char *val = str_trim(eq + 1);
        char *hash = strchr(val, '#');
        if (hash) { *hash = '\0'; val = str_trim(val); }
        apply_kv(out, key, val);
    }
    fclose(f);
    LOG_I("prefs: loaded %s (name=%s chassis=%d weapons=%d/%d armor=%d jet=%d team=%d)",
          path, out->name,
          out->loadout.chassis_id, out->loadout.primary_id,
          out->loadout.secondary_id, out->loadout.armor_id,
          out->loadout.jetpack_id, out->team);
    return true;
}

bool prefs_save(const UserPrefs *p, const char *path) {
    if (!p || !path) return false;

    /* Atomic write: tmp file + rename. Same pattern as map_cache. */
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        LOG_W("prefs: cannot write %s (errno-like). Prefs not persisted.", tmp);
        return false;
    }

    const Chassis *c     = mech_chassis((ChassisId)p->loadout.chassis_id);
    const Armor   *a     = armor_def(p->loadout.armor_id);
    const Jetpack *j     = jetpack_def(p->loadout.jetpack_id);
    const char    *prim  = weapon_short_name(p->loadout.primary_id);
    const char    *sec   = weapon_short_name(p->loadout.secondary_id);

    fprintf(f, "# soldut-prefs.cfg — player preferences (auto-saved by the lobby UI).\n");
    fprintf(f, "# Lines starting with '#' are comments. Hand-editable; the game will\n");
    fprintf(f, "# overwrite this file when you change loadout / team / name in the UI.\n");
    fprintf(f, "name=%s\n",          p->name);
    fprintf(f, "chassis=%s\n",       c ? c->name : "Trooper");
    fprintf(f, "primary=%s\n",       prim ? prim : "Pulse Rifle");
    fprintf(f, "secondary=%s\n",     sec  ? sec  : "Sidearm");
    fprintf(f, "armor=%s\n",         a ? a->name : "Light");
    fprintf(f, "jetpack=%s\n",       j ? j->name : "Standard");
    fprintf(f, "team=%d\n",          p->team);
    fprintf(f, "connect_addr=%s\n",  p->connect_addr);

    if (fflush(f) != 0 || fclose(f) != 0) {
        LOG_W("prefs: failed to flush %s", tmp);
        remove(tmp);
        return false;
    }

    if (rename(tmp, path) != 0) {
        LOG_W("prefs: rename(%s -> %s) failed — prefs not persisted", tmp, path);
        remove(tmp);
        return false;
    }
    return true;
}
