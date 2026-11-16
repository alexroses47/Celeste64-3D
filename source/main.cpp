// Celeste 64 para 3DS — prueba 5: Madeline jugable, fresas, sombra, piedras flotantes y transición al morir.
//
// Controles (como el original con mando):
//   Circle Pad  mover            D-pad  cámara (girar / acercar)
//   B / X       saltar           Y / A  dash
//   L / R       escalar
//   SELECT      captura a la SD (sdmc:/c64_shot_N.bmp)
//   START       salir
//   Pantalla táctil: arriba = cámara libre, abajo = medición
//
// Modo automático (emulador): si existe sdmc:/c64_auto.txt, ejecuta sus líneas en orden:
//   wait N             N cuadros sin tocar nada
//   hold TECLAS N      mantiene TECLAS N cuadros (A+B+X+Y+L+R, UP/DOWN/LEFT/RIGHT = Circle Pad, DUP.. = D-pad)
//   shot               captura
//   log                registra estado, posición y velocidad de Madeline
//   free x y z yaw pitch / game   cámara libre en esa pose / volver al juego
//   bench              medición por checkpoints
//   tp x y z           teletransporta a Madeline (pruebas)
//   pause              abre el menú de pausa
// Al terminar escribe "FIN" en sdmc:/c64_log.txt y se cierra.

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <array>
#include <vector>

#include "actors.h"
#include "audio.h"
#include "menu.h"
#include "scenes.h"
#include <functional>
#include "save.h"
#include "cutscene.h"
#include "gfx.h"
#include "sprites.h"
#include "input.h"
#include "math3d.h"
#include "model.h"
#include "player.h"
#include "world.h"

#define CLEAR_COLOR 0x444C83FF   // World.Render: target.Clear(0x444c83)
#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

constexpr float FOV_Y = 45.0f;        // Camera.cs: PI/4 * FOVMultiplier
constexpr float FOCAL = 60.0f;        // distancia de la pantalla (paralaje cero)
constexpr float IOD_MAX = 8.0f;       // separación de ojos con el deslizador al máximo
constexpr int MAX_STEPS = 4;          // pasos lógicos máximos por cuadro
constexpr float GHOST_DEPTH_BIAS = 0.0002f;
constexpr float DETAIL_DISTANCE = 800.0f;   // pinchos y props: por ahora igual al plano lejano (sin recorte visible)

// ---------------------------------------------------------------- registro
static FILE* logFile;

void logf_(const char* fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fputs(buf, stderr);              // llega a la Mac por 3dslink
	if (logFile) { fputs(buf, logFile); fflush(logFile); }
}

// ---------------------------------------------------------------- nivel (.lvl de tools/convert.py)
struct LevelHeader { char magic[4]; u32 version, numTex, numChunks, numParts, numVerts, numIdx; float spawn[3], angle; u32 numCheckpoints; };
struct LvlCheckpoint { float pos[3], angle; };
struct Chunk { float min[3], max[3]; u32 vstart, vcount, pstart, pcount, group; };
struct Part { u32 tex, istart, icount; };
// en el archivo: normal en bytes con signo; al cargar se cambia por la luz del sol ya calculada
struct Vertex { float pos[3], uv[2]; union { s8 nrm[4]; u8 light[4]; }; };

static LevelHeader hdr;
static char skyName[32];
static std::vector<CheckpointInfo> checkpoints;
static C3D_Tex** levelTex;
static Chunk* chunks;
static Part* parts;
// memoria lineal: un bloque por trozo (un solo bloque de 9 MB no cabe cuando la memoria está fragmentada)
static Vertex** chunkVerts;
static u16** chunkIdx;


static std::vector<std::array<C3D_Tex*, 6>> skyLayers;   // un cielo o varias capas ("bsides_0..2")
static Vertex* skyVerts;
static u16* skyIdx;

static char levelMusic[32], levelAmbience[32];

static bool loadLevel(const char* name)
{
	char path[128];
	snprintf(path, sizeof(path), "romfs:/levels/%s.lvl", name);
	FILE* f = fopen(path, "rb");
	if (!f) { logf_("no existe %s\n", path); return false; }
	fread(&hdr, sizeof(hdr), 1, f);
	if (memcmp(hdr.magic, "C64L", 4) != 0 || hdr.version != 9) { logf_("formato .lvl inválido\n"); fclose(f); return false; }
	fread(skyName, 32, 1, f);
	for (u32 i = 0; i < hdr.numCheckpoints; i++)
	{
		LvlCheckpoint c;
		fread(&c, sizeof(c), 1, f);
		checkpoints.push_back({ Vec3(c.pos[0], c.pos[1], c.pos[2]), c.angle });
	}
	u32 floatGroups = 0;
	fread(&floatGroups, 4, 1, f);
	Actors::InitFloating((int)floatGroups);
	float snow[4];
	fread(snow, 4, 4, f);
	Actors::snowAmount = snow[0];
	Actors::snowDirection = Vec3(snow[1], snow[2], snow[3]);
	fread(levelMusic, 32, 1, f);
	fread(levelAmbience, 32, 1, f);

	char (*names)[32] = (char(*)[32])malloc(32 * hdr.numTex);
	fread(names, 32, hdr.numTex, f);
	chunks = (Chunk*)malloc(sizeof(Chunk) * hdr.numChunks);
	fread(chunks, sizeof(Chunk), hdr.numChunks, f);
	parts = (Part*)malloc(sizeof(Part) * hdr.numParts);
	fread(parts, sizeof(Part), hdr.numParts, f);
	long vertsAt = ftell(f);
	long idxAt = vertsAt + (long)sizeof(Vertex) * hdr.numVerts;
	chunkVerts = (Vertex**)calloc(hdr.numChunks, sizeof(Vertex*));
	chunkIdx = (u16**)calloc(hdr.numChunks, sizeof(u16*));
	bool memOk = true;
	const Vec3 sun = Gfx::WorldSun();
	for (u32 c = 0; c < hdr.numChunks && memOk; c++)
	{
		Chunk& ch = chunks[c];
		u32 icount = 0;
		for (u32 p = 0; p < ch.pcount; p++) icount += parts[ch.pstart + p].icount;
		chunkVerts[c] = (Vertex*)linearAlloc(sizeof(Vertex) * (ch.vcount ? ch.vcount : 1));
		chunkIdx[c] = (u16*)linearAlloc(sizeof(u16) * (icount ? icount : 1));
		if (!chunkVerts[c] || !chunkIdx[c]) { memOk = false; break; }
		fseek(f, vertsAt + (long)sizeof(Vertex) * ch.vstart, SEEK_SET);
		fread(chunkVerts[c], sizeof(Vertex), ch.vcount, f);
		// Default.hlsl por vértice, hecho una sola vez: brillo del lado contrario al sol, sombra del lado del sol
		for (u32 k = 0; k < ch.vcount; k++)
		{
			Vertex& vx = chunkVerts[c][k];
			float d = (vx.nrm[0] * sun.x + vx.nrm[1] * sun.y + vx.nrm[2] * sun.z) / 127.0f;
			vx.light[0] = (u8)(d < 0 ? fminf(-d, 1.0f) * 255.0f + 0.5f : 0);
			vx.light[1] = 0;
			vx.light[2] = 0;
			vx.light[3] = (u8)(d > 0 ? fminf(d, 1.0f) * 255.0f + 0.5f : 0);
		}
		// los índices de cada parte quedan seguidos dentro del bloque del trozo
		u32 at = 0;
		for (u32 p = 0; p < ch.pcount; p++)
		{
			Part& part = parts[ch.pstart + p];
			fseek(f, idxAt + 2L * part.istart, SEEK_SET);
			fread(chunkIdx[c] + at, 2, part.icount, f);
			part.istart = at;
			at += part.icount;
		}
	}
	if (!memOk)
	{
		logf_("sin memoria lineal para %s (libre %lu KB)\n", name, linearSpaceFree() / 1024);
		free(names);
		fclose(f);
		return false;
	}
	fseek(f, idxAt + 2L * hdr.numIdx, SEEK_SET);
	bool ok = World::LoadCollision(f);
	// actores: (tipo, posición, ángulo, banderas, sólido al que van pegados, texto)
	u32 numActors = 0;
	fread(&numActors, 4, 1, f);
	for (u32 i = 0; i < numActors; i++)
	{
		u32 type, flags;
		s32 attach;
		float d[4];
		char text[32];
		fread(&type, 4, 1, f);
		fread(d, 4, 4, f);
		fread(&flags, 4, 1, f);
		fread(&attach, 4, 1, f);
		float extra[3];
		fread(extra, 4, 3, f);
		fread(text, 32, 1, f);
		Vec3 pos(d[0], d[1], d[2]);
		// HandleActorCreation: Facing = AngleToVector(angle - 90°)
		Vec2 facing = Calc::AngleToVector(d[3] * DEG2RAD - PI / 2);
		if (type == 1)
		{
			Strawberry st;
			st.id = (int)Actors::strawberries.size();
			st.sid = std::string(name) + "/" + std::to_string(st.id);   // "{map}/{contador}"
			st.position = pos;
			st.hasBubble = (flags & 4) != 0;
			st.bubbleTo = Vec3(extra[0], extra[1], extra[2]);
			st.locked = (flags & 1) != 0;
			st.playUnlockSound = (flags & 2) == 0;
			memcpy(st.unlockGroup, text, 32);
			Actors::strawberries.push_back(st);
		}
		else if (type == 2)
		{
			Spring s;
			s.base = s.position = pos;
			s.facing = facing;
			s.attach = attach;
			Actors::springs.push_back(s);
		}
		else if (type == 3)
		{
			Refill r;
			r.position = pos;
			r.isDouble = (flags & 1) != 0;
			Actors::refills.push_back(r);
		}
		else if (type == 4)
		{
			Feather fe;
			fe.position = pos;
			Actors::feathers.push_back(fe);
		}
		else if (type == 10)
		{
			Cassette c;
			c.position = pos;
			c.map = std::string(text, strnlen(text, 32));
			Actors::cassettes.push_back(c);
		}
		else if (type == 11)
		{
			Coin c;
			c.position = pos;
			Actors::coins.push_back(c);
		}
		else if (type >= 6 && type <= 9)
		{
			Npc n;
			n.type = (int)type;
			n.position = pos;
			n.facing = facing;
			n.dialog = std::string(text, strnlen(text, 32));
			Cutscenes::npcs.push_back(n);
		}
	}
	// sólidos dinámicos: (tipo, centro, destino, banderas, grupo)
	u32 numBlocks = 0;
	fread(&numBlocks, 4, 1, f);
	for (u32 i = 0; i < numBlocks; i++)
	{
		Block b;
		float c[3], e[3];
		fread(&b.kind, 4, 1, f);
		fread(c, 4, 3, f);
		fread(e, 4, 3, f);
		fread(&b.flags, 4, 1, f);
		fread(b.group, 32, 1, f);
		b.index = (int)i;
		b.start = b.position = Vec3(c[0], c[1], c[2]);
		b.end = Vec3(e[0], e[1], e[2]);
		Actors::blocks.push_back(b);
	}
	fclose(f);

	// banderas de checkpoint: una por cada PlayerSpawn con nombre (todas menos "Start")
	for (size_t i = 1; i < checkpoints.size(); i++)
	{
		CheckpointFlag fl;
		fl.checkpoint = (int)i;
		fl.position = checkpoints[i].position;
		Actors::flags.push_back(fl);
	}
	if (!ok) { logf_("falta la colisión en el nivel\n"); return false; }

	levelTex = (C3D_Tex**)calloc(hdr.numTex, sizeof(C3D_Tex*));
	for (u32 i = 0; i < hdr.numTex; i++)
		levelTex[i] = Gfx::GetTexture(names[i]);
	free(names);

	logf_("nivel %s: %lu trozos, %lu partes, %lu vertices, %lu indices, %lu texturas, %lu checkpoints, skybox %s\n",
		name, hdr.numChunks, hdr.numParts, hdr.numVerts, hdr.numIdx, hdr.numTex, hdr.numCheckpoints, skyName);
	return true;
}

