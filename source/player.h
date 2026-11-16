// Traducción de Celeste64/Source/Actors/Player.cs (estados de movimiento).
// Pendiente: pluma, fresas, burbujas, cassettes, cinemáticas, pelo dinámico, estelas, polvo y sonido.
#pragma once
#include "math3d.h"
#include "model.h"
#include "world.h"
#include "actors.h"

struct GameCamera
{
	Vec3 Position, LookAt;
	float FOVMultiplier = 1;
	float NearPlane = 20, FarPlane = 800;
};

namespace Game
{
	constexpr float Delta = 1.0f / 60.0f;   // UpdateMode.FixedStep(60)
	extern double Elapsed;                  // Time.Elapsed
	extern float HitStun;                   // World.HitStun
	extern GameCamera Camera;               // World.Camera
	inline bool OnInterval(double interval) { return Calc::OnInterval(Elapsed, Delta, interval); }
}

struct CheckpointInfo { Vec3 position; float angle; };

class Player
{
public:
	enum class State { None = -1, Normal, Dashing, Skidding, Climbing, StrawbGet, FeatherStart, Feather, Cutscene, Respawn, Dead, StrawbReveal, Bubble, Cassette };
	// lo que la lógica le pide a main (cambios de mapa)
	enum class Request { None, EnterSubmap, LeaveSubmap, QuitToOverworld };

	Vec3 Position;
	Vec2 Facing = Vec2(0, -1);       // Actor.facing por defecto: -UnitY
	bool Dead = false;
	Vec3 ModelScale = Vec3::One();
	SkinnedModel* Model = nullptr;
	bool DrawModel = true;
	bool RequestRespawn = false;      // StDeadUpdate pide recargar el mundo
	bool DrawHair = true;
	Hair HairModel;

	// reveal: entrada a un nivel extra (StrawbReveal); resumeCassette: vuelve de uno (sale del cassette)
	void Init(SkinnedModel* model, Vec3 position, Vec2 facing, bool respawned, bool reveal = false, ::Cassette* resumeCassette = nullptr);
	Request PendingRequest = Request::None;
	std::string RequestMap;
	bool InBubble() const { return state == State::Bubble; }
	void Stop() { velocity = Vec3(); }
	void EnterCassette(::Cassette* c);
	void Update();
	void LateUpdate();
	Mat4 ModelMatrix() const;
	State CurrentState() const { return state; }
	const char* StateName() const;
	int Dashes() const { return dashes; }
	bool OnGround() const { return onGround; }
	float PointShadowAlpha = 1.0f;
	bool InStrawbGet() const { return state == State::StrawbGet; }
	bool IsAbleToPickup() const { return state != State::StrawbGet && state != State::Respawn && state != State::Dead; }
	Vec3 Velocity() const { return velocity; }
	Vec3 PreviousVelocity() const { return previousVelocity; }
	u32 HairColor() const { return HairModel.Color; }
	void SetTargetFacing(Vec2 f) { targetFacing = f; }
	bool DrawOrbs() const { return drawOrbs; }
	float OrbsEase() const { return drawOrbsEase; }
	Vec3 WaistPosition() const { return SolidWaistTestPos(); }
	void DrawTrails(C3D_Tex* white);   // estelas del dash (transparentes)
	bool InFeatherState() const { return state == State::FeatherStart || state == State::Feather; }

	// IRidePlatforms
	bool RidingPlatformCheck(int blockIndex);
	void RidingPlatformSetVelocity(Vec3 value);
	void RidingPlatformMoved(Vec3 delta);

