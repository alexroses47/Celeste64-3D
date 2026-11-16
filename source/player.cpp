// Traducción línea a línea de Player.cs (solo los estados de movimiento).
// Las referencias "(original: ...)" apuntan a la región equivalente del C#.
#include "player.h"
#include "input.h"
#include "cutscene.h"
#include "audio.h"
#include "save.h"
#include "gfx.h"
#include <string.h>

namespace Game
{
	double Elapsed = 0;
	float HitStun = 0;
	GameCamera Camera;
}

// se conservan entre reapariciones (original: storedCameraForward / storedCameraDistance)
static Vec3 storedCameraForward = Vec3(0, 1, 0);
static float storedCameraDistance = 0.5f;

static constexpr float dt = Game::Delta;

static Vec3 withXY(Vec3 v, Vec2 xy) { return Vec3(xy.x, xy.y, v.z); }

// Utils.UpwardPerpendicularNormal
static Vec3 upwardPerpendicularNormal(Vec3 v)
{
	v = v.Normalized();
	Vec3 arbitrary = fabsf(v.x) > fabsf(v.y) ? Vec3::UnitY() : Vec3::UnitX();
	Vec3 result = Cross(v, arbitrary).Normalized();
	if (result.z < 0) result = -result;
	return result;
}

// Vec3.TransformNormal(v, Matrix.CreateFromAxisAngle(axis, angle))
static Vec3 rotateAround(Vec3 v, Vec3 axis, float angle)
{
	float c = cosf(angle), s = sinf(angle);
	return v * c + Cross(axis, v) * s + axis * (Dot(axis, v) * (1 - c));
}

// ---------------------------------------------------------------- inicio
void Player::Init(SkinnedModel* model, Vec3 position, Vec2 facing, bool respawned, bool reveal, ::Cassette* resumeCassette)
{
	Model = model;
	Position = position;
	Facing = facing;

	Model->SetBlendDuration("Idle", "Dash", 0.05f);
	Model->SetBlendDuration("Idle", "Run", 0.2f);
	Model->SetBlendDuration("Run", "Skid", .125f);
	Model->SetLooping("Dash", false);
	Model->Play("Idle");
	for (auto& e : Model->MaterialEffects) e = 0.60f;
	HairModel.Init();
	SetHairColor(0xdb2c00);

	// Added()
	if (respawned)
	{
		cameraTargetForward = storedCameraForward;
		cameraTargetDistance = storedCameraDistance;
		SetState(State::Respawn);
	}
	else if (reveal)
		SetState(State::StrawbReveal);
	else
		SetState(State::Normal);
	if (resumeCassette)
	{
		// el mundo principal "sigue" después del nivel extra: termina la rutina del cassette
		cameraTargetForward = storedCameraForward;
		cameraTargetDistance = storedCameraDistance;
		EnterCassette(resumeCassette);
		routineTimer = 1.0f;
		routineStage = 1;
	}

	cameraOriginPos = Position;
	if (!resumeCassette)
	{
		Vec3 orig, target;
		bool snap;
		GetCameraTarget(orig, target, snap);
		// (original: igual que en Added(), con los nombres cruzados: la cámara arranca en el punto de mira)
		Game::Camera.LookAt = target;
		Game::Camera.Position = orig;
	}
}

const char* Player::StateName() const
{
	switch (state)
	{
		case State::Normal: return "Normal";
		case State::Dashing: return "Dash";
		case State::Skidding: return "Derrape";
		case State::Climbing: return "Escalando";
		case State::StrawbGet: return "Fresa";
		case State::FeatherStart: case State::Feather: return "Pluma";
		case State::Cutscene: return "Cinematica";
		case State::StrawbReveal: return "Revelacion";
		case State::Bubble: return "Burbuja";
		case State::Cassette: return "Cassette";
		case State::Respawn: return "Reaparece";
		case State::Dead: return "Muerta";
		default: return "-";
	}
}

// ---------------------------------------------------------------- máquina de estados
void Player::SetState(State s)
{
	State prev = state;
	state = s;
	if (prev != State::None) Exit(prev);
	Enter(s);
}

void Player::Enter(State s)
{
	switch (s)
	{
		case State::Normal: StNormalEnter(); break;
		case State::Dashing: StDashingEnter(); break;
		case State::Skidding: StSkiddingEnter(); break;
		case State::Climbing: StClimbingEnter(); break;
		case State::StrawbGet: StStrawbGetEnter(); break;
		case State::FeatherStart: StFeatherStartEnter(); break;
		case State::Feather: StFeatherEnter(); break;
		case State::StrawbReveal: routineTimer = 0; routineStage = 0; break;
		case State::Bubble:
			// StBubbleRoutine: espera .2, viaja en curva a 220 u/s, espera .2
			bubbleFrom = Position;
			routineTimer = 0;
			routineStage = 0;
			break;
		case State::Cassette: routineTimer = 0; routineStage = 0; break;
		case State::Cutscene:
			// StCutsceneEnter
			Model->Play("Idle");
			if (tDashResetFlash > 0) { tDashResetFlash = 0; SetHairColor(CNormal); }
			break;
		case State::Respawn: StRespawnEnter(); break;
		case State::Dead: StDeadEnter(); break;
		default: break;
	}
}

void Player::Exit(State s)
{
	switch (s)
	{
		case State::Normal: StNormalExit(); break;
		case State::Dashing: StDashingExit(); break;
		case State::Skidding: StSkiddingExit(); break;
		case State::Climbing: StClimbingExit(); break;
		case State::StrawbGet: StStrawbGetExit(); break;
		case State::Feather: StFeatherExit(); break;
		case State::StrawbReveal: cameraOverride = false; break;
		case State::Bubble:
			Audio::Play("sfx_bubble_out", Position);
			Audio::Stop(sfxBubble);
			sfxBubble = 0;
			PointShadowAlpha = 1;
			break;
		case State::Cassette:
			if (cassette) cassette->tCooldown = 1.0f;   // SetCooldown
			cassette = nullptr;
			DrawModel = DrawHair = true;
			cameraOverride = false;
			PointShadowAlpha = 1;
			break;
		case State::Respawn: StRespawnExit(); break;
		default: break;
	}
}

