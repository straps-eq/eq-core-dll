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

static NPCEntry NPCs[] = {
    // raceID, modelName, genderID, dbStrID
    NPCEntry(728, "GBM", 2, 1),
};

// areCustomZonesEnabled if set to true will allow custom zones defined in Zones[] to be injected in game
bool areCustomZonesEnabled = false;

static ZoneEntry Zones[] = {
    // zoneType, zoneID, zoneShortName, zoneLongName, eqStrID, zoneFlags2, x, y, z
    ZoneEntry(0, 787, "hollows", "Darkened Hollows", 35154, 4, 0, 0, 0),
};
#endif