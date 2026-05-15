#pragma once

#include "world.h"

/*
 * Map-placement validator (M6 m6-pickup-placement).
 *
 * Given a loaded Level, walks every pickup / spawn / flag and reports
 * any whose center sits inside a solid tile, inside a blocking
 * polygon, or outside the playable bounds. Used at cook time (hard
 * fail) and as a CI test against every shipped .lvl.
 *
 * Polygon kinds treated as blocking: SOLID, ICE, DEADLY. ONE_WAY and
 * BACKGROUND are walk-through. Tiles are blocking iff TILE_F_SOLID
 * is set.
 *
 * The validator only flags placements that are PROVABLY unreachable
 * (center embedded in a blocking volume). Reachability via jet/grapple
 * is design territory and not checked here.
 */

typedef enum {
    PLACEMENT_OK = 0,
    PLACEMENT_OUT_OF_BOUNDS,
    PLACEMENT_IN_SOLID_TILE,
    PLACEMENT_IN_SOLID_POLY,
} PlacementIssueKind;

typedef enum {
    PLACEMENT_ENTITY_PICKUP = 0,
    PLACEMENT_ENTITY_SPAWN,
    PLACEMENT_ENTITY_FLAG,
} PlacementEntityKind;

typedef struct {
    PlacementIssueKind  kind;
    PlacementEntityKind entity;
    int  index;          /* index in level->{pickups,spawns,flags} */
    int  x, y;           /* world position that flagged */
    int  detail;         /* tile-index or poly-index, depending on kind */
} PlacementIssue;

/* Walk the level and append issues. Returns the number of issues
 * written. Caller-supplied `issues` buffer must hold at least
 * `max_issues` entries. */
int placement_validate(const Level *level,
                       PlacementIssue *issues, int max_issues);

const char *placement_issue_kind_str (PlacementIssueKind k);
const char *placement_entity_kind_str(PlacementEntityKind e);