void Player::StateUpdate()
{
	switch (state)
	{
		case State::Normal: StNormalUpdate(); break;
		case State::Dashing: StDashingUpdate(); break;
		case State::Skidding: StSkiddingUpdate(); break;
		case State::Climbing: StClimbingUpdate(); break;
		case State::StrawbGet: StStrawbGetUpdate(); break;
		case State::FeatherStart: StFeatherStartUpdate(); break;
		case State::Feather: StFeatherUpdate(); break;
		case State::StrawbReveal:
		{
			// StStrawbRevealRoutine
			routineTimer += dt;
			if (routineStage == 0)
			{
				Strawberry* s = Actors::strawberries.empty() ? nullptr : &Actors::strawberries[0];
				if (!s) { SetState(State::Normal); break; }
				targetFacing = (s->position - Position).XY().Normalized();
				revealLookAt = s->position + Vec3(0, 0, 3);
				Vec3 n = (Position - revealLookAt).Normalized();
				revealFrom = revealLookAt + n * 40 + Vec3::UnitZ() * 20;
				revealTo = Position + Vec3(0, 0, 16) + n * 40;
				revealControl = (revealFrom + revealTo) * .5f + Vec3::UnitZ() * 40;
				cameraOverride = true;
				cameraOverridePos = Game::Camera.Position = revealFrom;
				cameraOverrideLookAt = Game::Camera.LookAt = revealLookAt;
				routineStage = 1;
				routineTimer = 0;
			}
			else if (routineStage == 1 && routineTimer >= 1.0f) { routineStage = 2; routineTimer = 0; }
			else if (routineStage == 2)
			{
				float p = fminf(routineTimer / 3, 1);
				float e = -cosf(PI / 2 * p) + 1;   // Ease.Sine.In
				Vec3 a = Lerp(revealFrom, revealControl, e), b = Lerp(revealControl, revealTo, e);
				cameraOverridePos = Lerp(a, b, e);   // Utils.Bezier
				if (routineTimer >= 3) { routineStage = 3; routineTimer = 0; }
			}
			else if (routineStage == 3)
			{
				Vec3 lookAtTo, posTo;
				bool snap;
				GetCameraTarget(lookAtTo, posTo, snap);
				float t = sinf(PI / 2 * fminf(routineTimer, 1));   // Ease.Sine.Out
				cameraOverridePos = Lerp(revealTo, posTo, t);
				cameraOverrideLookAt = Lerp(revealLookAt, lookAtTo, t);
				if (routineTimer >= 1.02f) SetState(State::Normal);
			}
			break;
		}
		case State::Bubble:
		{
			routineTimer += dt;
			float duration = (bubbleTo - bubbleFrom).Length() / 220;
			if (routineStage == 0 && routineTimer >= .2f)
			{
				routineStage = 1;
				routineTimer = 0;
				sfxBubble = Audio::Play("sfx_bubble_loop", Position);
			}
			else if (routineStage == 1)
			{
				float ease = duration > 0 ? fminf(routineTimer / duration, 1) : 1;
				float s = -(cosf(PI * ease) - 1) / 2;   // Utils.SineInOut
				Vec3 control = (bubbleTo + bubbleFrom) * .5f + Vec3::UnitZ() * 40;
				Position = Lerp(Lerp(bubbleFrom, control, s), Lerp(control, bubbleTo, s), s);
				Audio::SetPosition(sfxBubble, Position);
				if (ease >= 1) { routineStage = 2; routineTimer = 0; }
			}
			else if (routineStage == 2 && routineTimer >= .2f)
				SetState(State::Normal);
			break;
		}
		case State::Cassette:
			// StCassetteRoutine: 1 s y entra al nivel extra (o sale de él); al volver, 1 s más y sale
			routineTimer += dt;
			if (routineStage == 0 && routineTimer >= 1.0f)
			{
				routineStage = 1;
				if (Actors::submap) PendingRequest = Request::LeaveSubmap;
				else if (cassette && !cassette->map.empty()) { PendingRequest = Request::EnterSubmap; RequestMap = cassette->map; }
				else PendingRequest = Request::QuitToOverworld;
				storedCameraForward = cameraTargetForward;
				storedCameraDistance = cameraTargetDistance;
			}
			else if (routineStage == 1 && routineTimer >= 2.0f)
			{
				Audio::Play("sfx_cassette_exit", Position);
				if (cassette) cassette->tWiggle = 1.0f;   // PlayerExit
				SetState(State::Normal);
				velocity = Vec3::UnitZ() * 25;
				holdJumpSpeed = velocity.z;
				tHoldJump = .1f;
				autoJump = true;
			}
			break;
		case State::Cutscene:
			// StCutsceneUpdate
			if (!Cutscenes::Active()) SetState(State::Normal);
			break;
		case State::Respawn: StRespawnUpdate(); break;
		case State::Dead: StDeadUpdate(); break;
		default: break;
	}
}

// ---------------------------------------------------------------- Update (original: Update())
static bool spikeBlockCheck(const Volume& spike, void* user)
{
	const Vec3* velocity = (const Vec3*)user;
	return Dot(velocity->Normalized(), spike.extra) < 0.5f;
}

void Player::Update()
{
	if (state != State::Respawn && state != State::Dead && state != State::StrawbReveal && state != State::Cassette)
	{
		// rotar cámara
		{
			float rot = Vec2(cameraTargetForward.x, cameraTargetForward.y).Angle();
			bool invertX = Save.invertCamera == 1 || Save.invertCamera == 3;
			rot -= Controls::Camera.x * dt * 4 * (invertX ? -1 : 1);
			cameraTargetForward = Vec3(Calc::AngleToVector(rot), 0);
		}
		// acercar / alejar
		if (Controls::Camera.y != 0)
		{
			bool invertY = Save.invertCamera == 2 || Save.invertCamera == 3;
			cameraTargetDistance += Controls::Camera.y * dt * (invertY ? -1 : 1);
			cameraTargetDistance = Calc::Clamp(cameraTargetDistance, 0, 1);
		}
		else
		{
			const float interval = 1.0f / 3, threshold = .1f;
			float m = fmodf(cameraTargetDistance, interval);
			if (m < threshold || m > interval - threshold)
				Calc::ApproachRef(cameraTargetDistance, Calc::Snap(cameraTargetDistance, interval), dt / 2);
		}
	}

	if (state == State::Respawn || state == State::Dead || state == State::Cutscene)
	{
		StateUpdate();
		return;
	}

	// plano de muerte, bloques de muerte y pinchos (no dentro de la burbuja)
	if (!InBubble() && (Position.z < World::DeathPlane ||
		World::OverlapsVolume(VolumeKind::Death, SolidWaistTestPos()) ||
		World::OverlapsVolume(VolumeKind::Spike, SolidWaistTestPos(), spikeBlockCheck, &velocity)))
	{
		Kill();
		return;
	}

	// entrar a una cinemática
	if (Cutscenes::Active())
	{
		SetState(State::Cutscene);
		return;
	}

	// temporizadores
	if (tCoyote > 0) tCoyote -= dt;
	if (tHoldJump > 0) tHoldJump -= dt;
	if (tDashCooldown > 0) tDashCooldown -= dt;
	if (tDashResetCooldown > 0) tDashResetCooldown -= dt;
	if (tDashResetFlash > 0) tDashResetFlash -= dt;
	if (tNoMove > 0) tNoMove -= dt;
	if (tPlatformVelocityStorage > 0) tPlatformVelocityStorage -= dt;
	if (tGroundSnapCooldown > 0) tGroundSnapCooldown -= dt;
	if (tClimbCooldown > 0) tClimbCooldown -= dt;

	previousVelocity = velocity;
	StateUpdate();

	// empuje de personajes (NPC: IHavePushout)
	for (const Npc& n : Cutscenes::npcs)
	{
		if (!n.model || n.pushoutRadius <= 0 || n.pushoutHeight <= 0) continue;
		if (Position.z < n.position.z - 12 || Position.z > n.position.z + n.pushoutHeight) continue;
		Vec2 diff = Position.XY() - n.position.XY();
		if (diff.LengthSquared() > n.pushoutRadius * n.pushoutRadius) continue;
		SweepTestMove(Vec3(diff.Normalized() * (n.pushoutRadius - diff.Length()), 0), false);
	}

	// empuje de props (IHavePushout)
	for (const Volume& v : World::volumes)
	{
		if (v.kind != VolumeKind::Pushout) continue;
		float radius = v.extra.x, height = v.extra.y;
		if (radius <= 0 || height <= 0) continue;
		Vec3 at = v.min;
		if (Position.z < at.z - 12 || Position.z > at.z + height) continue;
		Vec2 diff = Position.XY() - at.XY();
		if (diff.LengthSquared() > radius * radius) continue;
		Vec2 normal = diff.Normalized();
		float pushout = radius - diff.Length();
		SweepTestMove(Vec3(normal * pushout, 0), false);
	}

	// movimiento
	if (!InBubble())
	{
		SweepTestMove(velocity * dt, tNoMove <= 0);
		Popout(false);
	}

	// recogibles (IPickup): fresas, resortes, cristales, plumas y banderas
	if (IsAbleToPickup())
		Actors::Pickups(*this);
}

