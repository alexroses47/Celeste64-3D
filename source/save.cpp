#include "save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void logf_(const char* fmt, ...);

SaveData Save;

static const char* SavePath = "sdmc:/3ds/celeste64/save.txt";

bool SaveData::Load()
{
	FILE* f = fopen(SavePath, "r");
	if (!f) return false;
	char line[256];
	while (fgets(line, sizeof(line), f))
	{
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		std::string key = line, value = eq + 1;
		while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
		int iv = atoi(value.c_str());
		if (key == "zguide") zGuide = iv != 0;
		else if (key == "timer") speedrunTimer = iv != 0;
		else if (key == "music") musicVolume = iv;
		else if (key == "sfx") sfxVolume = iv;
		else if (key == "invert") invertCamera = iv;
		else if (key == "fps30") fps30 = iv != 0;
		else if (key == "level") record.id = value;
		else if (key == "checkpoint") record.checkpoint = iv;
		else if (key == "strawb") record.strawberries.insert(value.find('/') == std::string::npos ? "1/" + value : value);
		else if (key == "submap") record.completedSubMaps.insert(value);
		else if (key == "deaths") record.deaths = iv;
		else if (key == "time") record.time = atof(value.c_str());
		else if (key == "flag")
		{
			size_t colon = value.find(':');
			if (colon != std::string::npos)
				record.flags[value.substr(0, colon)] = atoi(value.c_str() + colon + 1);
		}
	}
	fclose(f);
	logf_("partida cargada: checkpoint %d, %d fresas, %d muertes\n", record.checkpoint, (int)record.strawberries.size(), record.deaths);
	return true;
}

bool SaveData::Write() const
{
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/celeste64", 0777);
	FILE* f = fopen(SavePath, "w");
	if (!f) { logf_("no se pudo guardar la partida\n"); return false; }
	fprintf(f, "zguide=%d\ntimer=%d\nmusic=%d\nsfx=%d\ninvert=%d\nfps30=%d\n", zGuide, speedrunTimer, musicVolume, sfxVolume, invertCamera, fps30);
	fprintf(f, "level=%s\ncheckpoint=%d\ndeaths=%d\ntime=%.3f\n", record.id.c_str(), record.checkpoint, record.deaths, record.time);
	for (auto& s : record.strawberries) fprintf(f, "strawb=%s\n", s.c_str());
	for (auto& s : record.completedSubMaps) fprintf(f, "submap=%s\n", s.c_str());
	for (auto& kv : record.flags) fprintf(f, "flag=%s:%d\n", kv.first.c_str(), kv.second);
	fclose(f);
	return true;
}