	// IPickup
	void StrawbGet(Strawberry* s);
	void Spring(Vec3 springPosition);
	void FeatherGet(Vec3 featherPosition);
	bool RefillDash(int amount = 1);
	void Kill();

private:
	// ---- constantes
	static constexpr float Acceleration = 500;
	static constexpr float PastMaxDeccel = 60;
	static constexpr float AirAccelMultMin = .5f;
	static constexpr float AirAccelMultMax = 1;
	static constexpr float MaxSpeed = 64;
	static constexpr float RotateThreshold = MaxSpeed * .2f;
	static constexpr float RotateSpeed = TAU * 1.5f;
	static constexpr float RotateSpeedAboveMax = TAU * .6f;
	static constexpr float Friction = 800;
	static constexpr float AirFrictionMult = .1f;
	static constexpr float Gravity = 600;
	static constexpr float MaxFall = -120;
	static constexpr float HalfGravThreshold = 100;
	static constexpr float JumpHoldTime = .1f;
	static constexpr float JumpSpeed = 90;
	static constexpr float JumpXYBoost = 10;
	static constexpr float CoyoteTime = .12f;
	static constexpr float WallJumpXYSpeed = MaxSpeed * 1.3f;
	static constexpr float DashSpeed = 140;
	static constexpr float DashEndSpeedMult = .75f;
	static constexpr float DashTime = .2f;
	static constexpr float DashResetCooldown = .2f;
	static constexpr float DashCooldown = .1f;
	static constexpr float DashRotateSpeed = TAU * .3f;
	static constexpr float DashJumpSpeed = 40;
	static constexpr float DashJumpHoldSpeed = 20;
	static constexpr float DashJumpHoldTime = .3f;
	static constexpr float DashJumpXYBoost = 16;
	static constexpr float SkidDotThreshold = -.7f;
	static constexpr float SkiddingStartAccel = 300;
	static constexpr float SkiddingAccel = 500;
	static constexpr float EndSkidSpeed = MaxSpeed * .8f;
	static constexpr float SkidJumpSpeed = 120;
	static constexpr float SkidJumpHoldTime = .16f;
	static constexpr float SkidJumpXYSpeed = MaxSpeed * 1.4f;
	static constexpr float WallPushoutDist = 3;
	static constexpr float ClimbCheckDist = 4;
	static constexpr float ClimbSpeed = 40;
	static constexpr float ClimbHopUpSpeed = 80;
	static constexpr float ClimbHopForwardSpeed = 40;
	static constexpr float ClimbHopNoMoveTime = .25f;
	static constexpr float FootstepInterval = .3f;
	static constexpr float SpringJumpSpeed = 160;
	static constexpr float SpringJumpHoldTime = .3f;
	static constexpr float FeatherStartTime = .4f;
	static constexpr float FeatherFlySpeed = 100;
	static constexpr float FeatherStartSpeed = 140;
	static constexpr float FeatherTurnSpeed = TAU * .75f;
	static constexpr float FeatherAccel = 60;
	static constexpr float FeatherDuration = 2.2f;
	static constexpr float FeatherExitXYMult = .5f;
	static constexpr float FeatherExitZSpeed = 60;
	static constexpr u32 CFeather = 0xf2d450;

	static constexpr u32 CNormal = 0xdb2c00;
	static constexpr u32 CNoDash = 0x6ec0ff;
	static constexpr u32 CTwoDashes = 0xfa91ff;
	static constexpr u32 CRefillFlash = 0xffffff;

	// ---- estado
	State state = State::None;
	Vec3 velocity, previousVelocity, groundNormal, platformVelocity;
	float tPlatformVelocityStorage = 0, tGroundSnapCooldown = 0;
	int climbingWallActor = -1;
	Vec3 climbingWallNormal;
	bool onGround = false;
	Vec2 targetFacing = Vec2::UnitY();
	Vec3 cameraTargetForward = Vec3(0, 1, 0);
	float cameraTargetDistance = 0.50f;
	Vec3 cameraOriginPos;
	float tCoyote = 0, coyoteZ = 0;
	float drawOrbsEase = 0;

	float tHoldJump = 0, holdJumpSpeed = 0;
	bool autoJump = false;
	float tNoMove = 0, tFootstep = 0;