// ---------------------------------------------------------------- LateUpdate
void Player::LateUpdate()
{
	// suelo
	{
		bool prevOnGround = onGround;
		Vec3 pushout, normal;
		int floor;
		onGround = GroundCheck(pushout, normal, floor);
		if (onGround)
			Position += pushout;

		if (tGroundSnapCooldown <= 0 && prevOnGround && !onGround)
		{
			RayHit hit;
			if (World::SolidRayCast(Position, -Vec3::UnitZ(), 5, hit) && FloorNormalCheck(hit.normal))
			{
				Position = hit.point;
				onGround = GroundCheck(pushout, normal, floor);
			}
		}

		if (onGround)
		{
			autoJump = false;
			groundNormal = normal;
			tCoyote = CoyoteTime;
			coyoteZ = Position.z;
			if (tDashResetCooldown <= 0)
				RefillDash();
		}
		else
			groundNormal = Vec3::UnitZ();

		if (!prevOnGround && onGround)
		{
			float t = Calc::ClampedMap(previousVelocity.z, 0, MaxFall);
			ModelScale = Lerp(Vec3::One(), Vec3(1.4f, 1.4f, .6f), t);
			Audio::Play("sfx_land", Position);
			for (int i = 0; i < 16; i++)
			{
				Vec2 angle = Calc::AngleToVector((i / 16.0f) * TAU);
				Vec3 at = Position + Vec3(angle, 0) * 4;
				Vec3 vel = (tPlatformVelocityStorage > 0 ? platformVelocity : Vec3()) + Vec3(angle, 0) * 50;
				Actors::AddDust(at, vel);
			}
		}
	}

	// origen de la cámara
	{
		float ZPad = state == State::Climbing ? 0 : 8;
		cameraOriginPos.x = Position.x;
		cameraOriginPos.y = Position.y;

		float targetZ;
		if (onGround) targetZ = Position.z;
		else if (Position.z < cameraOriginPos.z) targetZ = Position.z;
		else if (Position.z > cameraOriginPos.z + ZPad) targetZ = Position.z - ZPad;
		else targetZ = cameraOriginPos.z;

		if (cameraOriginPos.z != targetZ)
			cameraOriginPos.z += (targetZ - cameraOriginPos.z) * (1 - powf(.001f, dt));
	}

	// posición de la cámara
	{
		Vec3 lookAt, cameraPos;
		bool snap;
		if (cameraOverride)
		{
			lookAt = cameraOverrideLookAt;
			cameraPos = cameraOverridePos;
		}
		else
			GetCameraTarget(lookAt, cameraPos, snap);
		Game::Camera.Position += (cameraPos - Game::Camera.Position) * (1 - powf(0.01f, dt));
		Game::Camera.LookAt = lookAt;
		float targetFOV = Calc::ClampedMap(velocity.XY().Length(), MaxSpeed * 1.2f, 120, 1, 1.2f);
		Game::Camera.FOVMultiplier = Calc::Approach(Game::Camera.FOVMultiplier, targetFOV, dt / 4);
	}

	// modelo
	{
		Calc::ApproachRef(ModelScale.x, 1, dt / .8f);
		Calc::ApproachRef(ModelScale.y, 1, dt / .8f);
		Calc::ApproachRef(ModelScale.z, 1, dt / .8f);

		Facing = Calc::AngleToVector(Calc::AngleApproach(Facing.Angle(), targetFacing.Angle(), TAU * 2 * dt));

		Model->Update(dt);
		Model->Transform = Mat4::Scale(ModelScale * 3);

		if (state != State::Feather && state != State::FeatherStart)
		{
			u32 color;
			if (tDashResetFlash > 0) color = CRefillFlash;
			else if (dashes == 1) color = CNormal;
			else if (dashes == 0) color = CNoDash;
			else color = CTwoDashes;
			SetHairColor(color);
		}
	}

	// estelas
	for (auto& t : trails)
		if (t.percent < 1)
			t.percent += dt / 0.5f;

	// pelo: sale del hueso "Head" (Head.ModelMatrix * BaseTranslation * Model.Transform * Matrix)
	{
		static int head = -2;
		if (head == -2) head = Model->Data->JointIndex("Head");
		Mat4 hairMatrix = Mat4::Identity();
		if (head >= 0)
			hairMatrix = Mat4::Translate(Position) * Mat4::RotateZ(Facing.Angle() + PI / 2) * Model->Transform *
				Mat4::RotateX(PI / 2) * Model->JointMatrix(head);
		HairModel.Forward = -Vec3(Facing, 0);
		HairModel.Squish = ModelScale;
		HairModel.Grounded = onGround;
		HairModel.Update(dt, hairMatrix);
	}
}

Mat4 Player::ModelMatrix() const
{
	// v * piel * BaseTranslation(RotX 90°) * Model.Transform * Actor.Matrix
	return Mat4::Translate(Position) * Mat4::RotateZ(Facing.Angle() + PI / 2) * Model->Transform * Mat4::RotateX(PI / 2);
}

// ---------------------------------------------------------------- cámara (original: GetCameraTarget)
void Player::GetCameraTarget(Vec3& cameraLookAt, Vec3& cameraPosition, bool& snapRequested)
{
	snapRequested = false;
	cameraLookAt = cameraOriginPos;
	cameraPosition = cameraLookAt
		- cameraTargetForward * Calc::Lerp3(30, 60, 110, 110, cameraTargetDistance)
		+ Vec3::UnitZ() * Calc::Lerp3(1, 30, 80, 180, cameraTargetDistance);
	cameraLookAt += Vec3::UnitZ() * 12;

	const Volume* fixedCamera = World::OverlapsVolume(VolumeKind::FixedCamera, SolidWaistTestPos());
	if (fixedCamera && (cameraLookAt - fixedCamera->Center()).Length() > 5)
	{
		cameraPosition = fixedCamera->extra;
		cameraTargetForward = Vec3((cameraLookAt.XY() - cameraPosition.XY()).Normalized(), 0);
		snapRequested = true;
	}
	else
	{
		Vec3 from = cameraLookAt;
		Vec3 to = cameraPosition;
		Vec3 normal = (to - from).Normalized();
		float distance = (to - from).Length();
		if (distance > Game::Camera.NearPlane + 1)
			distance -= Game::Camera.NearPlane;

		RayHit hit;
		if (World::SolidRayCast(from, normal, distance, hit, false, true))
		{
			if ((hit.intersections % 2) == 1)
			{
				snapRequested = true;
				cameraPosition = hit.point;
			}
		}
		if (World::SolidRayCast(cameraPosition, Vec3::UnitZ(), 5, hit, true, true))
			cameraPosition = hit.point - Vec3::UnitZ() * 5;
	}
}

// ---------------------------------------------------------------- utilidades (original: Various Methods)
Vec2 Player::RelativeMoveInput()
{
	if (Controls::Move == Vec2())
		return Vec2();

	Vec2 forward, side;
	Vec2 cameraForward = (Game::Camera.LookAt - Game::Camera.Position).Normalized().XY();
	if (cameraForward.x == 0 && cameraForward.y == 0)
		forward = targetFacing;
	else
		forward = cameraForward.Normalized();
	side = Vec2(-forward.y, forward.x);   // Matrix3x2.CreateRotation(PI / 2)

	Vec2 input = -Controls::Move.Normalized();
	if (Dot(input, Vec2::UnitY()) >= .985f)
		input = Vec2::UnitY();

	return forward * input.y + side * input.x;
}

void Player::SetHairColor(u32 color)
{
	for (size_t i = 0; i < Model->Data->materials.size(); i++)
		if (!strcmp(Model->Data->materials[i].name, "Hair"))
		{
			Model->MaterialColors[i] = color;
			Model->MaterialEffects[i] = 0;
		}
	HairModel.Color = color;
	HairModel.Nodes = InFeatherState() ? 18 : (dashes >= 2 ? 16 : 10);
}

void Player::SweepTestMove(Vec3 delta, bool resolveImpact)
{
	if (delta.LengthSquared() <= 0)
		return;

	float remaining = delta.Length();
	const float stepSize = 2.0f;
	Vec3 stepNormal = delta / remaining;

	while (remaining > 0)
	{
		float step = fminf(remaining, stepSize);
		remaining -= step;
		Position += stepNormal * step;
		if (Popout(resolveImpact))
			resolveImpact = false;
	}
}

