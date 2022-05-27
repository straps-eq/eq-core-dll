#include "core_models.h"
#ifndef OPTIONS_H
#define OPTIONS_H

// isMapWindowDisabled if set to true will disable the map in game
// If disabled, you need to remove MQ2Map.cpp/h references too else it'll populate NPC data on a map that doesn't exist
bool isMapWindowDisabled = false;

// areLuclinModelsDisabled if set to true disables the ability for luclin models to load, forcing classic models
bool areLuclinModelsDisabled = false;

// isBazaarWindowDisabled if set to true will disable the bazaar window in game
bool isBazaarWindowDisabled = false;

// areCustomNPCsEnabled if set to true will allow the NPCs defined in NPCs[] to be injected in game
bool areCustomNPCsEnabled = false;

// NPC Entry:
// raceID is the index. If it's a new NPC, start at 733. You'll need to update the rule NPC:MaxRaceID
// GenderID ranges from 0 to 2 usually
// modelName is the race's shortname tag
// raceMask is a range of flags, typically using 8 is safe for most NPCs, but e.g. 1 = drivable boat, 2 = ridable boat, etc
// dbStrID if left to 1 reverts to raceID as it's ID, otherwise you can custom set one, and it'll look up dbStr for info
static NPCEntry NPCs[] = {
    // raceID, genderID, modelName, raceMask, dbStrID
    NPCEntry(733, 2, "SHI", 3, 1),
};

// areCustomZonesEnabled if set to true will allow custom zones defined in Zones[] to be injected in game
bool areCustomZonesEnabled = false;

static ZoneEntry Zones[] = {
    // zoneType, zoneID, zoneShortName, zoneLongName, eqStrID, zoneFlags2, x, y, z
    ZoneEntry(0, 787, "hollows", "Darkened Hollows", 35154, 4, 0, 0, 0),
};
#endif