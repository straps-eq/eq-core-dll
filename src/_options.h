#include "core_models.h"
#ifndef OPTIONS_H
#define OPTIONS_H

// isGammaRestoreOnCrashEnabled is recommended to be true. When eqgame crashes, this restores your gamma. future EQ clients have this enabled.
// This also may fix eq trying to modify your gamma settings
bool isGammaRestoreOnCrashEnabled = false;

// isNativeGammaEnabled is recommended to be true, this disables the gamma slider in game from doing anything, and never lets EQ adjust gamma
// if this is enabled, isGammaRestoreOnCrashEnabled is ignored since it isn't needed
bool isNativeGammaEnabled = false;

// isCpuSpeedFixEnabled is recommended to be true. Some CPUs have a symptom where the game runs too fast, future EQ clients have this enabled
// if a person reports the game running too fast, try enabling and giving this DLL.
// if this is enabled and your animations are going sluggish, it may be an incorrect cpu check. Let Xackery know in discord
bool isCpuSpeedFixEnabled = false;

// isMQInjectsEnabled if set to true will cause some edge-inspired features to work:
// MQ2Spawns, MQ2Maps, MQ2Commands, MQ2Windows, MQ2Pulse, MQ2Spawns, MapPlugin, MQ2ItemDisplay, MQ2Labels
bool isMQInjectsEnabled = false;

// isMapWindowDisabled if set to true will do a soft patch to disable the map in game. If disabled, I suggest isMQInjectsEnabled being false as well, else npc data is still populated
bool isMapWindowDisabled = false;

// areLuclinModelsDisabled if set to true disables the ability for luclin models to load, forcing classic models
bool areLuclinModelsDisabled = false;

// isBazaarWindowDisabled if set to true will disable the bazaar window in game by doing a soft patch.
bool isBazaarWindowDisabled = false;

// isHeroicDisabled if set to true will make heroic stats not display
bool isHeroicDisabled = false;

// isMaxHPFixEnabled if set to true allows hp beyond 10 million, this is a rare situation for custom servers
bool isMaxHPFixEnabled = false;

// isPatchmeDisabled if set to true will let you double click eqgame.exe and not get the "Please run EverQuest" message, will start properly
bool isPatchmeDisabled = false;

// isFoodDrinkSpamDisabled if set to true will stop you are hungry/thirsty messages to display on client. If server side isn't disabled, you can still get negative effects.
bool isFoodDrinkSpamDisabled = false;

// isMQ2PreventionEnabled if set to true will do basic prevention of mq2 by randomizing the version string, primitive anticheat
bool isMQ2PreventionEnabled = false;

// isSpellDataCRCEnabled if set to true will send spell data to the server as a CRC check, needs a server side modification not yet supported by eqemu master
bool isSpellDataCRCEnabled = false;

// isCombatDamageDoubleAppliedFixEnabled if set to true fixes a bug in rof2 where combat damage applied to client state is applied twice.
// it is the main cause of players falling unconsious while the server still thinks they're alive
// also can help with bouncing healthbar issues
bool isCombatDamageDoubleAppliedFixEnabled = false;

// isChecksumFixEnabled if set to true will override the normal checksum logic, if your server is not supporting checksums, can be left false
bool isChecksumFixEnabled = false;

// isReportHardwareAddressEnabled if set to true will inspect mac addresses and send a more informative context of where EQ is running. 
// This requires custom server side code that is not in eqemu master branch, and in majority of cases can be left false
bool isReportHardwareAddressEnabled = false;

// ***** NPC *******

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



// ***** ZONE *******

// areCustomZonesEnabled if set to true will allow custom zones defined in Zones[] to be injected in game
bool areCustomZonesEnabled = false;

static ZoneEntry Zones[] = {
    // zoneType, zoneID, zoneShortName, zoneLongName, eqStrID, zoneFlags2, x, y, z
    ZoneEntry(0, 787, "hollows", "Darkened Hollows", 35154, 4, 0, 0, 0),
};
#endif