bool Player::Popout(bool resolveImpact)
{
	Vec3 pushout, normal;
	int floor;
	if (GroundCheck(pushout, normal, floor))
	{
		Position += pushout;
		if (resolveImpact)
			velocity.z = fmaxf(velocity.z, 0);
	}
	else if (CeilingCheck(pushout))
	{
		Position += pushout;
		if (resolveImpact)
			velocity.z = fminf(velocity.z, 0);
	}

	WallHit hit;
	if (World::SolidWallCheckNearest(SolidWaistTestPos(), WallPushoutDist, hit) ||
		World::SolidWallCheckNearest(SolidHeadTestPos(), WallPushoutDist, hit))
	{
		int actor = World::solids[hit.solid].actor;
		// rebote contra paredes durante la pluma
		if (resolveImpact && state == State::Feather && tFeatherWallBumpCooldown <= 0 && !(Controls::Climb.Down() && TryClimb()))
		{
			Position += hit.pushout;
			Vec2 n = hit.normal.XY().Normalized();
			Vec2 v = velocity.XY();
			velocity = withXY(velocity, v - n * (2 * Dot(v, n)));   // Vec2.Reflect
			tFeatherWallBumpCooldown = 0.50f;
			Audio::Play("sfx_feather_state_bump_wall", Position);
		}
		// bloque rompible
		else if (resolveImpact && actor >= 0 && Actors::blocks[actor].kind == Block::Break &&
			Actors::blocks[actor].alive && velocity.XY().Length() > 90)
		{
			BreakBlock(actor, velocity.Normalized());
		}
		else
		{
			Position += hit.pushout;
			if (resolveImpact)
			{
				float dot = fminf(0.0f, Dot(velocity.Normalized(), hit.normal));
				velocity -= hit.normal * velocity.Length() * dot;
			}
		}
		return true;
	}
	return false;
}

void Player::BreakBlock(int blockIndex, Vec3 direction)
{
	Game::HitStun = 0.1f;
	Block& block = Actors::blocks[blockIndex];
	block.Shatter(direction);
	if (block.BouncesPlayer())
	{
		velocity.x = -velocity.x * 0.80f;
		velocity.y = -velocity.y * 0.80f;
		velocity.z = 100;
		SetState(State::Normal);
		CancelGroundSnap();
	}
}

// ---------------------------------------------------------------- plataformas (IRidePlatforms)
void Player::RidingPlatformSetVelocity(Vec3 value)
{
	if (value == Vec3())
		return;
	if (tPlatformVelocityStorage < 0 || value.z >= velocity.z
		|| value.XY().LengthSquared() + .1f >= velocity.XY().LengthSquared()
		|| (value.XY() != Vec2() && Dot(value.XY().Normalized(), velocity.XY().Normalized()) < .5f))
	{
		platformVelocity = value;
		tPlatformVelocityStorage = .1f;
	}
}

bool Player::RidingPlatformCheck(int blockIndex)
{
	if (climbingWallActor >= 0 && World::solids[climbingWallActor].actor == blockIndex)
		return true;
	// WorldBounds.Inflate(10).Intersects(platform.WorldBounds): el jugador ocupa (-5,-5,5)..(5,5,15)
	const Block& b = Actors::blocks[blockIndex];
	Vec3 pmin = Position + Vec3(-15, -15, -5), pmax = Position + Vec3(15, 15, 25);
	Vec3 bmin = b.position + b.LocalMin(), bmax = b.position + b.LocalMax();
	if (pmax.x < bmin.x || pmin.x > bmax.x || pmax.y < bmin.y || pmin.y > bmax.y || pmax.z < bmin.z || pmin.z > bmax.z)
		return false;
	Vec3 pushout, normal;
	int floor;
	return GroundCheck(pushout, normal, floor) && floor >= 0 && World::solids[floor].actor == blockIndex;
}

void Player::RidingPlatformMoved(Vec3 delta)
{
	Vec3 was = Position;
	SweepTestMove(delta, false);
	Vec3 newDelta = Position - was;
	climbCornerFrom += newDelta;
	climbCornerTo += newDelta;
}

// ---------------------------------------------------------------- Spring / Feather (recogibles)
void Player::Spring(Vec3 springPosition)
{
	SetState(State::Normal);
	Position.z = Calc::Approach(Position.z, springPosition.z + 3, 2);
	Vec2 posXY = Calc::Approach(Position.XY(), springPosition.XY(), 4);
	Position = withXY(Position, posXY);
	holdJumpSpeed = velocity.z = SpringJumpSpeed;
	tHoldJump = SpringJumpHoldTime;
	tCoyote = 0;
	autoJump = true;
	Vec2 velXY = Calc::Approach(velocity.XY(), Vec2(), 30);
	velocity = withXY(velocity, velXY);
	dashes = dashes > 1 ? dashes : 1;
	CancelGroundSnap();
}

void Player::FeatherGet(Vec3 featherPosition)
{
	Audio::Play("sfx_dashcrystal", Position);
	Game::HitStun = 0.05f;
	if (state == State::Feather)
	{
		tFeather = FeatherDuration;
		featherZ = featherPosition.z - 2;
		Audio::Play("sfx_feather_renew", Position);
	}
	else
	{
		SetState(State::FeatherStart);
		featherZ = featherPosition.z - 2;
		dashes = dashes > 1 ? dashes : 1;
		Audio::Play("sfx_feather_get", Position);
	}
}

void Player::HandleFeatherZ()
{
	Calc::ApproachRef(velocity.z, (featherZ - Position.z) * 40, 600 * dt);
}

void Player::StFeatherStartEnter()
{
	tFeatherStart = FeatherStartTime;
}

void Player::StFeatherStartUpdate()
{
	Vec2 input = RelativeMoveInput();
	if (input != Vec2())
		targetFacing = input.Normalized();

	SetHairColor(CFeather);
	HandleFeatherZ();

	tFeatherStart -= dt;
	if (tFeatherStart <= 0)
	{
		SetState(State::Feather);
		return;
	}

	Vec2 velXY = Calc::Approach(velocity.XY(), Vec2(), 200 * dt);
	velocity = withXY(velocity, velXY);

	if (dashes > 0 && tDashCooldown <= 0 && Controls::Dash.ConsumePress())
	{
		SetState(State::Dashing);
		dashes--;
		return;
	}
}

void Player::StFeatherEnter()
{
	velocity = withXY(velocity, targetFacing * FeatherStartSpeed);
	tFeather = FeatherDuration;
	HairModel.Roundness = 1;
	DrawModel = false;
	tFeatherWallBumpCooldown = 0;
	featherPlayedEndWarn = false;
	sfxFeather = Audio::Play("sfx_feather_state_active_loop", Position);
}

void Player::StFeatherExit()
{
	HairModel.Roundness = 0;
	DrawModel = true;
	Audio::Stop(sfxFeather);
	sfxFeather = 0;
}