// libera el mapa cargado (las texturas y los modelos quedan en caché para el siguiente)
static void unloadLevel()
{
	free(chunks); chunks = nullptr;
	free(parts); parts = nullptr;
	if (chunkVerts)
		for (u32 c = 0; c < hdr.numChunks; c++)
		{
			if (chunkVerts[c]) linearFree(chunkVerts[c]);
			if (chunkIdx[c]) linearFree(chunkIdx[c]);
		}
	free(chunkVerts); chunkVerts = nullptr;
	free(chunkIdx); chunkIdx = nullptr;
	free(levelTex); levelTex = nullptr;
	checkpoints.clear();
	World::solids.clear();
	World::verts.clear();
	World::faces.clear();
	World::volumes.clear();
	Actors::Clear();
	for (auto& n : Cutscenes::npcs) delete n.model;
	Cutscenes::npcs.clear();
	memset(&hdr, 0, sizeof(hdr));
}

// Skybox.cs: cubo de -1..1, cada cara con su textura
static void initSkybox()
{
	static const char* faceNames[6] = { "u", "d", "n", "s", "e", "w" };
	static const float P[8][3] = {
		{-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
		{-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
	};
	// esquinas de la subtextura (Foster: 0=arriba-izq, 1=arriba-der, 2=abajo-der, 3=abajo-izq), V invertida
	static const float T[4][2] = { {0, 1}, {1, 1}, {1, 0}, {0, 0} };
	static const int faceVerts[6][4] = {
		{0, 1, 2, 3}, {7, 6, 5, 4}, {4, 5, 1, 0}, {6, 7, 3, 2}, {0, 3, 7, 4}, {5, 6, 2, 1},
	};
	static const int faceUVs[6][4] = {
		{3, 2, 1, 0}, {3, 2, 1, 0}, {2, 3, 0, 1}, {2, 3, 0, 1}, {0, 1, 2, 3}, {2, 3, 0, 1},
	};
	skyVerts = (Vertex*)linearAlloc(sizeof(Vertex) * 24);
	skyIdx = (u16*)linearAlloc(sizeof(u16) * 36);
	for (int f = 0; f < 6; f++)
	{
		for (int k = 0; k < 4; k++)
		{
			Vertex* v = &skyVerts[f * 4 + k];
			memcpy(v->pos, P[faceVerts[f][k]], sizeof(v->pos));
			v->uv[0] = T[faceUVs[f][k]][0];
			v->uv[1] = T[faceUVs[f][k]][1];
			memset(v->nrm, 0, sizeof(v->nrm));
		}
		u16 b = f * 4;
		u16 tri[6] = { b, (u16)(b + 1), (u16)(b + 2), b, (u16)(b + 2), (u16)(b + 3) };
		memcpy(&skyIdx[f * 6], tri, sizeof(tri));

	}
	(void)faceNames;
}

static bool texExists(const std::string& key)
{
	FILE* f = fopen(("romfs:/tex/" + key + ".t3x").c_str(), "rb");
	if (f) fclose(f);
	return f != nullptr;
}

// World(): un cielo "skyboxes/<nombre>" o un grupo "skyboxes/<nombre>_0.._n"
static void loadSkyTextures()
{
	static const char* faceNames[6] = { "u", "d", "n", "s", "e", "w" };
	skyLayers.clear();
	auto addLayer = [](const std::string& base)
	{
		std::array<C3D_Tex*, 6> faces;
		for (int f = 0; f < 6; f++)
			faces[f] = Gfx::GetTexture((base + "_" + faceNames[f]).c_str(), false);
		skyLayers.push_back(faces);
	};
	std::string base = std::string("sky_") + skyName;
	if (texExists(base + "_u"))
		addLayer(base);
	else
		for (int i = 0; texExists(base + "_" + std::to_string(i) + "_u"); i++)
			addLayer(base + "_" + std::to_string(i));
}

// ---------------------------------------------------------------- cámara de dibujo
struct ViewPose { Vec3 eye, target; float fovY; };

static void viewMatrix(C3D_Mtx* out, const ViewPose& v)
{
	Mtx_LookAt(out, FVec3_New(v.eye.x, v.eye.y, v.eye.z), FVec3_New(v.target.x, v.target.y, v.target.z),
		FVec3_New(0, 0, 1), false);
}

// cámara libre / de medición: posición + ángulos
struct FreeCam { Vec3 pos; float yaw, pitch; };

static Vec3 freeForward(const FreeCam& c)
{
	return Vec3(sinf(c.yaw) * cosf(c.pitch), cosf(c.yaw) * cosf(c.pitch), sinf(c.pitch));
}

static void freeLookAt(FreeCam& c, Vec3 target)
{
	Vec3 d = target - c.pos;
	c.yaw = atan2f(d.x, d.y);
	c.pitch = atan2f(d.z, sqrtf(d.x * d.x + d.y * d.y));
}

static ViewPose freePose(const FreeCam& c) { return { c.pos, c.pos + freeForward(c), FOV_Y }; }

// ---------------------------------------------------------------- culling
static float planes[4][4];

static void extractPlanes(const C3D_Mtx* m)
{
	float row[4][4];
	for (int i = 0; i < 4; i++) { row[i][0] = m->r[i].x; row[i][1] = m->r[i].y; row[i][2] = m->r[i].z; row[i][3] = m->r[i].w; }
	for (int k = 0; k < 4; k++)
	{
		planes[0][k] = row[3][k] + row[0][k];
		planes[1][k] = row[3][k] - row[0][k];
		planes[2][k] = row[3][k] + row[1][k];
		planes[3][k] = row[3][k] - row[1][k];
	}
	for (int p = 0; p < 4; p++)
	{
		float l = sqrtf(planes[p][0] * planes[p][0] + planes[p][1] * planes[p][1] + planes[p][2] * planes[p][2]);
		for (int k = 0; k < 4; k++) planes[p][k] /= l;
	}
}

static bool boxVisible(const float* mn, const float* mx, Vec3 eye, float margin, float farPlane)
{
	for (int p = 0; p < 4; p++)
	{
		const float* pl = planes[p];
		float x = pl[0] >= 0 ? mx[0] : mn[0];
		float y = pl[1] >= 0 ? mx[1] : mn[1];
		float z = pl[2] >= 0 ? mx[2] : mn[2];
		if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < -margin) return false;
	}
	float dx = fmaxf(fmaxf(mn[0] - eye.x, eye.x - mx[0]), 0);
	float dy = fmaxf(fmaxf(mn[1] - eye.y, eye.y - mx[1]), 0);
	float dz = fmaxf(fmaxf(mn[2] - eye.z, eye.z - mx[2]), 0);
	return dx * dx + dy * dy + dz * dz < farPlane * farPlane;
}

// ---------------------------------------------------------------- dibujo
static u16* visible;
static std::vector<u32> ghostChunks;   // bloques de cassette apagados (30 % de opacidad)
static u32 visibleCount;
static u32 statTris, statCalls;

static ModelData playerData;
static SkinnedModel* playerModel;
static Player player;


static Vec3 viewEye;
static bool sphereVisible(Vec3 c, float r)
{
	for (int p = 0; p < 4; p++)
		if (planes[p][0] * c.x + planes[p][1] * c.y + planes[p][2] * c.z + planes[p][3] < -r - IOD_MAX)
			return false;
	float far = Game::Camera.FarPlane + r;
	return (c - viewEye).LengthSquared() < far * far;
}

static void cullChunks(const ViewPose& v, float iod)
{
	C3D_Mtx proj, view, vp;
	Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(v.fovY), C3D_AspectRatioTop, Game::Camera.NearPlane, Game::Camera.FarPlane, false);
	viewMatrix(&view, v);
	Mtx_Multiply(&vp, &proj, &view);
	extractPlanes(&vp);
	viewEye = v.eye;
	visibleCount = 0;
	ghostChunks.clear();
	for (u32 c = 0; c < hdr.numChunks; c++)
	{
		const Chunk& ch = chunks[c];
		if (ch.group >= 0x10000)
		{
			// sólido dinámico: límites locales + posición actual
			const Block& b = Actors::blocks[ch.group - 0x10000];
			if (!b.visible) continue;
			// bloque de cassette apagado: va en la pasada transparente
			if (b.kind == Block::Cassette && !World::solids[World::actorSolid[b.index]].collidable)
			{
				ghostChunks.push_back(c);
				continue;
			}
			float mn[3] = { ch.min[0] + b.position.x, ch.min[1] + b.position.y, ch.min[2] + b.position.z };
			float mx[3] = { ch.max[0] + b.position.x, ch.max[1] + b.position.y, ch.max[2] + b.position.z };
			if (boxVisible(mn, mx, v.eye, iod + 2.0f, Game::Camera.FarPlane))
				visible[visibleCount++] = c;
			continue;
		}
		if (ch.group >= 0x8000 && ch.group <= 0x8006)
		{
			// detalle (pinchos, props): a más de DETAIL_DISTANCE mide un par de píxeles; no se dibuja
			float dx = fmaxf(fmaxf(ch.min[0] - v.eye.x, v.eye.x - ch.max[0]), 0);
			float dy = fmaxf(fmaxf(ch.min[1] - v.eye.y, v.eye.y - ch.max[1]), 0);
			float dz = fmaxf(fmaxf(ch.min[2] - v.eye.z, v.eye.z - ch.max[2]), 0);
			if (dx * dx + dy * dy + dz * dz > DETAIL_DISTANCE * DETAIL_DISTANCE) continue;
		}
		float margin = iod + 1.0f + ((ch.group > 0 && ch.group < 0x8000) ? 12.0f : 0.0f);
		if (boxVisible(ch.min, ch.max, v.eye, margin, Game::Camera.FarPlane))
			visible[visibleCount++] = c;
	}
}

