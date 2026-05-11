#include "config.h"

#include "log.h"
#include "maps.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_defaults(ServerConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->port               = SOLDUT_DEFAULT_PORT;
    cfg->max_players        = 16;
    cfg->snapshot_hz        = 60;           /* Phase 2 — was 30 (M2 default) */
    cfg->mode               = MATCH_MODE_FFA;
    cfg->score_limit        = 25;
    cfg->time_limit         = 600.0f;       /* 10 min */
    cfg->friendly_fire      = false;
    cfg->auto_start_seconds = 60.0f;
    cfg->rounds_per_match   = 3;
    cfg->map_rotation[0]    = MAP_FOUNDRY;
    cfg->map_rotation_count = 1;
    cfg->mode_rotation[0]   = MATCH_MODE_FFA;
    cfg->mode_rotation_count= 1;
    cfg->loaded_from_file   = false;
    cfg->source_path[0]     = '\0';
}

/* ---- Tiny key=value parser ----------------------------------------- */

static char *str_trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* Split a comma-list "foo, bar, baz" into out_tokens (NUL-terminated
 * substrings within `buf`). Returns count. */
static int split_list(char *buf, char **tokens, int max_tokens) {
    int n = 0;
    char *p = buf;
    while (*p && n < max_tokens) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        tokens[n++] = p;
        while (*p && *p != ',') p++;
        if (*p) {
            *p = '\0';
            p++;
        }
        tokens[n - 1] = str_trim(tokens[n - 1]);
    }
    return n;
}

static void apply_kv(ServerConfig *cfg, const char *key, char *val) {
    if (strcasecmp(key, "port") == 0) {
        int p = atoi(val);
        if (p > 0 && p <= 65535) cfg->port = (uint16_t)p;
        else LOG_W("config: bad port '%s'", val);
    }
    else if (strcasecmp(key, "max_players") == 0) {
        int n = atoi(val);
        if (n >= 1 && n <= 32) cfg->max_players = n;
        else LOG_W("config: max_players '%s' out of range (1..32)", val);
    }
    else if (strcasecmp(key, "snapshot_hz") == 0) {
        int n = atoi(val);
        if (n >= 10 && n <= 60) cfg->snapshot_hz = n;
        else LOG_W("config: snapshot_hz '%s' out of range (10..60)", val);
    }
    else if (strcasecmp(key, "mode") == 0) {
        cfg->mode = match_mode_from_name(val);
    }
    else if (strcasecmp(key, "score_limit") == 0) {
        int s = atoi(val);
        if (s > 0) cfg->score_limit = s;
    }
    else if (strcasecmp(key, "time_limit") == 0) {
        float t = (float)atof(val);
        if (t > 0.0f) cfg->time_limit = t;
    }
    else if (strcasecmp(key, "friendly_fire") == 0 ||
             strcasecmp(key, "ff") == 0) {
        cfg->friendly_fire = (atoi(val) != 0) ||
                             (strcasecmp(val, "true") == 0) ||
                             (strcasecmp(val, "yes") == 0) ||
                             (strcasecmp(val, "on") == 0);
    }
    else if (strcasecmp(key, "auto_start_seconds") == 0) {
        float s = (float)atof(val);
        if (s > 0.0f) cfg->auto_start_seconds = s;
    }
    else if (strcasecmp(key, "rounds_per_match") == 0 ||
             strcasecmp(key, "match_rounds") == 0) {
        int n = atoi(val);
        if (n > 0 && n <= 32) cfg->rounds_per_match = n;
        else LOG_W("config: rounds_per_match '%s' out of range (1..32)", val);
    }
    else if (strcasecmp(key, "map_rotation") == 0) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", val);
        char *toks[CONFIG_ROTATION_MAX];
        int n = split_list(buf, toks, CONFIG_ROTATION_MAX);
        int written = 0;
        for (int i = 0; i < n; ++i) {
            int id = map_id_from_name(toks[i]);
            if (id >= 0) {
                cfg->map_rotation[written++] = id;
            } else {
                LOG_W("config: unknown map '%s' in map_rotation", toks[i]);
            }
        }
        if (written > 0) cfg->map_rotation_count = written;
    }
    else if (strcasecmp(key, "mode_rotation") == 0) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", val);
        char *toks[CONFIG_ROTATION_MAX];
        int n = split_list(buf, toks, CONFIG_ROTATION_MAX);
        int written = 0;
        for (int i = 0; i < n; ++i) {
            cfg->mode_rotation[written++] = match_mode_from_name(toks[i]);
        }
        if (written > 0) cfg->mode_rotation_count = written;
    }
    else {
        LOG_W("config: unknown key '%s'", key);
    }
}

bool config_load(ServerConfig *cfg, const char *path) {
    if (!cfg || !path) return false;
    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_I("config: %s not found — using defaults", path);
        return false;
    }

    snprintf(cfg->source_path, sizeof cfg->source_path, "%s", path);
    cfg->loaded_from_file = true;

    char line[512];
    int line_no = 0;
    while (fgets(line, sizeof line, f)) {
        line_no++;
        char *s = str_trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) {
            LOG_W("config:%s:%d: missing '=' — skipped", path, line_no);
            continue;
        }
        *eq = '\0';
        char *key = str_trim(s);
        char *val = str_trim(eq + 1);
        /* Strip an inline '# comment' from the value. */
        char *hash = strchr(val, '#');
        if (hash) { *hash = '\0'; val = str_trim(val); }
        apply_kv(cfg, key, val);
    }
    fclose(f);
    LOG_I("config: loaded %s (mode=%s map=%d max=%d)",
          path, match_mode_name(cfg->mode),
          cfg->map_rotation[0], cfg->max_players);
    return true;
}

int config_pick_map(const ServerConfig *cfg, int round_index) {
    if (!cfg || cfg->map_rotation_count <= 0) return MAP_FOUNDRY;
    if (round_index < 0) round_index = 0;
    return cfg->map_rotation[round_index % cfg->map_rotation_count];
}

MatchModeId config_pick_mode(const ServerConfig *cfg, int round_index) {
    if (!cfg || cfg->mode_rotation_count <= 0)
        return cfg ? cfg->mode : MATCH_MODE_FFA;
    if (round_index < 0) round_index = 0;
    return cfg->mode_rotation[round_index % cfg->mode_rotation_count];
}
