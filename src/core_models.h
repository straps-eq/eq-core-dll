#pragma once

#include "MQ2Main.h"

struct NPCEntry {
    explicit NPCEntry(int raceID, const char* modelName,  int genderID, int dbStrID) : raceID(raceID), modelName(modelName), genderID(genderID), dbStrID(dbStrID) {};
    int raceID;
    const char* modelName;			
    int genderID;
    int dbStrID;
};

struct ZoneEntry {
    explicit ZoneEntry(int zoneType, int zoneID, char* zoneShortName, char* zoneLongName, int eqStrID, int zoneFlags2, int x, int y, int z) : zoneType(zoneType), zoneID(zoneID), zoneShortName(zoneShortName), zoneLongName(zoneLongName), eqStrID(eqStrID), zoneFlags2(zoneFlags2), x(x), y(y), z(z) {};
    int zoneType;
    int zoneID;
    char* zoneShortName;
    char* zoneLongName;
    int eqStrID;
    int zoneFlags2;
    int x;
    int y;
    int z;
};