static void renderEye(const ViewPose& v, float iod)
{
	Mtx_PerspStereoTilt(&Gfx::projection, C3D_AngleFromDegrees(v.fovY), C3D_AspectRatioTop,
		Game::Camera.NearPlane, Game::Camera.FarPlane, iod, FOCAL, false);
	viewMatrix(&Gfx::view, v);

	// skybox (World.Render: escala 300, z * 0.5, centrado en la cámara, sin profundidad)
	Gfx::BindScene();
	{
		Gfx::SetUnlitTexEnv();
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		C3D_CullFace(GPU_CULL_NONE);
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
		C3D_BufInfo* buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, skyVerts, sizeof(Vertex), 3, 0x210);
		for (size_t layer = 0; layer < skyLayers.size(); layer++)
		{
			// RotZ(i * GeneralTimer * 0.01) * Scale(1, 1, 0.5) * Translate(cámara), con tamaño 300
			Mat4 m = Mat4::Translate(v.eye) * Mat4::Scale(Vec3(1, 1, 0.5f)) *
				Mat4::RotateZ(layer * Actors::GeneralTimer * 0.01f) * Mat4::Scale(300.0f);
			C3D_Mtx model, mv;
			Gfx::ToC3D(m, &model);
			Mtx_Multiply(&mv, &Gfx::view, &model);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, Gfx::scene.modelView, &mv);
			for (int f = 0; f < 6; f++)
			{
				if (!skyLayers[layer][f]) continue;
				C3D_TexBind(0, skyLayers[layer][f]);
				C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, &skyIdx[f * 6]);
			}
		}
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_AlphaTest(true, GPU_GREATER, 0x7F);
	}

	// escenario
	Gfx::BindLevel();
	Gfx::SetLevelTransform(Gfx::view, 0);
	Gfx::SetLitTexEnv();
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_BACK_CCW);

	C3D_Tex* bound = nullptr;
	u32 currentGroup = 0;
	for (u32 i = 0; i < visibleCount; i++)
	{
		const Chunk* ch = &chunks[visible[i]];
		if (ch->group != currentGroup)
		{
			currentGroup = ch->group;
			C3D_Mtx mv = Gfx::view;
			if (currentGroup >= 0x10000)
			{
				// sólido dinámico: su posición más el temblor (Solid.TShake)
				Vec3 o = Actors::blocks[currentGroup - 0x10000].RenderOffset();
				Mtx_Translate(&mv, o.x, o.y, o.z, true);
				Gfx::SetLevelTransform(mv, o.z);
			}
			else
			{
				// FloatingDecoration: Model.Transform = Translation(0, 0, sin(t) * 12)
				float z = Actors::FloatingOffset((int)currentGroup);
				Mtx_Translate(&mv, 0, 0, z, true);
				Gfx::SetLevelTransform(mv, z);
			}
		}
		C3D_BufInfo* buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, chunkVerts[ch - chunks], sizeof(Vertex), 3, 0x210);
		for (u32 p = 0; p < ch->pcount; p++)
		{
			const Part* part = &parts[ch->pstart + p];
			C3D_Tex* tex = levelTex[part->tex];
			if (!tex) continue;
			if (tex != bound) { C3D_TexBind(0, tex); bound = tex; }
			C3D_DrawElements(GPU_TRIANGLES, part->icount, C3D_UNSIGNED_SHORT, chunkIdx[ch - chunks] + part->istart);
			statTris += part->icount / 3;
			statCalls++;
		}
	}

	// Madeline y fresas (las que se están agarrando van en el efecto de abajo)
	bool strawbEffect = player.InStrawbGet();
	// silueta (ModelFlags.Silhouette): las partes tapadas de Madeline, del color de su pelo
	if (playerModel && player.DrawModel && !strawbEffect)
	{
		u32 c = player.HairColor();
		u32 abgr = 0xFF000000 | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
		C3D_DepthTest(true, GPU_LESS, GPU_WRITE_COLOR);   // DepthCompare.Greater del original (aquí va al revés)
		playerModel->DrawTinted(player.ModelMatrix(), abgr, Sprites::White());
		if (player.DrawHair)
		{
			Gfx::BindScene();
			player.HairModel.Draw();
		}
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
	}
	if (playerModel && player.DrawModel && !strawbEffect)
	{
		playerModel->Draw(player.ModelMatrix());
		statTris += playerData.numIdx / 3;
		statCalls += playerData.parts.size();
	}
	if (player.DrawHair && !strawbEffect)
	{
		Gfx::BindScene();
		player.HairModel.Draw();
	}
	Actors::DrawModels(false);
	Cutscenes::DrawNpcs();

	// sprites 3D: sombras, guía de caída, halos
	Sprites::Flush3D();

	// modelos transparentes: estelas del dash y fresas ya guardadas (sin escribir profundidad)
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
	C3D_AlphaTest(false, GPU_ALWAYS, 0);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
	Actors::DrawGhosts(Sprites::White());
	player.DrawTrails(Sprites::White());
	if (!ghostChunks.empty())
	{
		// CassetteBlock.SetOn(false): Color.White * 0.30 (premultiplicado)
		Gfx::BindLevel();
		Gfx::SetLitTexEnv(0x4D4D4D4D);
		// los bloques de cassette de dos colores comparten caras: el apagado va un pelo más atrás
		// para que no pelee con el encendido (rayas que titilan)
		C3D_DepthMap(true, -1.0f, -GHOST_DEPTH_BIAS);
		for (u32 c : ghostChunks)
		{
			const Chunk* ch = &chunks[c];
			Vec3 o = Actors::blocks[ch->group - 0x10000].RenderOffset();
			C3D_Mtx mv = Gfx::view;
			Mtx_Translate(&mv, o.x, o.y, o.z, true);
			Gfx::SetLevelTransform(mv, o.z);
			C3D_BufInfo* buf = C3D_GetBufInfo();
			BufInfo_Init(buf);
			BufInfo_Add(buf, chunkVerts[c], sizeof(Vertex), 3, 0x210);
			for (u32 p = 0; p < ch->pcount; p++)
			{
				const Part* part = &parts[ch->pstart + p];
				if (!levelTex[part->tex]) continue;
				C3D_TexBind(0, levelTex[part->tex]);
				C3D_DrawElements(GPU_TRIANGLES, part->icount, C3D_UNSIGNED_SHORT, chunkIdx[c] + part->istart);
			}
		}
		C3D_DepthMap(true, -1.0f, 0.0f);
	}
	C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
	C3D_AlphaTest(true, GPU_GREATER, 0x7F);
	C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);

	// World.Render: efecto al agarrar una fresa
	if (strawbEffect)
	{
		static C3D_Tex* splash = Gfx::GetTexture("spr_splash", false);
		Sprites::Begin2D();
		// fondo negro al 90 % que además deja la profundidad en "lejos" (equivale a limpiarla)
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
		C3D_DepthMap(true, 0.0f, 0.0f);
		Sprites::Rect2D(0, 0, 400, 240, Color4(0, 0, 0, 1) * 0.90f);
		Sprites::Flush2D();
		C3D_DepthMap(true, -1.0f, 0.0f);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		// la imagen de 256 px a escala 240/360
		float sz = 256.0f * 240.0f / 360.0f;
		Sprites::Image2D(splash, 200 - sz / 2, 120 - sz / 2, sz, sz, Color4());
		Sprites::End2D();
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		C3D_CullFace(GPU_CULL_BACK_CCW);
		if (playerModel)
			playerModel->Draw(player.ModelMatrix());
		Gfx::BindScene();
		player.HairModel.Draw();
		Actors::DrawModels(true);
	}
}

// ---------------------------------------------------------------- interfaz 2D (contador de fresas y transición)
static int returnCheckpoint = 0;     // checkpoint del nivel principal al entrar a un nivel extra
static GameCamera returnCamera;       // la cámara del nivel principal (el original no lo descarga)
static bool inEnding = false;        // World.IsInEndingArea
static double musicTime = 0;         // para los pulsos de la música (bloques de cassette)
static int lastBeat = -1;

static float strawbCounterEase, strawbCounterCooldown, strawbCounterWiggle;
static int strawbCounterWas;

static void updateHud(float dt)
{
	// World.Update: contador de fresas
	if (strawbCounterWas != Actors::collectedCount)
	{
		strawbCounterCooldown = 4.0f;
		strawbCounterWiggle = 1.0f;
		strawbCounterWas = Actors::collectedCount;
	}
	else
		Calc::ApproachRef(strawbCounterWiggle, 0, dt / .6f);

	if (player.InStrawbGet())
		strawbCounterCooldown = 2.0f;
	else
		strawbCounterCooldown -= dt;

	if (strawbCounterCooldown > 0 || player.InStrawbGet() || inEnding)
		strawbCounterEase = Calc::Approach(strawbCounterEase, 1, dt * 6.0f);
	else
		strawbCounterEase = Calc::Approach(strawbCounterEase, 0, dt * 6.0f);
}

