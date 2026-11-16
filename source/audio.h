// Audio (equivale a Source/Audio de FMOD): música en streaming y efectos con posición.
// Los archivos salen de tools/audio.py: romfs:/audio/{music,sfx}/*.ogg y romfs:/audio/events.txt.
#pragma once
#include "math3d.h"

namespace Audio
{
	bool Init();                 // false si el DSP no arrancó (falta sdmc:/3ds/dspfirm.cdc)
	void Exit();
	bool Available();

	// efectos: devuelve un identificador (0 = no sonó). position != nullptr => sonido 3D
	int Play(const char* event, const Vec3* position = nullptr);
	inline int Play(const char* event, Vec3 position) { return Play(event, &position); }
	// voz de diálogo ("granny/granny1_01_g1")
	int PlayVoice(const char* voice);
	// empieza a decodificar una voz por adelantado (al comenzar un diálogo)
	void PreloadVoice(const char* voice);
	void Stop(int handle);
	bool IsPlaying(int handle);
	void SetPosition(int handle, Vec3 position);
	void StopAll();

	// la cámara (Audio.SetListener(Camera))
	void SetListener(Vec3 position, Vec3 forward);
	// recalcula volumen y paneo de los sonidos 3D; una vez por cuadro
	void Update();

	void PlayMusic(const char* name);     // "mus_lvl1"...
	void StopMusic();
	void SetMusicVolume(float v);
	void SetSfxVolume(float v);

	const char* Status();                // texto para la pantalla de abajo
}