void Player::StFeatherUpdate()
{
	const float EndWarningTime = 0.8f;
	// Time.BetweenInterval(.1): parpadeo al final
	bool between = fmod(Game::Elapsed, 0.2) < 0.1;
	if (tFeather > EndWarningTime || between)
		SetHairColor(CFeather);
	else if (dashes == 2)
		SetHairColor(CTwoDashes);
	else
		SetHairColor(CNormal);

	HandleFeatherZ();

	Vec2 velXY = velocity.XY();
	Vec2 input = RelativeMoveInput();
	if (input != Vec2()) input = input.Normalized();
	else input = targetFacing;

	velXY = Calc::RotateToward(velXY, input * FeatherFlySpeed, FeatherTurnSpeed * dt, FeatherAccel * dt);
	targetFacing = velXY.Normalized();
	velocity = withXY(velocity, velXY);

	tFeather -= dt;
	tFeatherWallBumpCooldown -= dt;
	Audio::SetPosition(sfxFeather, Position);

	if (tFeather <= EndWarningTime && !featherPlayedEndWarn)
	{
		featherPlayedEndWarn = true;
		Audio::Play("sfx_feather_state_end_warning", Position);
	}

	if (tFeather <= 0)
	{
		SetState(State::Normal);
		velocity.x *= FeatherExitXYMult;
		velocity.y *= FeatherExitXYMult;
		holdJumpSpeed = velocity.z = FeatherExitZSpeed;
		tHoldJump = .1f;
		autoJump = true;
		Audio::Play("sfx_feather_state_end", Position);
		return;
	}

	if (dashes > 0 && tDashCooldown <= 0 && Controls::Dash.ConsumePress())
	{
		SetState(State::Dashing);
		dashes--;
		return;
	}

	if (Controls::Climb.Down() && TryClimb())
	{
		SetState(State::Climbing);
		return;
	}
}

void Player::Jump()
{
	Position.z = coyoteZ;
	holdJumpSpeed = velocity.z = JumpSpeed;
	tHoldJump = JumpHoldTime;
	tCoyote = 0;
	autoJump = false;

	Vec2 input = RelativeMoveInput();
	if (input != Vec2())
	{
		input = input.Normalized();
		targetFacing = input;
		velocity += Vec3(input * JumpXYBoost, 0);
	}

	AddPlatformVelocity(true);
	CancelGroundSnap();
	ModelScale = Vec3(.6f, .6f, 1.4f);
	Audio::Play("sfx_jump", Position);
}

void Player::WallJump()
{
	holdJumpSpeed = velocity.z = JumpSpeed;
	tHoldJump = JumpHoldTime;
	autoJump = false;

	Vec2 velXY = targetFacing * WallJumpXYSpeed;
	velocity = withXY(velocity, velXY);

	AddPlatformVelocity(false);
	CancelGroundSnap();
	ModelScale = Vec3(.6f, .6f, 1.4f);
	Audio::Play("sfx_jump_wall", Position);
}

void Player::SkidJump()
{
	Position.z = coyoteZ;
	holdJumpSpeed = velocity.z = SkidJumpSpeed;
	tHoldJump = SkidJumpHoldTime;
	tCoyote = 0;

	Vec2 velXY = targetFacing * SkidJumpXYSpeed;
	velocity = withXY(velocity, velXY);

	AddPlatformVelocity(false);
	CancelGroundSnap();
	ModelScale = Vec3(.6f, .6f, 1.4f);
	Audio::Play("sfx_jump", Position);
	Audio::Play("sfx_jump_skid", Position);
	for (int i = 0; i < 16; i++)
	{
		Vec3 dir(Calc::AngleToVector((i / 16.0f) * TAU), 0);
		Actors::AddDust(Position + dir * 8, Vec3(velocity.XY() * 0.5f, 10) - dir * 50, Color4::FromRGB(0x666666));
	}
}

void Player::DashJump()
{
	Position.z = coyoteZ;
	velocity.z = DashJumpSpeed;
	holdJumpSpeed = DashJumpHoldSpeed;
	tHoldJump = DashJumpHoldTime;
	tCoyote = 0;
	autoJump = false;
	dashes = 1;

	Vec2 input = RelativeMoveInput();
	if (input != Vec2())
	{
		input = input.Normalized();
		targetFacing = input;
		velocity += Vec3(input * DashJumpXYBoost, 0);
	}

	AddPlatformVelocity(false);
	CancelGroundSnap();
	ModelScale = Vec3(.6f, .6f, 1.4f);
	Audio::Play("sfx_jump", Position);
	Audio::Play("sfx_jump_superslide", Position);
}

void Player::AddPlatformVelocity(bool playSound)
{
	if (tPlatformVelocityStorage > 0)
	{
		Vec3 add = platformVelocity;
		add.z = Calc::Clamp(add.z, 0, 180);
		if (add.XY().LengthSquared() > 300 * 300)
			add = withXY(add, add.XY().Normalized() * 300);
		velocity += add;
		platformVelocity = Vec3();
		tPlatformVelocityStorage = 0;
		if (playSound && (add.z >= 10 || add.XY().Length() > 10))
			Audio::Play("sfx_jump_assisted", Position);
	}
}

void Player::Kill()
{
	SetState(State::Dead);
	storedCameraForward = cameraTargetForward;
	storedCameraDistance = cameraTargetDistance;
	Save.record.deaths++;
	Dead = true;
}

bool Player::ClimbCheckAt(Vec3 offset, WallHit& hit)
{
	Vec2 rel = RelativeMoveInput();
	if (World::SolidWallCheckClosestToNormal(SolidWaistTestPos() + offset, ClimbCheckDist, -Vec3(targetFacing, 0), hit)
		&& (rel == Vec2() || Dot(hit.normal.XY().Normalized(), rel) <= -0.5f)
		&& ClimbNormalCheck(hit.normal))
		return true;
	return false;
}

bool Player::TryClimb()
{
	WallHit wall;
	bool result = ClimbCheckAt(Vec3(), wall);

	if (!result && velocity.z > 0 && !onGround && state != State::Climbing)
		result = ClimbCheckAt(Vec3::UnitZ() * 4, wall);

	if (result)
	{
		climbingWallNormal = wall.normal;
		climbingWallActor = wall.solid;
		Vec3 moveTo = wall.point + (Position - SolidWaistTestPos()) + wall.normal * WallPushoutDist;
		SweepTestMove(moveTo - Position, false);
		targetFacing = -climbingWallNormal.XY().Normalized();
		return true;
	}
	climbingWallActor = -1;
	climbingWallNormal = Vec3();
	return false;
}

bool Player::WallJumpCheck()
{
	WallHit hit;
	if (Controls::Jump.Pressed()
		&& World::SolidWallCheckClosestToNormal(SolidWaistTestPos(), ClimbCheckDist, -Vec3(targetFacing, 0), hit))
	{
		Controls::Jump.ConsumePress();
		Position += hit.pushout * (WallPushoutDist / ClimbCheckDist);
		targetFacing = hit.normal.XY().Normalized();
		return true;
	}
	return false;
}

bool Player::GroundCheck(Vec3& pushout, Vec3& normal, int& floor)
{
	pushout = Vec3();
	floor = -1;
	normal = Vec3::UnitZ();
	RayHit hit;
	if (World::SolidRayCast(Position + Vec3::UnitZ() * 5, -Vec3::UnitZ(), 5.01f, hit))
	{
		pushout = hit.point - Position;
		floor = hit.solid;
		normal = hit.normal;
		return true;
	}
	return false;
}

bool Player::CeilingCheck(Vec3& pushout)
{
	const float Height = 12;
	pushout = Vec3();
	RayHit hit;
	if (World::SolidRayCast(Position + Vec3::UnitZ(), Vec3::UnitZ(), Height - 1, hit))
	{
		pushout = hit.point - Vec3::UnitZ() * Height - Position;
		return true;
	}
	return false;
}

// ---------------------------------------------------------------- Normal
void Player::StNormalEnter()
{
	tHoldJump = 0;
	tFootstep = FootstepInterval;
}

void Player::StNormalExit()
{
	tHoldJump = 0;
	tNoMove = 0;
	autoJump = false;
	Model->Rate = 1;
}

