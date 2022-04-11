#pragma once

#include "MQ2Main.h"

struct NPCEntry {
    explicit NPCEntry(int raceID, const char* modelName,  int genderID, int dbStrID) : raceID(raceID), modelName(modelName), genderID(genderID), dbStrID(dbStrID) {};
    int raceID;
    const char* modelName;			
    int genderID;
    int dbStrID;
};