	int dashes = 1;
	float tDash = 0, tDashCooldown = 0, tDashResetCooldown = 0, tDashResetFlash = 0, tNoDashJump = 0;
	bool dashedOnGround = false;
	float tNoSkidJump = 0;

	float climbCornerEase = 0;
	Vec3 climbCornerFrom, climbCornerTo;
	Vec2 climbCornerFacingFrom, climbCornerFacingTo;
	bool climbCornerCameraValid = false;
	Vec2 climbCornerCameraFrom, climbCornerCameraTo;
	int climbInputSign = 1;
	float tClimbCooldown = 0;

	Strawberry* lastStrawb = nullptr;
	Vec2 strawbGetForward;
	float strawbTimer = 0;
	int strawbStage = 0;
	bool cameraOverride = false;
	Vec3 cameraOverridePos, cameraOverrideLookAt;

	float tFeatherStart = 0, featherZ = 0, tFeather = 0, tFeatherWallBumpCooldown = 0;
	bool featherPlayedEndWarn = false;
	u32 lastDashHairColor = CNormal;
	int dashTrailsCreated = 0;
	bool drawOrbs = false;
	struct Trail { SkinnedModel* model = nullptr; Hair hair; Mat4 world; float percent = 1; u32 color = 0; };
	std::vector<Trail> trails;
	void CreateDashTrail();
	int sfxWallSlide = 0, sfxFeather = 0, sfxBubble = 0;   // sonidos en bucle (Sound)

	Vec3 SolidWaistTestPos() const { return Position + Vec3::UnitZ() * 3; }
	Vec3 SolidHeadTestPos() const { return Position + Vec3::UnitZ() * 10; }

	void SetState(State s);
	void Enter(State s);
	void Exit(State s);
	void StateUpdate();

	void GetCameraTarget(Vec3& lookAt, Vec3& position, bool& snap);
	Vec2 RelativeMoveInput();
	void SetHairColor(u32 color);
	void SweepTestMove(Vec3 delta, bool resolveImpact);
	bool Popout(bool resolveImpact);
	void CancelGroundSnap() { tGroundSnapCooldown = 0.1f; }
	void Jump();
	void WallJump();
	void SkidJump();
	void DashJump();
	void AddPlatformVelocity(bool playSound);
	bool ClimbCheckAt(Vec3 offset, WallHit& hit);
	bool TryClimb();
	static bool ClimbNormalCheck(Vec3 n) { return fabsf(n.z) < 0.35f; }
	static bool FloorNormalCheck(Vec3 n) { return !ClimbNormalCheck(n) && n.z > 0; }
	bool WallJumpCheck();
	bool TryDash();
	void SetDashSpeed(Vec2 dir);
	bool GroundCheck(Vec3& pushout, Vec3& normal, int& floor);
	bool CeilingCheck(Vec3& pushout);

	void StNormalEnter();
	void StNormalExit();
	void StNormalUpdate();
	void StDashingEnter();
	void StDashingExit();
	void StDashingUpdate();
	void StSkiddingEnter();
	void StSkiddingExit();
	void StSkiddingUpdate();
	void StClimbingEnter();
	void StClimbingExit();
	void StClimbingUpdate();
	void StStrawbGetEnter();
	void StStrawbGetExit();
	void StStrawbGetUpdate();
	void HandleFeatherZ();
	void StFeatherStartEnter();
	void StFeatherStartUpdate();
	void StFeatherEnter();
	void StFeatherExit();
	void StFeatherUpdate();
	void BreakBlock(int blockIndex, Vec3 direction);
	::Cassette* cassette = nullptr;
	float routineTimer = 0;
	int routineStage = 0;
	Vec3 bubbleFrom, bubbleTo, revealFrom, revealTo, revealControl, revealLookAt;
	void BubbleTo(Vec3 target);
	void StRespawnEnter();
	void StRespawnUpdate();
	void StRespawnExit();
	void StDeadEnter();
	void StDeadUpdate();
};