void Player::StNormalUpdate()
{
	// hablar con personajes (NPC)
	if (onGround)
		for (Npc& npc : Cutscenes::npcs)
			if (npc.model && npc.interactEnabled)
			{
				if ((Position - npc.position).LengthSquared() < npc.interactRadius * npc.interactRadius &&
					Dot((npc.position - Position).XY(), targetFacing) > 0 &&
					fabsf(npc.position.z - Position.z) < 2)
				{
					npc.isPlayerOver = true;
					if (Controls::Dash.ConsumePress())
					{
						Cutscenes::Interact(npc);
						return;
					}
					break;
				}
			}

	// movimiento
	{
		Vec2 velXY = velocity.XY();

		if (Controls::Move == Vec2() || tNoMove > 0)
		{
			float fric = Friction;
			if (!onGround)
				fric *= AirFrictionMult;
			velXY = Calc::Approach(velXY, Vec2(), fric * dt);
		}
		else if (onGround)
		{
			float max = MaxSpeed;

			// velocidad máxima según la pendiente
			if (groundNormal != Vec3::UnitZ())
			{
				float slopeDot = 1 - Calc::Clamp(Dot(groundNormal, Vec3::UnitZ()), 0, 1);
				slopeDot *= Dot(groundNormal.XY().Normalized(), targetFacing) * 2;
				max += max * slopeDot;
			}

			float trueMax = max;
			{
				float mag = Calc::ClampedMap(Controls::Move.Length(), .4f, .92f, .3f, 1);
				max *= mag;
			}

			Vec2 input = RelativeMoveInput();

			// alejarse suavemente de los bordes (original: "Please don't look at this code")
			{
				const float d = 4;
				RayHit hit;
				if (input != Vec2() &&
					!World::SolidRayCast(Position + Vec3(input, 1) * d, -Vec3::UnitZ(), 8, hit) &&
					!World::SolidRayCast(Position + Vec3(0, 0, d), Vec3(input, 0), d, hit))
				{
					Vec2 left = Calc::AngleToVector(Calc::Angle(input) + 0.3f);
					Vec2 right = Calc::AngleToVector(Calc::Angle(input) - 0.3f);
					int count = 0;

					if (World::SolidRayCast(Position + Vec3(left, 1) * d, -Vec3::UnitZ(), 8, hit))
					{
						while (World::SolidRayCast(Position + Vec3(left, 1) * d, -Vec3::UnitZ(), 8, hit) && count++ < 10)
							left = Calc::AngleToVector(Calc::Angle(left) - 0.1f);
						input = Calc::AngleToVector(Calc::Angle(left) + 0.1f);
					}
					else if (World::SolidRayCast(Position + Vec3(right, 1) * d, -Vec3::UnitZ(), 8, hit))
					{
						while (World::SolidRayCast(Position + Vec3(right, 1) * d, -Vec3::UnitZ(), 8, hit) && count++ < 10)
							right = Calc::AngleToVector(Calc::Angle(right) + 0.1f);
						input = Calc::AngleToVector(Calc::Angle(right) - 0.1f);
					}
				}
			}

			float accel;
			if (velXY.LengthSquared() >= trueMax * trueMax && Dot(input, velXY) >= .7f)
				accel = PastMaxDeccel;
			else
				accel = Acceleration;

			if (velXY.LengthSquared() >= RotateThreshold * RotateThreshold)
			{
				if (Dot(input, velXY.Normalized()) <= SkidDotThreshold)
				{
					Facing = targetFacing = input;
					SetState(State::Skidding);
					return;
				}
				else
				{
					float rotate = velXY.LengthSquared() > trueMax * trueMax ? RotateSpeedAboveMax : RotateSpeed;
					targetFacing = Calc::RotateToward(targetFacing, input, rotate * dt, 0);
					velXY = targetFacing * Calc::Approach(velXY.Length(), max, accel * dt);
				}
			}
			else
			{
				velXY = Calc::Approach(velXY, input * max, accel * dt);
				targetFacing = input.Normalized();
			}
		}
		else
		{
			Vec2 rel = RelativeMoveInput();
			float accel;
			if (velXY.LengthSquared() >= MaxSpeed * MaxSpeed && Dot(rel.Normalized(), velXY.Normalized()) >= .7f)
			{
				accel = PastMaxDeccel;
				float dot = Dot(rel.Normalized(), targetFacing);
				accel *= Calc::ClampedMap(dot, -1, 1, AirAccelMultMax, AirAccelMultMin);
			}
			else
			{
				accel = Acceleration;
				float dot = Dot(rel.Normalized(), targetFacing);
				accel *= Calc::ClampedMap(dot, -1, 1, AirAccelMultMin, AirAccelMultMax);
			}
			velXY = Calc::Approach(velXY, rel * MaxSpeed, accel * dt);
		}

		velocity = withXY(velocity, velXY);
	}

	// pasos (el banco no trae la muestra de pasos: solo el polvo)
	if (onGround && velocity.XY().Length() > 10)
	{
		tFootstep -= dt * Model->Rate;
		if (tFootstep <= 0)
			tFootstep = FootstepInterval;
		if (Game::OnInterval(0.05f))
		{
			Vec3 at = Position + Vec3(Actors::Rng(-3, 3), Actors::Rng(-3, 3), 0);
			Actors::AddDust(at, tPlatformVelocityStorage > 0 ? platformVelocity : Vec3());
		}
	}
	else
		tFootstep = FootstepInterval;

	// empezar a escalar
	if (Controls::Climb.Down() && tClimbCooldown <= 0 && TryClimb())
	{
		SetState(State::Climbing);
		return;
	}

	// dash
	if (TryDash())
		return;

	// salto y gravedad
	if (tCoyote > 0 && Controls::Jump.ConsumePress())
		Jump();
	else if (WallJumpCheck())
		WallJump();
	else
	{
		if (tHoldJump > 0 && (autoJump || Controls::Jump.Down()))
		{
			if (velocity.z < holdJumpSpeed)
				velocity.z = holdJumpSpeed;
		}
		else
		{
			float mult;
			if ((Controls::Jump.Down() || autoJump) && fabsf(velocity.z) < HalfGravThreshold)
				mult = .5f;
			else
			{
				mult = 1;
				autoJump = false;
			}
			Calc::ApproachRef(velocity.z, MaxFall, Gravity * mult * dt);
			tHoldJump = 0;
		}
	}

	// animaciones
	if (onGround)
	{
		Vec2 velXY = velocity.XY();
		if (velXY.LengthSquared() > 1)
		{
			Model->Play("Run");
			Model->Rate = Calc::ClampedMap(velXY.Length(), 0, MaxSpeed * 2, .1f, 3);
		}
		else
		{
			Model->Play("Idle");
			Model->Rate = 1;
		}
	}
	else
	{
		// vuelve al primer cuadro de Run una y otra vez
		Model->Clear();
		Model->Play("Run");
	}
}

// ---------------------------------------------------------------- Dashing
bool Player::TryDash()
{
	if (dashes > 0 && tDashCooldown <= 0 && Controls::Dash.ConsumePress())
	{
		dashes--;
		SetState(State::Dashing);
		return true;
	}
	return false;
}

void Player::StDashingEnter()
{

	Vec2 rel = RelativeMoveInput();
	if (rel != Vec2())
		targetFacing = rel;
	Facing = targetFacing;

	dashedOnGround = onGround;
	SetDashSpeed(targetFacing);
	autoJump = true;

	tDash = DashTime;
	tDashResetCooldown = DashResetCooldown;
	tNoDashJump = .1f;

	Game::HitStun = .02f;
	Audio::Play(dashes <= 1 ? "sfx_dash_red" : "sfx_dash_pink", Position);
	lastDashHairColor = dashes <= 0 ? CNoDash : CNormal;
	dashTrailsCreated = 0;
}

void Player::StDashingExit()
{
	tDashCooldown = DashCooldown;
	CreateDashTrail();
}

// Player.CreateDashtTrail: copia la pose, el pelo y la posición; se desvanece en 0.5 s
void Player::CreateDashTrail()
{
	Trail* trail = nullptr;
	for (auto& t : trails)
		if (t.percent >= 1) { trail = &t; break; }
	if (!trail)
	{
		if (trails.size() >= 4) return;
		trails.push_back(Trail());
		trail = &trails.back();
		trail->model = new SkinnedModel(Model->Data);
	}
	trail->model->CopyPose(*Model);
	trail->hair = HairModel;
	trail->percent = 0;
	trail->world = ModelMatrix();
	trail->color = lastDashHairColor;
}

