// Colisión del mundo: sólidos con caras poligonales (Solid.cs) y las consultas de World.cs.
#pragma once
#include <3ds.h>
#include <stdio.h>
#include <vector>
#include "math3d.h"

struct CollisionFace
{
	Vec3 normal;
	float d;            // System.Numerics.Plane: dot(normal, p) + d = 0
	u32 vstart, vcount;
};

struct CollisionSolid
{
	Vec3 min, max;          // límites locales (sumar offset para el mundo)
	u32 fstart, fcount;     // caras con vértices locales
	int actor = -1;         // -1 escenario fijo; si no, índice del sólido dinámico (Block)
	bool collidable = true;
	bool transparent = false;
	Vec3 offset;            // posición del actor (0 para el escenario)
};

struct RayHit
{
	Vec3 point, normal;
	float distance = 0;
	int solid = -1;
	int intersections = 0;
};

struct WallHit
{
	Vec3 pushout, normal, point;
	int solid = -1;
};

enum class VolumeKind : u32 { Death = 0, Spike = 1, FixedCamera = 2, Pushout = 3, Ending = 4 };

struct Volume
{
	VolumeKind kind;
	Vec3 min, max;
	int attach = -1;     // sólido dinámico al que va pegado (Attacher)
	Vec3 extra;          // pinchos: dirección; cámara fija: punto; empuje: (radio, altura, 0) con min = max = posición
	Vec3 Center() const { return (min + max) * 0.5f; }
	bool Contains(Vec3 p, Vec3 shift = Vec3()) const
	{
		p -= shift;
		return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
	}
};

namespace World
{
	constexpr float DeathPlane = -100;

	extern std::vector<CollisionSolid> solids;
	extern std::vector<Vec3> verts;
	extern std::vector<CollisionFace> faces;
	extern std::vector<Volume> volumes;
	// por sólido dinámico: índice de su CollisionSolid, desplazamiento desde el inicio y si sigue vivo
	extern std::vector<int> actorSolid;
	extern std::vector<Vec3> actorDisplacement;
	extern std::vector<bool> actorAlive;
	Vec3 VolumeShift(const Volume& v);

	// lee la sección de colisión del .lvl (el archivo ya está posicionado ahí)
	bool LoadCollision(FILE* f);

	bool SolidRayCast(Vec3 point, Vec3 direction, float distance, RayHit& hit,
		bool ignoreBackfaces = true, bool ignoreTransparent = false);

	// hasta 8 resultados, como StackList8<WallHit>
	int SolidWallCheck(Vec3 point, float radius, WallHit out[8]);
	bool SolidWallCheckNearest(Vec3 point, float radius, WallHit& hit);
	bool SolidWallCheckClosestToNormal(Vec3 point, float radius, Vec3 normal, WallHit& hit);

	// primer volumen del tipo que contiene el punto (o nullptr)
	const Volume* OverlapsVolume(VolumeKind kind, Vec3 point, bool (*predicate)(const Volume&, void*) = nullptr, void* user = nullptr);

	// estadísticas para medir el costo de las consultas
	extern u32 statRayCasts, statWallChecks, statFacesTested;
}