// ---------------------------------------------------------------- transiciones (Game.cs + Screenwipes)
enum Wipe { WipeAngled, WipeSlide, WipeSpotlight };
struct TransitionState
{
	int step = 0;               // 0 nada, 1 hacia negro, 2 en negro, 3 desde negro
	float percent = 0, hold = 0;
	Wipe to = WipeAngled, from = WipeAngled;
	std::function<void()> perform;
};
static TransitionState trans;

static float wipeDuration(Wipe w) { return w == WipeAngled ? 0.50f : (w == WipeSlide ? 0.20f : 1.2f); }
static bool inTransition() { return trans.step != 0; }

static void startTransition(Wipe to, Wipe from, float hold, std::function<void()> perform)
{
	if (trans.step) return;
	trans.step = 1;
	trans.percent = 0;
	trans.hold = hold;
	trans.to = to;
	trans.from = from;
	trans.perform = perform;
	if (to == WipeSpotlight) Audio::Play("ui_spotlight_out");
}

static void transitionUpdate(float dt)
{
	if (!trans.step) return;
	if (trans.step == 1)
	{
		trans.percent = Calc::Approach(trans.percent, 1.0f, dt / wipeDuration(trans.to));
		if (trans.percent >= 1) trans.step = 2;
	}
	else if (trans.step == 2)
	{
		trans.hold -= dt;
		if (trans.hold <= 0)
		{
			if (trans.perform) trans.perform();
			trans.step = 3;
			trans.percent = 0;
			if (trans.from == WipeSpotlight) Audio::Play("ui_spotlight_in");
		}
	}
	else
	{
		trans.percent = Calc::Approach(trans.percent, 1.0f, dt / wipeDuration(trans.from));
		if (trans.percent >= 1) trans.step = 0;
	}
}

static void drawTransition()
{
	if (!trans.step) return;
	const bool fromBlack = trans.step == 3;
	const Wipe wipe = fromBlack ? trans.from : trans.to;
	const float percent = trans.step == 2 ? 1.0f : trans.percent;
	const float W = 640, H = 360;            // el original dibuja a 640x360
	const float sx = 400.0f / W, sy = 240.0f / H;
	const Color4 black(0, 0, 0, 1);
	if ((percent <= 0 && fromBlack) || (percent >= 1 && !fromBlack))
	{
		Sprites::Rect2D(0, 0, 400, 240, black);
		return;
	}
	auto tri = [&](Vec2 a, Vec2 b, Vec2 c)
	{
		Sprites::Triangle2D(Vec2(a.x * sx, a.y * sy), Vec2(b.x * sx, b.y * sy), Vec2(c.x * sx, c.y * sy), black);
	};
	if (wipe == WipeAngled)
	{
		const int Rows = 12;
		const float AngleSize = 64;
		float rowHeight = (H + 20) / Rows;
		float left = -AngleSize, width = W + AngleSize;
		for (int i = 0; i < Rows; i++)
		{
			float x = left, y = -10 + i * rowHeight, e = 0;
			float across = i / (float)Rows;
			float delay = (fromBlack ? 1 - across : across) * 0.3f;
			if (percent > delay)
				e = fminf(1.0f, (percent - delay) / 0.7f);
			if (fromBlack)
				e = 1 - e;
			float w = width * e;
			Vec2 t[6] = {
				{ x, y }, { x + w, y }, { x, y + rowHeight },
				{ x + w, y }, { x + w + AngleSize, y + rowHeight }, { x, y + rowHeight },
			};
			if (fromBlack)
				for (auto& p : t) { p.x = W - p.x; p.y = H - p.y; }
			tri(t[0], t[1], t[2]);
			tri(t[3], t[4], t[5]);
		}
	}
	else if (wipe == WipeSlide)
	{
		// SlideWipe: un rectángulo inclinado que cruza la pantalla
		const float shift = 64;
		float rx = fromBlack ? Calc::Lerp(-shift, W, percent) : Calc::Lerp(-W, -shift, percent);
		float rw = W + shift * 2;
		Vec2 a(rx + shift, 0), b(rx + rw, 0), c(rx + rw - shift, H), d(rx, H);
		tri(a, b, c);
		tri(a, c, d);
	}
	else
	{
		// SpotlightWipe: círculo que se abre en el centro
		float ease = fromBlack ? percent : 1 - percent;
		const float openRadius = 96;
		float radius;
		if (ease < 0.3f)
		{
			float t = ease / 0.3f;
			radius = (t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2) * openRadius;
		}
		else if (ease < 0.7f)
			radius = openRadius;
		else
			radius = openRadius + ((ease - 0.7f) / 0.3f) * (W - openRadius);
		Vec2 c(W / 2, H / 2);
		Vec2 last(1, 0);
		for (int i = 0; i < 256; i += 12)
		{
			Vec2 next = Calc::AngleToVector(((i + 12.0f) / 256) * TAU);
			tri(c + last * 5000, c + last * radius, c + next * radius);
			tri(c + last * 5000, c + next * 5000, c + next * radius);
			last = next;
		}
	}
}

enum class SceneId { Title, Overworld, World };
static SceneId scene = SceneId::Title;
static int ambience = 0;

static Menu pauseMenu, optionsMenu;
static bool paused = false;
static bool quitRequested = false;

static void drawIcon(C3D_Tex* icon, const char* label, float x, float y, float size, float scale = 1)
{
	// UI.Icon: ícono con contorno negro y el texto a la derecha
	float s = size * scale;
	float ix = x + (size - s) / 2, iy = y + (size - s) / 2;
	for (int ox = -1; ox <= 1; ox++)
		for (int oy = -1; oy <= 1; oy++)
			if (ox || oy) Sprites::Image2D(icon, ix + ox, iy + oy, s, s, Color4(0, 0, 0, 1));
	Sprites::Image2D(icon, ix, iy, s, s, Color4());
	Sprites::Text2D(label, x + size * 0.7f, y + size / 2 - Sprites::LineHeight() / 2, Color4());
}

// ---------------------------------------------------------------- pantalla de abajo
// El arte de la postal de Forsaken City con el crédito del port (título) o las estadísticas
// de la partida (jugando). Con SELECT apretado muestra los datos de prueba.
static C3D_RenderTarget* targetBottom;
static char debugText[512] = "";

static void drawStat(C3D_Tex* icon, const char* text, float x, float y, float size)
{
	Sprites::Image2D(icon, x, y, size, size, Color4());
	Sprites::Text2D(text, x + size * 0.85f, y + size / 2 - Sprites::LineHeight() * 0.65f, Color4(), true, -1, 1.3f);
}

static void drawBottomScreen(bool inGame)
{
	static C3D_Tex* art = Gfx::GetTexture("ui_postcard", false);
	static C3D_Tex* iconStrawb = Gfx::GetTexture("spr_icon_strawberry", false);
	static C3D_Tex* iconSkull = Gfx::GetTexture("spr_icon_skull", false);
	static C3D_Tex* iconTimer = Gfx::GetTexture("spr_icon_stopwatch", false);
	static float artW = 0, artH = 0;
	if (artW == 0)
	{
		FILE* f = fopen("romfs:/tex/ui_postcard.size", "r");
		int w = 3, h = 2;
		if (f) { fscanf(f, "%d %d", &w, &h); fclose(f); }
		artW = (float)w; artH = (float)h;
	}

	C3D_RenderTargetClear(targetBottom, C3D_CLEAR_ALL, 0x0b090dFF, 0);
	C3D_FrameDrawOn(targetBottom);
	Gfx::MarkUnbound();
	Sprites::Begin2DBottom();

	// la postal cubre toda la pantalla (recortada al centro)
	float scale = fmaxf(320.0f / artW, 240.0f / artH);
	float w = artW * scale, h = artH * scale;
	float u0 = (w - 320) / 2 / w, v0 = (h - 240) / 2 / h;
	if (art)
		Sprites::Image2DUV(art, Vec2(0, 0), Vec2(320, 0), Vec2(320, 240), Vec2(0, 240),
			Vec2(u0, 1 - v0), Vec2(1 - u0, v0), Color4());

	auto centered = [](const char* text, float y, float sc, Color4 color)
	{
		Sprites::Text2D(text, 160 - Sprites::TextWidth(text) * sc / 2, y, color, true, -1, sc);
	};

	if (hidKeysHeld() & KEY_SELECT)
	{
		Sprites::Rect2D(0, 0, 320, 240, Color4(0, 0, 0, 1) * 0.8f);
		Sprites::Text2D(debugText, 8, 8, Color4(), true);
	}
	else if (!inGame)
	{
		Sprites::Rect2D(0, 182, 320, 58, Color4(0, 0, 0, 1) * 0.55f);
		centered("3DS port by alexroses47", 190, 1.3f, Color4());
		centered("Unofficial fan port. Not made or endorsed by the Celeste team.", 216, 0.8f, Color4(0.75f, 0.75f, 0.75f, 1));
	}
	else
	{
		Sprites::Rect2D(0, 0, 320, 240, Color4(0, 0, 0, 1) * 0.35f);
		// nombre del mapa arriba
		char name[48];
		if (Actors::mapName == "1") snprintf(name, sizeof(name), "Forsaken City");
		else snprintf(name, sizeof(name), "Cassette %s", Actors::mapName.c_str());
		Sprites::Rect2D(0, 0, 320, 40, Color4(0, 0, 0, 1) * 0.45f);
		centered(name, 6, 2.0f, Color4());

		// estadísticas de la partida (como en la postal del original)
		char buf[32];
		float size = 30;
		snprintf(buf, sizeof(buf), "x%02d", (int)Save.record.strawberries.size());
		drawStat(iconStrawb, buf, 52, 78, size);
		snprintf(buf, sizeof(buf), "x%03d", Save.record.deaths);
		drawStat(iconSkull, buf, 182, 78, size);
		int ms = (int)(Save.record.time * 1000);
		int hh = ms / 3600000, mm = (ms / 60000) % 60, ss = (ms / 1000) % 60, mil = ms % 1000;
		if (hh > 0) snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%03d", hh, mm, ss, mil);
		else snprintf(buf, sizeof(buf), "%02d:%02d:%03d", mm, ss, mil);
		float tw = size * 0.85f + Sprites::TextWidth(buf) * 1.3f;
		drawStat(iconTimer, buf, 160 - tw / 2, 128, size);

		Sprites::Rect2D(0, 212, 320, 28, Color4(0, 0, 0, 1) * 0.45f);
		centered("START: pause", 218, 1.0f, Color4(0.85f, 0.85f, 0.85f, 1));
	}
	Sprites::End2D();
}

