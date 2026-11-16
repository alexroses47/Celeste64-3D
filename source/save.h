// Save.cs: opciones y récord del nivel, en sdmc:/3ds/celeste64/save.txt (texto clave=valor).
#pragma once
#include <map>
#include <set>
#include <string>

struct LevelRecord
{
	std::string id = "0_FORSAKEN_CITY";
	int checkpoint = 0;                       // índice del checkpoint (0 = Start)
	std::set<std::string> strawberries;       // ids "mapa/n" (como el original: "{map}/{contador}")
	std::set<std::string> completedSubMaps;
	std::map<std::string, int> flags;         // GRANNY, THEO, BADELINE...
	int deaths = 0;
	double time = 0;                          // segundos

	int GetFlag(const std::string& name) const { auto it = flags.find(name); return it == flags.end() ? 0 : it->second; }
	void IncFlag(const std::string& name) { flags[name] = GetFlag(name) + 1; }
};

struct SaveData
{
	bool zGuide = true;
	bool speedrunTimer = false;
	int musicVolume = 10;          // 0..10
	int sfxVolume = 10;
	int invertCamera = 0;          // 0 ninguno, 1 X, 2 Y, 3 ambos
	bool fps30 = false;            // limitar a 30 cuadros por segundo
	LevelRecord record;

	bool Load();
	bool Write() const;
	void EraseRecord() { record = LevelRecord(); }
};

extern SaveData Save;