void Player::StDashingUpdate()
{
	Model->Play("Dash");

	tDash -= dt;
	if (tDash <= 0)
	{
		if (!onGround)
			velocity *= DashEndSpeedMult;
		SetState(State::Normal);
		return;
	}

	if (dashTrailsCreated <= 0 || (dashTrailsCreated == 1 && tDash <= DashTime * .5f))
	{
		dashTrailsCreated++;
		CreateDashTrail();
	}

	if (Controls::Move != Vec2() && Dot(Controls::Move, targetFacing) >= -.2f)
	{
		targetFacing = Calc::RotateToward(targetFacing, RelativeMoveInput(), DashRotateSpeed * dt, 0);
		SetDashSpeed(targetFacing);
	}

	if (tNoDashJump > 0)
		tNoDashJump -= dt;

	if (dashedOnGround && tCoyote > 0 && tNoDashJump <= 0 && Controls::Jump.ConsumePress())
	{
		SetState(State::Normal);
		DashJump();
		return;
	}
}

bool Player::RefillDash(int amount)
{
	if (dashes < amount)
	{
		dashes = amount;
		tDashResetFlash = .05f;
		return true;
	}
	return false;
}

void Player::SetDashSpeed(Vec2 dir)
{
	if (dashedOnGround)
		velocity = Vec3(dir, 0) * DashSpeed;
	else
		velocity = Vec3(dir, .4f).Normalized() * DashSpeed;
}

// ---------------------------------------------------------------- Skidding
void Player::StSkiddingEnter()
{
	tNoSkidJump = .1f;
	Model->Play("Skid", true);
	Audio::Play("sfx_skid", Position);
	for (int i = 0; i < 5; i++)
		Actors::AddDust(Position + Vec3(targetFacing, 0) * (float)i, Vec3(-targetFacing, 0.0f).Normalized() * 50, Color4::FromRGB(0x666666));
}

void Player::StSkiddingExit()
{
	Model->Play("Idle", true);
}

void Player::StSkiddingUpdate()
{
	if (tNoSkidJump > 0)
		tNoSkidJump -= dt;

	if (TryDash())
		return;

	Vec2 rel = RelativeMoveInput();
	if (rel.LengthSquared() < .2f * .2f || Dot(rel, targetFacing) < .7f || !onGround)
	{
		SetState(State::Normal);
		return;
	}

	Vec2 velXY = velocity.XY();
	if (tNoSkidJump <= 0 && Controls::Jump.ConsumePress())
	{
		SetState(State::Normal);
		SkidJump();
		return;
	}

	bool dotMatches = Dot(velXY.Normalized(), targetFacing) >= .7f;
	float accel = dotMatches ? SkiddingAccel : SkiddingStartAccel;
	velXY = Calc::Approach(velXY, rel * MaxSpeed, accel * dt);
	velocity = withXY(velocity, velXY);

	if (dotMatches && velXY.LengthSquared() >= EndSkidSpeed * EndSkidSpeed)
	{
		SetState(State::Normal);
		return;
	}
}

// ---------------------------------------------------------------- Climbing
void Player::StClimbingEnter()
{
	Model->Play("Climb.Idle", true);
	Model->Rate = 1.8f;
	velocity = Vec3();
	climbCornerEase = 0;
	climbInputSign = 1;
	Audio::Play("sfx_grab", Position);
}

void Player::StClimbingExit()
{
	Model->Play("Idle");
	Model->Rate = 1.0f;
	climbingWallActor = -1;
	Audio::Stop(sfxWallSlide);
	sfxWallSlide = 0;
}

void Player::StClimbingUpdate()
{
	if (!Controls::Climb.Down())
	{
		Audio::Play("sfx_let_go", Position);
		SetState(State::Normal);
		return;
	}

	if (Controls::Jump.ConsumePress())
	{
		SetState(State::Normal);
		targetFacing = -targetFacing;
		WallJump();
		return;
	}

	if (dashes > 0 && tDashCooldown <= 0 && Controls::Dash.ConsumePress())
	{
		SetState(State::Dashing);
		dashes--;
		return;
	}

	CancelGroundSnap();

	Vec3 forward(targetFacing, 0);
	Vec3 wallUp = upwardPerpendicularNormal(climbingWallNormal);
	Vec3 wallRight = rotateAround(wallUp, climbingWallNormal, -PI / 2);
	bool forceCorner = false;
	bool wallSlideSoundEnabled = false;

	// el signo de la entrada solo cambia cuando dejamos de movernos
	if (fabsf(Controls::Move.x) < .5f)
		climbInputSign = (Dot(targetFacing, cameraTargetForward.XY().Normalized()) < -.4f) ? -1 : 1;

	Vec2 inputTranslated = Controls::Move;
	inputTranslated.x *= climbInputSign;

	if (climbCornerEase <= 0)
	{
		Vec3 side = wallRight * inputTranslated.x;
		Vec3 up = -wallUp * inputTranslated.y;
		Vec3 move = side + up;

		Vec3 gp, gn;
		int gf;
		if (move.z < 0 && GroundCheck(gp, gn, gf))
			move.z = 0;

		// no trepar bordes hacia pinchos
		if (move.z > 0 && World::OverlapsVolume(VolumeKind::Spike, Position + Vec3::UnitZ() * ClimbCheckDist + forward * (ClimbCheckDist + 1)))
			move.z = 0;
		if (World::OverlapsVolume(VolumeKind::Spike, SolidWaistTestPos() + side + forward * (ClimbCheckDist + 1)))
			move -= side;

		if (fabsf(move.x) < 0.1f) move.x = 0;
		if (fabsf(move.y) < 0.1f) move.y = 0;
		if (fabsf(move.z) < 0.1f) move.z = 0;

		if (move != Vec3())
			SweepTestMove(move * ClimbSpeed * dt, false);

		if (fabsf(inputTranslated.x) < 0.25f && inputTranslated.y >= 0)
		{
			if (inputTranslated.y > 0 && !onGround)
			{
				wallSlideSoundEnabled = true;
				if (Game::OnInterval(0.05f))
				{
					Vec3 at = Position + wallUp * 5 + Vec3(Facing, 0) * 2;
					Actors::AddDust(at, tPlatformVelocityStorage > 0 ? platformVelocity : Vec3());
				}
			}
			Model->Play("Climb.Idle");
		}
		else
		{
			Model->Play("Climb.Up");
			if (Game::OnInterval(0.3f))
				Audio::Play("sfx_handhold", Position);
		}
	}
	else
	{
		// esquina: interpolación
		float ease = 1.0f - climbCornerEase;
		velocity = Vec3();
		Position = Lerp(climbCornerFrom, climbCornerTo, ease);
		targetFacing = Calc::AngleToVector(Calc::AngleLerp(climbCornerFacingFrom.Angle(), climbCornerFacingTo.Angle(), ease));
		if (climbCornerCameraValid)
		{
			float angle = Calc::AngleLerp(climbCornerCameraFrom.Angle(), climbCornerCameraTo.Angle(), ease * 0.50f);
			cameraTargetForward = Vec3(Calc::AngleToVector(angle), cameraTargetForward.z);
		}
		Calc::ApproachRef(climbCornerEase, 0, dt / 0.20f);
		return;
	}

	climbCornerFrom = Position;
	climbCornerFacingFrom = targetFacing;
	climbCornerCameraValid = false;

	RayHit hit;
	WallHit wh;
	// esquinas interiores
	if (inputTranslated.x != 0 && World::SolidRayCast(SolidWaistTestPos(), wallRight * inputTranslated.x, ClimbCheckDist, hit))
	{
		Position = hit.point + (Position - SolidWaistTestPos()) + hit.normal * WallPushoutDist;
		targetFacing = -hit.normal.XY();
		climbingWallNormal = hit.normal;
		climbingWallActor = hit.solid;
	}
	// paredes que se inclinan hacia afuera
	else if (World::SolidRayCast(SolidWaistTestPos(), -climbingWallNormal, ClimbCheckDist + 2, hit) && ClimbNormalCheck(hit.normal))
	{
		Position = hit.point + (Position - SolidWaistTestPos()) + hit.normal * WallPushoutDist;
		targetFacing = -hit.normal.XY();
		climbingWallNormal = hit.normal;
		climbingWallActor = hit.solid;
	}
	// esquinas exteriores según la entrada
	else if (inputTranslated.x != 0 &&
		World::SolidRayCast(SolidWaistTestPos() + forward * (ClimbCheckDist + 1) + wallRight * inputTranslated.x,
			wallRight * -inputTranslated.x, ClimbCheckDist * 2, hit) &&
		ClimbNormalCheck(hit.normal))
	{
		Position = hit.point + (Position - SolidWaistTestPos()) + hit.normal * WallPushoutDist;
		targetFacing = -hit.normal.XY();
		climbingWallNormal = hit.normal;
		climbingWallActor = hit.solid;
		climbCornerCameraFrom = cameraTargetForward.XY();
		climbCornerCameraTo = targetFacing;
		climbCornerCameraValid = true;
		Model->Play("Climb.Idle");
		forceCorner = true;
	}
	// saltito al llegar arriba
	else if (inputTranslated.y < 0 && !ClimbCheckAt(Vec3::UnitZ(), wh))
	{
		Audio::Play("sfx_climb_ledge", Position);
		SetState(State::Normal);
		velocity = Vec3(targetFacing * ClimbHopForwardSpeed, ClimbHopUpSpeed);
		tNoMove = ClimbHopNoMoveTime;
		tClimbCooldown = 0.3f;
		autoJump = false;
		AddPlatformVelocity(false);
		return;
	}
	// se suelta
	else if (!TryClimb())
	{
		SetState(State::Normal);
		return;
	}

	// sonido de deslizarse por la pared (Sound en bucle que sigue a Madeline)
	if (wallSlideSoundEnabled)
	{
		if (!sfxWallSlide || !Audio::IsPlaying(sfxWallSlide))
			sfxWallSlide = Audio::Play("sfx_wall_slide", Position);
		Audio::SetPosition(sfxWallSlide, Position);
	}
	else if (sfxWallSlide)
	{
		Audio::Stop(sfxWallSlide);
		sfxWallSlide = 0;
	}

	if (forceCorner || (Position - climbCornerFrom).Length() > 2)
	{
		climbCornerEase = 1.0f;
		climbCornerTo = Position;
		climbCornerFacingTo = targetFacing;
		Position = climbCornerFrom;
		targetFacing = climbCornerFacingFrom;
	}
}

