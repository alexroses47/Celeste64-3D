// Personajes (NPC.cs, Granny.cs, Theo.cs, Badeline.cs, Signpost.cs) y cinemáticas (Cutscene.cs).
#pragma once
#include <string>
#include <vector>
#include "actors.h"
#include "math3d.h"
#include "model.h"

struct Npc
{
	enum Type { Granny = 6, Theo = 7, Badeline = 8, Sign = 9 };
	int type = 0;
	Vec3 position;
	Vec2 facing = Vec2(0, -1);
	std::string dialog;            // letreros: clave del diálogo
	SkinnedModel* model = nullptr;
	Mat4 transform = Mat4::Identity();
	Hair hair;
	bool hasHair = false;

	bool interactEnabled = true;
	float interactRadius = 16;
	Vec3 interactHoverOffset;
	bool isPlayerOver = false, showHover = false;
	float showTimer = 0;
	float pushoutRadius = 8, pushoutHeight = 12;

	Mat4 ActorMatrix() const { return Mat4::Translate(position) * Mat4::RotateZ(facing.Angle() + PI / 2); }
	std::string NextConversation() const;
	void CheckForDialog();
};

namespace Dialog
{
	struct Line { std::string face, voice, text; };
	bool Load();
	const std::vector<Line>* Lines(const std::string& key);
}

namespace Cutscenes
{
	extern std::vector<Npc> npcs;

	void InitNpcs();                 // modelos y parámetros de cada personaje
	bool Active();
	bool FreezeGame();               // la cinemática congela todo lo demás (desbloqueo de fresa)

	void Interact(Npc& npc);
	void StrawberryUnlock(Strawberry& s);

	void Update(float dt);
	void UpdateNpcs(float dt);
	void LateUpdateNpcs();
	void CollectSprites(Vec3 camLeft, Vec3 camUp);
	void DrawNpcs();
	void RenderUI();                 // franjas de cine y cuadro de diálogo (2D)
}
