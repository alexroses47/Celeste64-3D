// Actores del nivel: fresas, bloques (sólidos dinámicos), resortes, cristales, plumas, banderas,
// partículas, escombros y el pelo de Madeline.
#pragma once
#include <string>
#include <vector>
#include "math3d.h"
#include "model.h"
#include "sprites.h"

class Player;

// ---------------------------------------------------------------- ParticleSystem.cs
struct ParticleTheme { float rate; const char* sprite; Vec3 gravity; float life, size; };

struct ParticleSystem
{
	struct Particle { Vec3 position, velocity; float life; };
	ParticleTheme theme;
	std::vector<Particle> particles;
	size_t maxParticles = 32;
	float accumulator = 0, deltaTime = 0;

	explicit ParticleSystem(const ParticleTheme& t = {}) : theme(t) {}
	void Spawn(Vec3 position, Vec3 velocity, float rateMultiplier);
	void Update(float dt);
	void CollectSprites(Vec3 source, Vec3 camPos, Vec3 camLeft, Vec3 camUp);
};

// ---------------------------------------------------------------- Strawberry.cs
struct Strawberry
{
	int id = 0;
	std::string sid;               // "mapa/n" (Save.CurrentRecord.Strawberries)
	bool hasBubble = false;        // BubbleTo: al recogerla, Madeline viaja en burbuja
	Vec3 bubbleTo;
	Vec3 position;
	bool locked = false;
	bool playUnlockSound = true;
	char unlockGroup[32] = {};
	bool collected = false;
	bool collecting = false;
	bool destroyed = false;
	float pointShadowAlpha = 1;
	float scaleMultiplier = 1;
	float unlockTimer = -1;          // animación al desbloquearse
	SkinnedModel* model = nullptr;
	ParticleSystem particles{ { 10.0f, "spr_particle-star", Vec3(0, 0, 80), 1.0f, 1.2f } };

	float Pulse() const;
	Mat4 ModelTransform() const;
	Mat4 WorldMatrix() const;
};

// ---------------------------------------------------------------- Solid.cs y sus derivados
struct Block
{
	enum Kind { Falling = 1, Floaty, Moving, Traffic, Gate, Break, Cassette, DoubleDash, IntroCar };
	int kind = 0, index = 0;
	Vec3 position, start, end, velocity;
	u32 flags = 0;
	char group[32] = {};
	float tShake = 0;
	Vec3 shake;
	bool alive = true, visible = true;

	// FallingBlock
	int fstate = 0;            // 0 espera, 1 tiembla, 2 cae, 3 aterrizó, 4 reaparece
	bool triggered = false, hasEnd = false;
	Vec3 endPosition;
	float respawnDelay = 0;
	// FloatyBlock
	Vec3 origin;
	bool hasRider = false;
	// MovingBlock
	float lerp = 0, target = 1;
	// TrafficBlock / GateBlock
	int rstate = 0;
	float timer = 0;
	Vec3 normal;
	bool opened = false;
	// DoubleDashPuzzleBlock
	bool ready = false, satisfied = false;
	float pulse = 0;
	u32 pulseColor = 0xffffff;

	bool Slow() const { return flags & 1; }
	bool Secret() const { return flags & 2; }
	bool BouncesPlayer() const { return flags & 4; }
	bool Transparent() const { return flags & 8; }

	Vec3 LocalMin() const;
	Vec3 LocalMax() const;
	bool Satisfied() const { return kind == Falling ? fstate == 3 : (kind == DoubleDash ? satisfied : true); }
	bool HasPlayerRider() const;
	void Added();
	void Update(float dt);
	void MoveTo(Vec3 target);
	void Transformed();
	void Destroy();
	void Shatter(Vec3 direction);   // BreakBlock.Break
	void Trigger() { triggered = true; }
	Vec3 RenderOffset() const { return position + shake; }
};

// ---------------------------------------------------------------- objetos
struct Spring
{
	Vec3 base, position;
	Vec2 facing = Vec2(0, -1);
	int attach = -1;
	float tCooldown = 0;
	SkinnedModel* model = nullptr;
};

struct Refill
{
	Vec3 position;
	bool isDouble = false;
	float tCooldown = 0, tCollect = 0, pointShadowAlpha = 1;
	SkinnedModel* model = nullptr;
	ParticleSystem particles{ { 10.0f, "spr_particle-star", Vec3(0, 0, 90), 0.5f, 2.5f } };
	Mat4 ModelTransform() const;
};

