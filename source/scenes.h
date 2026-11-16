// Pantalla de título (Titlescreen.cs) y mapa con las postales (Overworld.cs).
#pragma once
#include "math3d.h"

namespace Title
{
	enum Result { None, ToOverworld, Exit };
	void Enter();
	Result Update(float dt);
	void Render(float iod);     // 3D + 2D de un ojo
}

namespace Overworld
{
	enum Result { None, ToTitle, EnterLevel };
	void Enter(bool startOnLastSelected);
	Result Update(float dt);
	bool RestartRequested();    // la entrada eligió "Restart" (borrar el récord)
	void PrepareTextures();     // dibuja la postal en su textura (una vez por cuadro, antes de los ojos)
	void Render(float iod);
}

// partes compartidas
namespace UI
{
	void Overlay(double elapsed);          // capa en movimiento (aditiva) + viñeta
	void Prompts(const char* confirm, const char* cancel);
	void Version();
}