static void drawHud()
{
	static C3D_Tex* icon = Gfx::GetTexture("spr_icon_strawberry", false);
	static C3D_Tex* stopwatch = Gfx::GetTexture("spr_icon_stopwatch", false);
	Sprites::Begin2D();
	const float k = 240.0f / 360.0f;
	float hudY = 8 * k;
	if (Save.speedrunTimer)
	{
		// UI.Timer: mm:ss:mmm (o hh:mm:ss:mmm)
		int ms = (int)(Save.record.time * 1000);
		int h = ms / 3600000, m = (ms / 60000) % 60, sec = (ms / 1000) % 60, mil = ms % 1000;
		char t[32];
		if (h > 0) snprintf(t, sizeof(t), "%02d:%02d:%02d:%03d", h, m, sec, mil);
		else snprintf(t, sizeof(t), "%02d:%02d:%03d", m, sec, mil);
		drawIcon(stopwatch, t, 4 * k, hudY, 30 * k);
		hudY += 30 * k + 4 * k;
	}
	if (strawbCounterEase > 0 || paused)
	{
		// UI.Strawberries en (4, 8) con IconSize 30, todo a escala 240/360
		float size = 30 * k;
		float wiggle = 1 + sinf(strawbCounterWiggle * TAU * 2) * strawbCounterWiggle * .3f;
		float x = 4 * k - 60 * k * (1 - Ease::CubeOut(paused ? 1 : strawbCounterEase));
		float y = hudY;
		float s = size * wiggle;
		float ix = x + (size - s) / 2, iy = y + (size - s) / 2;
		for (int ox = -1; ox <= 1; ox++)
			for (int oy = -1; oy <= 1; oy++)
				if (ox || oy) Sprites::Image2D(icon, ix + ox, iy + oy, s, s, Color4(0, 0, 0, 1));
		Sprites::Image2D(icon, ix, iy, s, s, Color4());
		char label[16];
		snprintf(label, sizeof(label), "x%02d", Actors::collectedCount);
		Sprites::Text2D(label, x + size * 0.7f, y + size / 2 - Sprites::LineHeight() / 2, Color4());
	}
	Cutscenes::RenderUI();
	if (paused)
	{
		Sprites::Rect2D(0, 0, 400, 240, Color4(0, 0, 0, 1) * 0.70f);
		pauseMenu.Render(200, 120);
	}
	drawTransition();
	Sprites::End2D();
}

// ---------------------------------------------------------------- capturas
static int shotCounter;

static void saveScreenshot(C3D_RenderTarget* target)
{
	const int W = target->frameBuf.height;   // 400 arriba, 320 abajo
	u8* buf = (u8*)linearAlloc(240 * W * 3);
	if (!buf) return;
	C3D_SyncDisplayTransfer((u32*)target->frameBuf.colorBuf, GX_BUFFER_DIM(240, W),
		(u32*)buf, GX_BUFFER_DIM(240, W), DISPLAY_TRANSFER_FLAGS);

	char path[64];
	snprintf(path, sizeof(path), "sdmc:/c64_shot_%d.bmp", shotCounter++);
	FILE* f = fopen(path, "wb");
	if (f)
	{
		u32 dataSize = W * 240 * 3;
		u8 head[54] = { 'B', 'M' };
		u32 v;
		v = 54 + dataSize; memcpy(head + 2, &v, 4);
		v = 54; memcpy(head + 10, &v, 4);
		v = 40; memcpy(head + 14, &v, 4);
		v = W; memcpy(head + 18, &v, 4);
		v = 240; memcpy(head + 22, &v, 4);
		u16 s = 1; memcpy(head + 26, &s, 2);
		s = 24; memcpy(head + 28, &s, 2);
		memcpy(head + 34, &dataSize, 4);
		fwrite(head, 54, 1, f);
		static u8 row[400 * 3];
		for (int r = 0; r < 240; r++)
		{
			for (int x = 0; x < W; x++)
				memcpy(&row[x * 3], &buf[(x * 240 + r) * 3], 3);
			fwrite(row, W * 3, 1, f);
		}
		fclose(f);
		logf_("captura %s\n", path);
	}
	linearFree(buf);
}

// ---------------------------------------------------------------- medición
struct BenchFrame { float ms, cpu, gpu, logic; u32 tris, calls, chunks; };
struct Bench
{
	bool active;
	u64 startTick, lastTick;
	u32 frames;
	float worstMs, cpuSum, gpuSum, logicSum;
	u64 trisSum, callsSum;
	BenchFrame worst[3];   // los tres peores cuadros con su desglose
};

// puntos que recorre la medición: checkpoints + fresas (los niveles extra tienen un solo checkpoint)
static std::vector<CheckpointInfo> benchPoints;
static void buildBenchPoints()
{
	benchPoints = checkpoints;
	for (auto& st : Actors::strawberries)
		benchPoints.push_back({ st.position - Vec3(0, 0, 8), 0 });
	if (benchPoints.size() < 6 && benchPoints.size() >= 2)
	{
		// en mapas chicos: puntos intermedios entre el inicio y la fresa
		Vec3 a = benchPoints.front().position, b = benchPoints.back().position;
		for (int i = 1; i <= 4; i++)
			benchPoints.push_back({ Lerp(a, b, i / 5.0f), 0 });
	}
}

constexpr float BENCH_SECS_PER_CP = 2.5f;

static void checkpointCamera(FreeCam& cam, u32 i, float turn)
{
	const std::vector<CheckpointInfo>& pts = benchPoints.empty() ? checkpoints : benchPoints;
	const CheckpointInfo& c = pts[i < pts.size() ? i : 0];
	float a = c.angle * DEG2RAD - PI / 2 + turn;   // Map.cs: Facing = AngleToVector(angle - 90°)
	cam.pos = Vec3(c.position.x - cosf(a) * 60.0f, c.position.y - sinf(a) * 60.0f, c.position.z + 35.0f);
	freeLookAt(cam, c.position + Vec3(0, 0, 8));
}

// ---------------------------------------------------------------- jugador
static void spawnPlayer(int checkpoint, bool respawned, bool reveal = false, Cassette* resume = nullptr)
{
	const CheckpointInfo& c = checkpoints[checkpoint];
	delete playerModel;
	playerModel = new SkinnedModel(&playerData);
	player = Player();
	Actors::player = &player;
	Actors::currentCheckpoint = checkpoint;
	// Checkpoint.Added: la bandera del checkpoint actual se corre 8 hacia -Y
	for (auto& fl : Actors::flags)
	{
		fl.position = checkpoints[fl.checkpoint].position;
		if (fl.checkpoint == checkpoint)
			fl.position.y -= 8;
	}
	// HandleActorCreation: Facing = AngleToVector(angle - 90°)
	player.Init(playerModel, c.position, Calc::AngleToVector(c.angle * DEG2RAD - PI / 2), respawned, reveal, resume);
}

// la partida guardada aplicada al mapa cargado
static void applySave()
{
	for (auto& st : Actors::strawberries)
	{
		st.collected = Save.record.strawberries.count(st.sid) > 0;
		if (st.collected) st.locked = false;
		st.destroyed = false;
	}
	Actors::collectedCount = (int)Save.record.strawberries.size();
	for (auto& n : Cutscenes::npcs) n.CheckForDialog();
}

// descarga el mapa actual y carga otro ("1" o un nivel extra "1-N")
static bool loadMap(const std::string& name)
{
	unloadLevel();
	logf_("  memoria tras descargar: %lu KB\n", linearSpaceFree() / 1024);
	if (!loadLevel(name.c_str())) return false;
	logf_("  memoria tras nivel: %lu KB\n", linearSpaceFree() / 1024);
	Actors::mapName = name;
	Actors::submap = name != "1";
	loadSkyTextures();
	logf_("  memoria tras cielo: %lu KB\n", linearSpaceFree() / 1024);
	free(visible);
	visible = (u16*)malloc(sizeof(u16) * hdr.numChunks);
	Actors::LoadModels();
	Cutscenes::InitNpcs();
	applySave();
	Actors::AddedAll();
	logf_("mapa %s cargado, memoria lineal libre: %lu KB\n", name.c_str(), linearSpaceFree() / 1024);
	return true;
}



// LevelInfo.Enter: entra al nivel con el récord guardado (o borrado si se eligió "Restart")
static void enterWorld(bool restart)
{
	if (restart)
	{
		Save.EraseRecord();
		Save.Write();
	}
	if (Actors::mapName != "1") loadMap("1");
	applySave();
	Actors::ResetForRespawn();
	inEnding = false;
	int cp = Save.record.checkpoint < (int)checkpoints.size() ? Save.record.checkpoint : 0;
	spawnPlayer(cp, false);
	paused = false;
	Audio::PlayMusic(levelMusic);
	musicTime = 0;
	lastBeat = -1;
	ambience = 0;
	scene = SceneId::World;
}

// paso lógico de 1/60 s (World.Update reducido al jugador)