struct Feather
{
	Vec3 position;
	float tCooldown = 0, pointShadowAlpha = 1;
	SkinnedModel* model = nullptr;
	ParticleSystem particles{ { 10.0f, "spr_particle-star", Vec3(0, 0, 90), 0.5f, 2.5f } };
	Mat4 ModelTransform() const;
};

// Cassette.cs
struct Cassette
{
	Vec3 position;
	std::string map;               // nivel extra al que lleva ("" = salir)
	float tCooldown = 1.0f, tWiggle = 0, pointShadowAlpha = 1;
	SkinnedModel* model = nullptr;
	SkinnedModel* collectedModel = nullptr;
	bool IsCollected() const;
	Mat4 ModelTransform() const;
};

// Coin.cs
struct Coin
{
	Vec3 position;
	bool collected = false;
	SkinnedModel* model = nullptr;
};

struct CheckpointFlag
{
	int checkpoint = 0;     // índice en la lista de checkpoints
	Vec3 position;
	float tWiggle = 0;
	SkinnedModel* modelOn = nullptr;
	SkinnedModel* modelOff = nullptr;
};

struct Debris { Vec3 position, velocity; const char* image; float timer; };

// Dust.cs
struct Dust { Vec3 position, velocity; int image; Color4 color; float percent, duration; };

// ---------------------------------------------------------------- Hair.cs
class Hair
{
public:
	int Nodes = 10;
	u32 Color = 0xdb2c00;
	Vec3 Squish = Vec3::One();
	Vec3 Forward;
	bool Grounded = false;
	float Roundness = 0;
	float ForwardOffsetPerNode = 0.50f;

	void Init();
	void Update(float dt, const Mat4& positionTransform);
	void Draw(float alpha = 1);   // escenario ya enlazado; alpha < 1 = estela (premultiplicado)

private:
	std::vector<Vec3> nodes;
	float wave = 0;
};

// ---------------------------------------------------------------- mundo de actores
namespace Actors
{
	extern std::vector<Strawberry> strawberries;
	extern std::vector<Block> blocks;
	extern std::vector<Spring> springs;
	extern std::vector<Refill> refills;
	extern std::vector<Feather> feathers;
	extern std::vector<CheckpointFlag> flags;
	extern std::vector<Cassette> cassettes;
	extern std::vector<Coin> coins;
	extern std::string mapName;              // mapa cargado ("1", "1-3"...)
	extern bool submap;                      // World.Entry.Submap
	extern int beatCounter;                  // Game.audioBeatCounter (bloques de cassette)
	bool AnyCoinRemaining();
	void OnBeat(int index);                  // IListenToAudioCallback
	void Clear();                            // descarga los actores del mapa
	extern std::vector<Debris> debris;
	extern std::vector<Dust> dust;
	extern float snowAmount;
	extern Vec3 snowDirection;
	void AddDust(Vec3 position, Vec3 velocity, Color4 color = Color4(0.7f, 0.75f, 0.8f, 1.0f));
	void CollectSnow(Vec3 camPos, Vec3 camForward, Vec3 camLeft, Vec3 camUp);
	extern ModelData strawberryData, springData, refillData, refillDoubleData, featherData, flagOnData, flagOffData;
	extern ModelData tapeData, tapeCollectedData, coinData;
	extern float GeneralTimer;               // World.GeneralTimer
	extern std::vector<float> floatRate, floatOffset;
	extern int collectedCount;
	extern Player* player;
	extern int currentCheckpoint;            // World.Entry.CheckPoint

	// visibilidad (la pone main con el frustum del cuadro)
	extern bool (*IsVisible)(Vec3 center, float radius);
	// el desbloqueo de una fresa es una cinemática (la pone main)
	extern void (*OnStrawberryUnlock)(Strawberry& s);

	void InitFloating(int groups);
	float FloatingOffset(int group);
	void LoadModels();
	void AddedAll();                         // Added() de bloques y fresas (después de cargar todo)
	void ResetForRespawn();                  // la recarga del mundo al reaparecer

	void UpdateSolids(float dt);             // bloques (antes que el jugador)
	void Update(float dt);                   // resto de actores
	void Pickups(Player& p);                 // IPickup
	void CollectSprites(Vec3 camPos, Vec3 camLeft, Vec3 camUp);
	void DrawModels(bool onlyCollecting);
	void DrawGhosts(C3D_Tex* white);         // fresas guardadas, cassettes y monedas recogidas (transparentes)

	float Rng(float min, float max);
}

// Sprite.CreateShadowSprite
void AddShadowSprite(Vec3 position, float strength);