// ---------------------------------------------------------------- StrawbGet
void Player::StrawbGet(Strawberry* s)
{
	if (state != State::StrawbGet)
	{
		Audio::Play("sfx_collect_strawb", s->position);
		lastStrawb = s;
		SetState(State::StrawbGet);
		Position = s->position + Vec3::UnitZ() * -3;
		s->position = Position + Vec3::UnitZ() * 12;
	}
}

void Player::StStrawbGetEnter()
{
	Model->Play("StrawberryGrab");
	velocity = Vec3();
	strawbGetForward = (Game::Camera.Position - Position).XY().Normalized();
	cameraOverride = true;
	cameraOverridePos = Game::Camera.Position;
	cameraOverrideLookAt = Game::Camera.LookAt;
	strawbTimer = 0;
	strawbStage = 0;
}

void Player::StStrawbGetExit()
{
	cameraOverride = false;
	Strawberry* s = lastStrawb;
	lastStrawb = nullptr;
	if (s)
	{
		s->destroyed = true;
		s->collecting = false;
		if (s->hasBubble && state == State::Normal)
			BubbleTo(s->bubbleTo);
	}
}

void Player::StStrawbGetUpdate()
{
	Facing = targetFacing = Calc::AngleToVector(strawbGetForward.Angle() - PI / 7);
	cameraOverridePos = Position + Vec3(strawbGetForward * 50, 40);
	cameraOverrideLookAt = Position + Vec3::UnitZ() * 6;

	// StStrawbGetRoutine: 2.0 s -> se guarda la fresa; 1.2 s más -> Normal
	strawbTimer += dt;
	if (strawbStage == 0 && strawbTimer >= 2.0f)
	{
		strawbStage = 1;
		if (lastStrawb)
		{
			lastStrawb->collected = true;
			Actors::collectedCount++;
			Save.record.strawberries.insert(lastStrawb->sid);   // Save.CurrentRecord.Strawberries.Add
			Save.Write();
		}
	}
	if (strawbStage == 1 && strawbTimer >= 3.2f)
	{
		if (Actors::submap)
		{
			// Save.CurrentRecord.CompletedSubMaps.Add(Map) y Pop al nivel principal
			Save.record.completedSubMaps.insert(Actors::mapName);
			Save.Write();
			PendingRequest = Request::LeaveSubmap;
			strawbStage = 2;
		}
		else
			SetState(State::Normal);
	}
}

// ---------------------------------------------------------------- Respawn / Dead
void Player::StRespawnEnter()
{
	Audio::Play("sfx_revive", Position);
	PointShadowAlpha = 0;
	DrawModel = DrawHair = false;
	drawOrbs = true;
	drawOrbsEase = 1;
}

void Player::StRespawnUpdate()
{
	drawOrbsEase -= dt * 2;
	if (drawOrbsEase <= 0)
		SetState(State::Normal);
}

void Player::StRespawnExit()
{
	PointShadowAlpha = 1;
	DrawModel = DrawHair = true;
	drawOrbs = false;
}

void Player::StDeadEnter()
{
	Audio::Play("sfx_death", Position);
	PointShadowAlpha = 0;
	DrawModel = DrawHair = false;
	drawOrbs = true;
	drawOrbsEase = 0;
}

void Player::StDeadUpdate()
{
	if (drawOrbsEase < 1.0f)
		drawOrbsEase += dt * 2.0f;
	if (drawOrbsEase > 0.30f)
		RequestRespawn = true;
}

void Player::DrawTrails(C3D_Tex* white)
{
	for (auto& t : trails)
	{
		if (t.percent >= 1) continue;
		// "I HATE this alpha fade out" (comentario del original): se desvanece en la segunda mitad
		float alpha = Ease::CubeOut(Calc::ClampedMap(t.percent, 0.5f, 1.0f, 1, 0));
		u32 c = t.color;
		u32 r = (u32)(((c >> 16) & 0xFF) * alpha), g = (u32)(((c >> 8) & 0xFF) * alpha), b = (u32)((c & 0xFF) * alpha);
		u32 abgr = ((u32)(alpha * 255) << 24) | (b << 16) | (g << 8) | r;
		t.model->DrawTinted(t.world, abgr, white);
		Gfx::BindScene();
		t.hair.Color = t.color;
		t.hair.Draw(alpha);
	}
}

// ---------------------------------------------------------------- Bubble / Cassette
void Player::BubbleTo(Vec3 target)
{
	bubbleTo = target;
	Model->Play("StrawberryGrab");
	SetState(State::Bubble);
	PointShadowAlpha = 0;
	Audio::Play("sfx_bubble_in", Position);
}

void Player::EnterCassette(::Cassette* c)
{
	if (state == State::Cassette) return;
	cassette = c;
	SetState(State::Cassette);
	Position = c->position - Vec3::UnitZ() * 3;
	DrawModel = DrawHair = false;
	PointShadowAlpha = 0;
	cameraOverride = true;
	cameraOverridePos = Game::Camera.Position;
	cameraOverrideLookAt = c->position;
	Audio::Play("sfx_cassette_enter", Position);
}