static void gameStep()
{
	Game::Elapsed += Game::Delta;
	if (!inEnding) Save.record.time += Game::Delta;   // World.Update: el tiempo no corre en la zona final
	musicTime += Game::Delta;
	if (paused) return;
	if (player.Dead)
	{
		player.Update();
		player.LateUpdate();
		if (player.RequestRespawn && !inTransition())
			startTransition(WipeAngled, WipeAngled, 0, []
			{
				// Perform: el mundo se recarga con el jugador reapareciendo en el checkpoint
				Actors::ResetForRespawn();
				spawnPlayer(Actors::currentCheckpoint, true);
			});
		return;
	}
	// cinemática que congela todo lo demás (desbloqueo de fresa)
	if (Cutscenes::FreezeGame())
	{
		Cutscenes::Update(Game::Delta);
		return;
	}
	if (Game::HitStun > 0)
	{
		Game::HitStun -= Game::Delta;
		return;
	}
	Actors::GeneralTimer += Game::Delta;
	Actors::UpdateSolids(Game::Delta);
	player.Update();
	Actors::Update(Game::Delta);
	Cutscenes::UpdateNpcs(Game::Delta);
	Cutscenes::Update(Game::Delta);
	player.LateUpdate();
	Cutscenes::LateUpdateNpcs();

	// pulsos de la música (FMOD avisaba en cada tiempo): "Cassette Jam" va a 100 por minuto
	int beat = (int)(musicTime * 100.0 / 60.0);
	if (beat != lastBeat)
	{
		lastBeat = beat;
		Actors::beatCounter++;
		Actors::OnBeat(Actors::beatCounter);
	}

	// zona final: "at_baddy" cambia la música a Inner Goth
	bool nowEnding = !Actors::submap && World::OverlapsVolume(VolumeKind::Ending, player.Position) != nullptr;
	if (nowEnding != inEnding)
	{
		inEnding = nowEnding;
		Audio::PlayMusic(inEnding ? "mus_lvl1_baddy" : levelMusic);
	}

	// cambios de mapa que pidió la lógica (cassettes)
	if (player.PendingRequest != Player::Request::None && !inTransition())
	{
		Player::Request req = player.PendingRequest;
		std::string map = player.RequestMap;
		player.PendingRequest = Player::Request::None;
		Audio::Stop(ambience);   // EnterCassette: Game.Ambience.Stop()
		ambience = -1;
		if (req == Player::Request::EnterSubmap)
		{
			returnCheckpoint = Actors::currentCheckpoint;
			returnCamera = Game::Camera;
			startTransition(WipeSpotlight, WipeSpotlight, 0, [map]
			{
				Audio::StopMusic();
				Audio::StopAll();
				if (loadMap(map))
					spawnPlayer(0, false, true);
				else
				{
					// no cupo en memoria: de vuelta al nivel principal, saliendo del cassette
					loadMap("1");
					Cassette* back = nullptr;
					for (auto& c : Actors::cassettes)
						if (c.map == map) back = &c;
					Game::Camera = returnCamera;
					spawnPlayer(returnCheckpoint, false, false, back);
				}
				Audio::PlayMusic(levelMusic);
				musicTime = 0;
				lastBeat = -1;
				ambience = 0;
			});
		}
		else if (req == Player::Request::LeaveSubmap)
		{
			startTransition(WipeSpotlight, WipeSpotlight, 0, []
			{
				std::string from = Actors::mapName;
				Audio::StopMusic();
				Audio::StopAll();
				loadMap("1");
				Cassette* back = nullptr;
				for (auto& c : Actors::cassettes)
					if (c.map == from) back = &c;
				Game::Camera = returnCamera;
				spawnPlayer(returnCheckpoint, false, false, back);
				Audio::PlayMusic(levelMusic);
				musicTime = 0;
				ambience = 0;
			});
		}
		else
		{
			Save.Write();
			startTransition(WipeSpotlight, WipeSlide, 0, []
			{
				Audio::StopAll();
				Overworld::Enter(true);
				scene = SceneId::Overworld;
			});
		}
	}
}

// ---------------------------------------------------------------- modo automático
struct AutoStep { char kind; int frames; u32 keys; FreeCam cam; };
static std::vector<AutoStep> autoSteps;
static int autoIndex = -1, autoFrames;

static u32 parseKeys(const char* s)
{
	static const struct { const char* name; u32 key; } names[] = {
		{ "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
		{ "UP", KEY_CPAD_UP }, { "DOWN", KEY_CPAD_DOWN }, { "LEFT", KEY_CPAD_LEFT }, { "RIGHT", KEY_CPAD_RIGHT },
		{ "DUP", KEY_DUP }, { "DDOWN", KEY_DDOWN }, { "DLEFT", KEY_DLEFT }, { "DRIGHT", KEY_DRIGHT },
	};
	u32 keys = 0;
	char buf[64];
	strncpy(buf, s, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = 0;
	for (char* tok = strtok(buf, "+"); tok; tok = strtok(nullptr, "+"))
		for (auto& n : names)
			if (!strcmp(tok, n.name)) keys |= n.key;
	return keys;
}

static void loadAutoScript()
{
	FILE* f = fopen("sdmc:/c64_auto.txt", "r");
	if (!f) return;
	char line[128], word[64];
	while (fgets(line, sizeof(line), f))
	{
		AutoStep s = {};
		int n;
		if (sscanf(line, "wait %d", &n) == 1) { s.kind = 'w'; s.frames = n; }
		else if (sscanf(line, "hold %63s %d", word, &n) == 2) { s.kind = 'h'; s.keys = parseKeys(word); s.frames = n; }
		else if (!strncmp(line, "shotb", 5)) s.kind = 'S';   // pantalla de abajo
		else if (!strncmp(line, "shot", 4)) s.kind = 's';
		else if (!strncmp(line, "log", 3)) s.kind = 'l';
		else if (!strncmp(line, "game", 4)) s.kind = 'g';
		else if (!strncmp(line, "bench", 5)) s.kind = 'b';
		else if (!strncmp(line, "nextmap", 7)) s.kind = 'm';
		else if (sscanf(line, "tp %f %f %f", &s.cam.pos.x, &s.cam.pos.y, &s.cam.pos.z) == 3) s.kind = 't';
		else if (!strncmp(line, "pause", 5)) s.kind = 'P';
		else if (sscanf(line, "free %f %f %f %f %f", &s.cam.pos.x, &s.cam.pos.y, &s.cam.pos.z, &s.cam.yaw, &s.cam.pitch) == 5) s.kind = 'f';
		else continue;
		autoSteps.push_back(s);
	}
	fclose(f);
	if (!autoSteps.empty()) { autoIndex = 0; logf_("modo automático: %d pasos\n", (int)autoSteps.size()); }
}

// circle pad simulado a partir de las teclas
static void injectInput(u32 keys, u32 prevKeys)
{
	(void)prevKeys;
	Controls::Move = Vec2();
	if (keys & KEY_CPAD_UP) Controls::Move.y -= 1;
	if (keys & KEY_CPAD_DOWN) Controls::Move.y += 1;
	if (keys & KEY_CPAD_LEFT) Controls::Move.x -= 1;
	if (keys & KEY_CPAD_RIGHT) Controls::Move.x += 1;
	if (Controls::Move.Length() > 1) Controls::Move = Controls::Move.Normalized();
	Controls::Camera = Vec2();
	if (keys & KEY_DLEFT) Controls::Camera.x -= 1;
	if (keys & KEY_DRIGHT) Controls::Camera.x += 1;
	if (keys & KEY_DUP) Controls::Camera.y -= 1;
	if (keys & KEY_DDOWN) Controls::Camera.y += 1;
	auto act = [&](VirtualAction& a, u32 mask)
	{
		bool now = (keys & mask) != 0, before = (prevKeys & mask) != 0;
		a.down = now;
		if (now && !before) a.tPressed = a.buffer > 0 ? a.buffer : 1.0f / 60.0f;
	};
	act(Controls::Jump, KEY_B | KEY_X);
	act(Controls::Dash, KEY_Y | KEY_A);
	act(Controls::Climb, KEY_L | KEY_R);
	act(Controls::Confirm, KEY_A);
	act(Controls::Cancel, KEY_B);
}

static void logPlayer()
{
	Vec3 p = player.Position, v = player.Velocity();
	logf_("jugador: estado %s, pos (%.1f, %.1f, %.1f), vel (%.1f, %.1f, %.1f), dashes %d, checkpoint %d\n",
		player.StateName(), p.x, p.y, p.z, v.x, v.y, v.z, player.Dashes(), Actors::currentCheckpoint);
}

// ---------------------------------------------------------------- main
static void cycleMap()
{
	// cambia de mapa: 1 -> 1-1 -> ... -> 1-10 -> 1 (para medir)
	static const char* maps[] = { "1", "1-1", "1-2", "1-3", "1-4", "1-5", "1-6", "1-7", "1-8", "1-9", "1-10" };
	int cur = 0;
	for (int m = 0; m < 11; m++) if (Actors::mapName == maps[m]) cur = m;
	std::string next = maps[(cur + 1) % 11];
	startTransition(WipeAngled, WipeAngled, 0, [next]
	{
		Audio::StopMusic();
		Audio::StopAll();
		if (!loadMap(next)) loadMap("1");
		spawnPlayer(0, false);
		Audio::PlayMusic(levelMusic);
		musicTime = 0;
		lastBeat = -1;
		ambience = 0;
	});
}

int main()
{
	gfxInitDefault();
	gfxSet3D(true);
	romfsInit();

	if (__3dslink_host.s_addr)
	{
		static u32* socBuf;
		socBuf = (u32*)memalign(0x1000, 0x100000);
		if (socBuf && socInit(socBuf, 0x100000) == 0)
			link3dsConnectToHost(false, true);
	}

	logFile = fopen("sdmc:/c64_log.txt", "w");
	bool isNew = false;
	APT_CheckNew3DS(&isNew);
	logf_("Celeste 64 3DS - prueba 5 (%s)\n", isNew ? "New 3DS" : "Old 3DS");

	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2);
	C3D_RenderTarget* targetL = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTarget* targetR = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(targetL, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTargetSetOutput(targetR, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
	targetBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH16);
	C3D_RenderTargetSetOutput(targetBottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	Gfx::Init();

	Save.Load();
	C3D_FrameRate(Save.fps30 ? 30.0f : 60.0f);
	Sprites::Init();
	if (!loadMap("1") || !playerData.Load("player"))
	{
		logf_("No se pudo cargar el nivel.\n");
		while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
		C3D_Fini(); romfsExit(); gfxExit();
		return 0;
	}
	initSkybox();
	Actors::IsVisible = sphereVisible;
	Actors::OnStrawberryUnlock = [](Strawberry& s) { Cutscenes::StrawberryUnlock(s); };
	int startCheckpoint = Save.record.checkpoint < (int)checkpoints.size() ? Save.record.checkpoint : 0;
	spawnPlayer(startCheckpoint, false);

	// World(): menú de pausa
	optionsMenu.Title = "Options";
	optionsMenu.AddToggle("Z-Guide", [] { Save.zGuide = !Save.zGuide; }, [] { return Save.zGuide; });
	optionsMenu.AddToggle("Timer", [] { Save.speedrunTimer = !Save.speedrunTimer; }, [] { return Save.speedrunTimer; });
	// propio del port: 30 cuadros fijos (más parejo que saltar entre 60 y 30)
	optionsMenu.AddToggle("30 FPS", [] { Save.fps30 = !Save.fps30; C3D_FrameRate(Save.fps30 ? 30.0f : 60.0f); },
		[] { return Save.fps30; });
	optionsMenu.AddMultiSelect("Inverted Camera", { "None", "X", "Y", "Both" },
		[] { return Save.invertCamera; }, [](int i) { Save.invertCamera = i; });
	optionsMenu.AddSpacer();
	optionsMenu.AddSlider("BGM", 0, 10, [] { return Save.musicVolume; },
		[](int i) { Save.musicVolume = i; Audio::SetMusicVolume(i / 10.0f); });
	optionsMenu.AddSlider("SFX", 0, 10, [] { return Save.sfxVolume; },
		[](int i) { Save.sfxVolume = i; Audio::SetSfxVolume(i / 10.0f); });
	pauseMenu.Title = "Paused";
	pauseMenu.AddOption("Resume", [] { paused = false; Audio::Play("ui_unpause"); });
	pauseMenu.AddOption("Retry", [] { paused = false; player.Kill(); });
	pauseMenu.AddSubmenu("Options", &optionsMenu);
	pauseMenu.AddOption("Save & Quit", []
	{
		// vuelve al mapa con la postal ya volteada (Overworld(true))
		Save.Write();
		startTransition(WipeAngled, WipeAngled, 0, []
		{
			paused = false;
			pauseMenu.CloseSubMenus();
			Audio::Stop(ambience);
			ambience = -1;
			Audio::StopAll();
			Overworld::Enter(true);
			scene = SceneId::Overworld;
		});
	});

	// música y ambiente del mapa (worldspawn: music "mus_lvl1", ambience "mountain")
	Audio::Init();
	Audio::SetMusicVolume(Save.musicVolume / 10.0f);
	Audio::SetSfxVolume(Save.sfxVolume / 10.0f);
	ambience = -1;   // -1 = sin ambiente; 0 = pedir cuando su muestra esté decodificada
	Title::Enter();
	loadAutoScript();

	enum class Mode { Game, Free, Bench } mode = Mode::Game;
	FreeCam freeCam;
	checkpointCamera(freeCam, 0, 0);
	Bench bench = {};
	u64 lastTick = svcGetSystemTick();
	double accumulator = 0;
	u32 fpsFrames = 0;
	u64 fpsTick = lastTick;
	float fps = 0, logicMs = 0;
	int stepsLast = 0;
	bool wantShot = false, wantShotBottom = false;
	u32 autoKeys = 0, autoPrevKeys = 0;
	bool autoDone = false;

	logf_("memoria lineal libre: %lu KB\n", linearSpaceFree() / 1024);

	while (aptMainLoop())
	{
		u64 now = svcGetSystemTick();
		double frameSecs = (now - lastTick) / (double)SYSCLOCK_ARM11;
		lastTick = now;

		hidScanInput();
		u32 down = hidKeysDown();

		// World.Update: START abre / cierra el menú de pausa
		bool canPause = scene == SceneId::World && !player.Dead && !player.InStrawbGet() && !inTransition() && !Cutscenes::Active();
		if ((down & KEY_START) && autoIndex < 0)
		{
			if (!paused && canPause) { paused = true; pauseMenu.Index = 0; Audio::Play("ui_pause"); }
			else if (paused) { pauseMenu.CloseSubMenus(); paused = false; Audio::Play("ui_unpause"); }
		}
		if (quitRequested) break;

		float iod = osGet3DSliderState() * IOD_MAX;

		// ---- título y mapa
		if (scene != SceneId::World)
		{
			float dt = (float)fmin(frameSecs, 0.1);
			if (autoIndex >= 0)
			{
				// el modo automático también sirve aquí
				AutoStep& st = autoSteps[autoIndex];
				autoPrevKeys = autoKeys;
				autoKeys = st.kind == 'h' ? st.keys : 0;
				injectInput(autoKeys, autoPrevKeys);
				bool done = st.kind == 's' || st.kind == 'S' || st.kind == 'l' ? true : ++autoFrames >= st.frames;
				if (st.kind == 's') wantShot = true;
				if (st.kind == 'S') wantShotBottom = true;
				if (done) { autoFrames = 0; if (++autoIndex >= (int)autoSteps.size()) { autoDone = true; autoIndex = (int)autoSteps.size() - 1; } }
			}
			else
				Controls::Sample();
			if (!inTransition())
			{
				if (scene == SceneId::Title)
				{
					Title::Result r = Title::Update(dt);
					if (r == Title::ToOverworld)
						startTransition(WipeAngled, WipeAngled, 0, [] { Overworld::Enter(false); scene = SceneId::Overworld; });
					else if (r == Title::Exit)
						break;
				}
				else
				{
					Overworld::Result r = Overworld::Update(dt);
					if (r == Overworld::ToTitle)
						startTransition(WipeAngled, WipeAngled, 0, [] { Title::Enter(); scene = SceneId::Title; });
					else if (r == Overworld::EnterLevel)
						startTransition(WipeSlide, WipeSpotlight, 1.5f, [] { enterWorld(Overworld::RestartRequested()); });
				}
			}
			else if (scene == SceneId::Title) Title::Update(dt);
			else Overworld::Update(dt);
			transitionUpdate(dt);
			Controls::Step(dt);
			Audio::Update();

			Sprites::BeginFrame();
			C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
			if (scene == SceneId::Overworld) Overworld::PrepareTextures();
			u32 clear = scene == SceneId::Title ? 0x000000FF : 0x0b090dFF;
			for (int eye = 0; eye < (iod > 0 ? 2 : 1); eye++)
			{
				C3D_RenderTarget* t = eye == 0 ? targetL : targetR;
				C3D_RenderTargetClear(t, C3D_CLEAR_ALL, clear, 0);
				C3D_FrameDrawOn(t);
				Gfx::MarkUnbound();
				if (scene == SceneId::Title) Title::Render(eye == 0 ? -iod : iod);
				else Overworld::Render(eye == 0 ? -iod : iod);
				Sprites::Begin2D();
				drawTransition();
				Sprites::End2D();
			}
			drawBottomScreen(false);
			C3D_FrameEnd(0);
			if (wantShot) { saveScreenshot(targetL); wantShot = false; }
		if (wantShotBottom) { saveScreenshot(targetBottom); wantShotBottom = false; }
			if (autoDone) { logf_("FIN\n"); break; }
			continue;
		}

		// pantalla táctil: arriba = cámara libre, abajo = medición
		if ((down & KEY_TOUCH) && (hidKeysHeld() & KEY_SELECT))
		{
			touchPosition t;
			hidTouchRead(&t);
			if (t.py >= 180 && !inTransition())
			{
				mode = Mode::Game;
				cycleMap();
			}
			else if (t.py < 90)
			{
				mode = mode == Mode::Free ? Mode::Game : Mode::Free;
				if (mode == Mode::Free)
				{
					freeCam.pos = Game::Camera.Position;
					freeLookAt(freeCam, Game::Camera.LookAt);
				}
			}
			else if (mode != Mode::Bench)
			{
				mode = Mode::Bench;
				bench = {};
				bench.active = true;
				bench.startTick = bench.lastTick = now;
				buildBenchPoints();
				logf_("medición iniciada en %s (3D %s, %d puntos)\n", Actors::mapName.c_str(), iod > 0 ? "activado" : "desactivado", (int)benchPoints.size());
			}
		}

		// ---- modo automático
		int steps;
		if (autoIndex >= 0)
		{
			AutoStep& s = autoSteps[autoIndex];
			autoPrevKeys = autoKeys;
			autoKeys = 0;
			bool done = false;
			switch (s.kind)
			{
				case 'w': done = ++autoFrames >= s.frames; break;
				case 'h': autoKeys = s.keys; done = ++autoFrames >= s.frames; break;
				case 's': wantShot = true; done = true; break;
				case 'S': wantShotBottom = true; done = true; break;
				case 'l': logPlayer(); done = true; break;
				case 't': player.Position = s.cam.pos; done = true; break;
				case 'P': paused = true; pauseMenu.Index = 0; done = true; break;
				case 'g': mode = Mode::Game; done = true; break;
				case 'm': if (!inTransition()) { mode = Mode::Game; cycleMap(); done = true; } break;
				case 'f': mode = Mode::Free; freeCam = s.cam; done = true; break;
				case 'b':
					if (autoFrames++ == 0) { mode = Mode::Bench; bench = {}; bench.active = true; bench.startTick = bench.lastTick = now; buildBenchPoints(); }
					done = !bench.active;
					break;
			}
			injectInput(autoKeys, autoPrevKeys);
			if (done)
			{
				autoFrames = 0;
				if (++autoIndex >= (int)autoSteps.size()) { autoDone = true; autoIndex = (int)autoSteps.size() - 1; }
			}
			steps = 1;   // determinista: un paso por cuadro
		}
		else
		{
			Controls::Sample();
			accumulator += frameSecs;
			if (accumulator > MAX_STEPS * Game::Delta) accumulator = MAX_STEPS * Game::Delta;
			steps = (int)(accumulator / Game::Delta);
			accumulator -= steps * Game::Delta;
		}

		// ---- lógica
		u64 logicStart = svcGetSystemTick();
		World::statRayCasts = World::statWallChecks = World::statFacesTested = 0;
		// en cámara libre el mundo sigue avanzando, pero Madeline no recibe controles
		if (mode == Mode::Free && autoIndex < 0)
		{
			Controls::Move = Controls::Camera = Vec2();
			Controls::Jump.down = Controls::Dash.down = Controls::Climb.down = false;
			Controls::Jump.tPressed = Controls::Dash.tPressed = Controls::Climb.tPressed = 0;
		}
		if (paused)
		{
			if (autoIndex < 0) Controls::Sample();
			u32 d = autoIndex >= 0 ? (autoKeys & ~autoPrevKeys) : hidKeysDown();
			pauseMenu.Update((float)frameSecs, d & (KEY_DUP | KEY_CPAD_UP), d & (KEY_DDOWN | KEY_CPAD_DOWN),
				d & (KEY_DLEFT | KEY_CPAD_LEFT), d & (KEY_DRIGHT | KEY_CPAD_RIGHT));
			if (paused && pauseMenu.IsInMainMenu() && Controls::Cancel.ConsumePress())
			{
				paused = false;
				Audio::Play("ui_unpause");
			}
			Controls::Step((float)frameSecs);
			steps = 0;
		}
		if (true)
			for (int i = 0; i < steps; i++)
			{
				gameStep();
				transitionUpdate(Game::Delta);
				updateHud(Game::Delta);
				Controls::Step(Game::Delta);
			}
		stepsLast = steps;
		logicMs = (svcGetSystemTick() - logicStart) * 1000.0f / SYSCLOCK_ARM11;

		// ---- cámara
		ViewPose pose;
		if (mode == Mode::Bench && bench.active)
		{
			float secs = (now - bench.startTick) / (float)SYSCLOCK_ARM11;
			if (bench.frames > 0)
			{
				float ms = (now - bench.lastTick) * 1000.0f / SYSCLOCK_ARM11;
				if (ms > bench.worstMs) bench.worstMs = ms;
				bench.cpuSum += C3D_GetProcessingTime();
				bench.gpuSum += C3D_GetDrawingTime();
				bench.logicSum += logicMs;
				BenchFrame bf = { ms, C3D_GetProcessingTime(), C3D_GetDrawingTime(), logicMs, statTris, statCalls, visibleCount };
				for (int w = 0; w < 3; w++)
					if (bf.ms > bench.worst[w].ms)
					{
						for (int k = 2; k > w; k--) bench.worst[k] = bench.worst[k - 1];
						bench.worst[w] = bf;
						break;
					}
			}
			bench.lastTick = now;
			if (secs >= BENCH_SECS_PER_CP * benchPoints.size())
			{
				for (auto& w : bench.worst)
					logf_("  cuadro lento: %.1f ms (CPU %.1f, GPU %.1f, logica %.1f, %lu tri, %lu llamadas, %lu trozos)\n",
						w.ms, w.cpu, w.gpu, w.logic, w.tris, w.calls, w.chunks);
				u32 n = bench.frames > 1 ? bench.frames - 1 : 1;
				u32 fr = bench.frames ? bench.frames : 1;
				logf_("RESULTADO: %.1f fps promedio, peor cuadro %.1f ms, CPU %.2f ms, GPU %.2f ms, logica %.2f ms, %llu tri/cuadro, %llu llamadas/cuadro, 3D %s\n",
					bench.frames / secs, bench.worstMs, bench.cpuSum / n, bench.gpuSum / n, bench.logicSum / n,
					bench.trisSum / fr, bench.callsSum / fr, iod > 0 ? "activado" : "desactivado");
				bench.active = false;
				mode = Mode::Game;
			}
			else
			{
				u32 i = (u32)(secs / BENCH_SECS_PER_CP);
				float t = (secs - i * BENCH_SECS_PER_CP) / BENCH_SECS_PER_CP;
				checkpointCamera(freeCam, i, t * TAU);
			}
		}
		if (mode == Mode::Free && autoIndex < 0)
		{
			float dt = (float)frameSecs;
			circlePosition cp;
			hidCircleRead(&cp);
			u32 held = hidKeysHeld();
			float speed = (held & KEY_A) ? 360.0f : 120.0f;
			float mx = abs(cp.dx) > 15 ? cp.dx / 156.0f : 0;
			float my = abs(cp.dy) > 15 ? cp.dy / 156.0f : 0;
			float fx = sinf(freeCam.yaw), fy = cosf(freeCam.yaw);
			freeCam.pos.x += (fx * my + fy * mx) * speed * dt;
			freeCam.pos.y += (fy * my - fx * mx) * speed * dt;
			if (held & KEY_R) freeCam.pos.z += speed * dt;
			if (held & KEY_L) freeCam.pos.z -= speed * dt;
			if (held & KEY_DLEFT) freeCam.yaw -= 1.8f * dt;
			if (held & KEY_DRIGHT) freeCam.yaw += 1.8f * dt;
			if (held & KEY_DUP) freeCam.pitch += 1.2f * dt;
			if (held & KEY_DDOWN) freeCam.pitch -= 1.2f * dt;
			freeCam.pitch = Calc::Clamp(freeCam.pitch, -1.5f, 1.5f);
		}
		if (mode == Mode::Game)
			pose = { Game::Camera.Position, Game::Camera.LookAt, FOV_Y * Game::Camera.FOVMultiplier };
		else
			pose = freePose(freeCam);

		// ---- audio: el oyente es la cámara (World.Update: Audio.SetListener(Camera))
		Audio::SetListener(pose.eye, (pose.target - pose.eye).Normalized());
		Audio::Update();
		if (ambience == 0) ambience = Audio::Play("amb_mountain");

		// ---- descarte por frustum (también lo usan los actores)
		statTris = statCalls = 0;
		cullChunks(pose, iod);

		// ---- sprites 3D del cuadro (los mismos para los dos ojos)
		Sprites::BeginFrame();
		{
			Vec3 fwd = (pose.target - pose.eye).Normalized();
			Vec3 right = Cross(fwd, Vec3::UnitZ()).Normalized();
			Vec3 camLeft = -right, camUp = Cross(right, fwd);
			static C3D_Tex* circle = Gfx::GetTexture("spr_circle", false);
			// sombra puntual de Madeline (ICastPointShadow)
			if (player.PointShadowAlpha > 0 && !player.InStrawbGet())
				AddShadowSprite(player.Position + Vec3::UnitZ(), player.PointShadowAlpha);
			// guía de caída (Save.ZGuide): un círculo gris cada 5 unidades hasta el suelo
			if (Save.zGuide && !player.OnGround() && !player.Dead && player.PointShadowAlpha > 0)
			{
				float distance = 1000.0f;
				RayHit hit;
				if (World::SolidRayCast(player.Position, -Vec3::UnitZ(), distance, hit))
					distance = hit.distance;
				const Color4 gray = Color4(128 / 255.0f, 128 / 255.0f, 128 / 255.0f, 1) * 0.50f;
				for (int i = 3; i < distance; i += 5)
					Sprites::Billboard(circle, player.Position - Vec3::UnitZ() * (float)i, 0.5f, gray, camLeft, camUp);
			}
			Actors::CollectSprites(pose.eye, camLeft, camUp);
			Cutscenes::CollectSprites(camLeft, camUp);
			Actors::CollectSnow(pose.eye, fwd, camLeft, camUp);
			if (player.InBubble())
			{
				static C3D_Tex* bubble = Gfx::GetTexture("spr_bubble", false);
				Sprites::Billboard(bubble, player.Position + Vec3::UnitZ() * 8, 10, Color4(), camLeft, camUp, true);
			}

			// bolitas al morir / reaparecer (Player.CollectSprites con drawOrbs)
			if (player.DrawOrbs() && player.OrbsEase() > 0)
			{
				float ease = player.OrbsEase();
				Color4 col = ((int)floorf(ease * 10) % 2 == 0) ? Color4::FromRGB(player.HairColor()) : Color4();
				float s = ease < 0.5f ? 0.5f + ease : Ease::CubeOut(1 - (ease - 0.5f) * 2);
				for (int i = 0; i < 8; i++)
				{
					float rot = (i / 8.0f + ease * 0.25f) * TAU;
					float rad = Ease::CubeOut(ease) * 16;
					Vec3 pos = player.WaistPosition() + camLeft * (cosf(rot) * rad) + camUp * (sinf(rot) * rad);
					float size = 3 * s;
					Sprites::Billboard(circle, pos, size + 0.5f, Color4(0, 0, 0, 1), camLeft, camUp, true);
					Sprites::Billboard(circle, pos, size, col, camLeft, camUp, true);
				}
			}
		}

		// ---- dibujo

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(targetL, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(targetL);
		renderEye(pose, -iod);
		drawHud();
		if (iod > 0.0f)
		{
			C3D_RenderTargetClear(targetR, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(targetR);
			renderEye(pose, iod);
			drawHud();
		}
		float cmdUse = C3D_GetCmdBufUsage();
		drawBottomScreen(true);
		C3D_FrameEnd(0);

		if (wantShot) { saveScreenshot(targetL); wantShot = false; }
		if (wantShotBottom) { saveScreenshot(targetBottom); wantShotBottom = false; }
		if (autoDone) { logf_("FIN\n"); break; }
		if (bench.active) { bench.frames++; bench.trisSum += statTris; bench.callsSum += statCalls; }

		// ---- estadísticas en la pantalla de abajo
		fpsFrames++;
		if (now - fpsTick >= SYSCLOCK_ARM11 / 2)
		{
			fps = fpsFrames * (float)SYSCLOCK_ARM11 / (now - fpsTick);
			fpsFrames = 0;
			fpsTick = now;
			// datos para la vista de prueba (SELECT en la pantalla de abajo)
			snprintf(debugText, sizeof(debugText),
				"%s 3DS  %s\nFPS %.1f  pasos %d\nCPU %.2f ms  GPU %.2f ms\nLogica %.2f ms  cmdbuf %.0f%%\n"
				"%lu tri  %lu llamadas\nMapa %s  checkpoint %d\nTactil + SELECT: arriba camara libre,\nmedio medicion, abajo cambiar mapa",
				isNew ? "New" : "Old", mode == Mode::Game ? "juego" : (mode == Mode::Free ? "camara libre" : "midiendo..."),
				fps, stepsLast, C3D_GetProcessingTime(), C3D_GetDrawingTime(), logicMs, cmdUse * 100.0f,
				statTris, statCalls, Actors::mapName.c_str(), Actors::currentCheckpoint);
		}
	}

	Audio::Exit();
	if (logFile) fclose(logFile);
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
