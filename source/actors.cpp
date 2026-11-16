#include "actors.h"
#include "audio.h"
#include "save.h"
#include "gfx.h"
#include "player.h"
#include "world.h"
#include <stdlib.h>
#include <string.h>

void logf_(const char* fmt, ...);

namespace Actors
{
	std::vector<Strawberry> strawberries;
	std::vector<Block> blocks;
	std::vector<Spring> springs;
	std::vector<Refill> refills;
	std::vector<Feather> feathers;
	std::vector<CheckpointFlag> flags;
	std::vector<Debris> debris;
	std::vector<Dust> dust;
	float snowAmount = 1;
	Vec3 snowDirection(0, 0, -1);

	void AddDust(Vec3 position, Vec3 velocity, Color4 color)
	{
		if (dust.size() >= 160) return;
		dust.push_back({ position, velocity, rand() % 5, color, 0, Rng(0.5f, 1.0f) });
	}

	// Snow.cs: copos en celdas de 100 alrededor de la cámara, con el mismo patrón en cada celda
	void CollectSnow(Vec3 camPos, Vec3 camForward, Vec3 camLeft, Vec3 camUp)
	{
		if (snowAmount <= 0) return;
		static C3D_Tex* circle = Gfx::GetTexture("spr_circle", false);
		const float area = 100;
		const float time = GeneralTimer;
		int budget = 500;   // límite de copos por cuadro para la Old 3DS
		auto mod = [](float x, float m) { return fmodf(fmodf(x, m) + m, m); };
		float ax = floorf((camPos.x - 300) / area) * area, bx = ceilf((camPos.x + 300) / area) * area;
		float ay = floorf((camPos.y - 300) / area) * area, by = ceilf((camPos.y + 300) / area) * area;
		float az = floorf((camPos.z - 200) / area) * area, bz = ceilf(camPos.z / area) * area;
		for (float x = ax; x <= bx; x += area)
		for (float y = ay; y <= by; y += area)
		for (float z = az; z <= bz; z += area)
		{
			Vec3 center = Vec3(x, y, z) + Vec3::One() * (area * 0.5f);
			if (Dot(center - camPos, camForward) <= 0) continue;
			float distZ = Calc::ClampedMap(camPos.z - center.z, 0, 200, 1, 0);
			if (distZ <= 0) continue;
			float d2 = (camPos.XY() - center.XY()).LengthSquared();
			if (d2 > 300 * 300) continue;
			float dist2d = Calc::ClampedMap(sqrtf(d2), 100, 300, 1, 0);
			if (dist2d <= 0) continue;
			float alpha = dist2d * distZ;
			if (alpha < 0.1f || !IsVisible(center, area * 0.87f)) continue;
			int count = (int)(Calc::Lerp(0, 50, dist2d) * snowAmount);
			u32 seed = 12345;
			auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
			Color4 color = Color4() * alpha;
			for (int i = 0; i < count && budget > 0; i++, budget--)
			{
				float ox = rnd() * area, sx = 5 + rnd() * 20;
				float oy = rnd() * area, sy = 5 + rnd() * 20;
				float oz = rnd() * area, sz = 5 + rnd() * 20;
				Vec3 p(x + mod(ox + sx * time * snowDirection.x, area),
				       y + mod(oy + sy * time * snowDirection.y, area),
				       z + mod(oz + sz * time * snowDirection.z, area));
				Sprites::Billboard(circle, p, 0.50f, color, camLeft, camUp);
			}
		}
	}
	ModelData strawberryData, springData, refillData, refillDoubleData, featherData, flagOnData, flagOffData;
	ModelData tapeData, tapeCollectedData, coinData;
	std::vector<Cassette> cassettes;
	std::vector<Coin> coins;
	std::string mapName = "1";
	bool submap = false;
	int beatCounter = 0;

	bool AnyCoinRemaining()
	{
		for (auto& c : coins)
			if (!c.collected) return true;
		return false;
	}

	void OnBeat(int index)
	{
		// CassetteBlock.AudioCallbackEvent: alterna cada dos pulsos
		if (index % 2 != 0) return;
		for (auto& b : blocks)
			if (b.kind == Block::Cassette && b.alive)
			{
				CollisionSolid& s = World::solids[World::actorSolid[b.index]];
				s.collidable = !s.collidable;
			}
	}

	static void freeModel(SkinnedModel*& m) { delete m; m = nullptr; }

	void Clear()
	{
		for (auto& s : strawberries) freeModel(s.model);
		for (auto& s : springs) freeModel(s.model);
		for (auto& r : refills) freeModel(r.model);
		for (auto& f : feathers) freeModel(f.model);
		for (auto& f : flags) { freeModel(f.modelOn); freeModel(f.modelOff); }
		for (auto& c : cassettes) { freeModel(c.model); freeModel(c.collectedModel); }
		for (auto& c : coins) freeModel(c.model);
		strawberries.clear(); blocks.clear(); springs.clear(); refills.clear(); feathers.clear();
		flags.clear(); cassettes.clear(); coins.clear(); debris.clear(); dust.clear();
		collectedCount = 0;
		beatCounter = 0;
	}
	float GeneralTimer = 0;
	std::vector<float> floatRate, floatOffset;
	int collectedCount = 0;
	Player* player = nullptr;
	int currentCheckpoint = 0;
	static bool alwaysVisible(Vec3, float) { return true; }
	bool (*IsVisible)(Vec3 center, float radius) = alwaysVisible;
	void (*OnStrawberryUnlock)(Strawberry& s) = nullptr;

	float Rng(float min, float max) { return min + (max - min) * (rand() / (float)RAND_MAX); }

	void InitFloating(int groups)
	{
		floatRate.assign(groups + 1, 0);
		floatOffset.assign(groups + 1, 0);
		for (int g = 1; g <= groups; g++)
		{
			floatRate[g] = Rng(1, 3);          // FloatingDecoration.Added
			floatOffset[g] = Rng(0, TAU);
		}
	}

	float FloatingOffset(int group)
	{
		if (group <= 0 || group >= (int)floatRate.size()) return 0;
		return sinf(GeneralTimer * floatRate[group] * 0.25f + floatOffset[group]) * 12;
	}

	static SkinnedModel* newModel(const ModelData& d, float effects = 1.0f)
	{
		if (!d.verts) return nullptr;
		SkinnedModel* m = new SkinnedModel(&d);
		for (auto& e : m->MaterialEffects) e = effects;
		return m;
	}

	void LoadModels()
	{
		static bool loaded = false;
		if (!loaded)
		{
			loaded = true;
			strawberryData.Load("strawberry");
			springData.Load("spring_board");
			refillData.Load("refill_gem");
			refillDoubleData.Load("refill_gem_double");
			featherData.Load("feather");
			flagOnData.Load("flag_on");
			flagOffData.Load("flag_off");
			tapeData.Load("tape_1");
			tapeCollectedData.Load("tape_2");
			coinData.Load("coin");
		}
		for (auto& c : cassettes)
		{
			c.model = newModel(tapeData);
			c.collectedModel = newModel(tapeCollectedData);
		}
		for (auto& c : coins)
			c.model = newModel(coinData);

		for (auto& s : strawberries)
			s.model = newModel(strawberryData, 0);            // Model.Materials[0].Effects = 0
		for (auto& s : springs)
		{
			s.model = newModel(springData);
			if (s.model)
			{
				s.model->Transform = Mat4::Scale(8.0f);
				s.model->SetLooping("Spring", false);
				s.model->Play("Idle");
			}
		}
		for (auto& r : refills)
			r.model = newModel(r.isDouble ? refillDoubleData : refillData);
		for (auto& f : feathers)
			f.model = newModel(featherData);
		for (auto& f : flags)
		{
			f.modelOn = newModel(flagOnData);
			f.modelOff = newModel(flagOffData);
			if (f.modelOn) { f.modelOn->Play("Idle"); f.modelOn->Transform = Mat4::Scale(0.2f); }
			if (f.modelOff) { f.modelOff->Play("Idle"); f.modelOff->Transform = Mat4::Scale(0.2f); }
		}
	}

	// grupo de bloques que desbloquea una fresa (IUnlockStrawberry con el mismo GroupName)
	static bool unlockConditions(const Strawberry& s, bool& anyCondition)
	{
		anyCondition = false;
		bool ready = true;
		if (!s.unlockGroup[0]) return true;
		for (const Block& b : blocks)
		{
			if ((b.kind != Block::Falling && b.kind != Block::DoubleDash) || strcmp(b.group, s.unlockGroup) != 0)
				continue;
			anyCondition = true;
			if (b.alive && !b.Satisfied())
				ready = false;
		}
		return ready;
	}

	void AddedAll()
	{
		for (auto& b : blocks)
			b.Added();
		for (auto& s : strawberries)
		{
			if (s.collected) s.locked = false;
			if (s.locked)
			{
				bool any;
				unlockConditions(s, any);
				if (!any) s.locked = false;
				else s.pointShadowAlpha = 0;
			}
		}
	}

	void ResetForRespawn()
	{
		// el original recarga el mundo: todo vuelve a su estado inicial salvo lo guardado
		for (auto& b : blocks)
		{
			Block fresh;
			fresh.kind = b.kind;
			fresh.index = b.index;
			fresh.start = fresh.position = b.start;
			fresh.end = b.end;
			fresh.flags = b.flags;
			memcpy(fresh.group, b.group, sizeof(fresh.group));
			b = fresh;
			World::actorAlive[b.index] = true;
			World::solids[World::actorSolid[b.index]].collidable = b.kind != Block::Cassette || (b.flags & 16);
			b.Transformed();
		}
		for (auto& s : strawberries)
		{
			if (!s.collected) { s.destroyed = false; s.collecting = false; }
			s.scaleMultiplier = 1;
			s.unlockTimer = -1;
		}
		for (auto& s : springs) { s.tCooldown = 0; if (s.model) s.model->Play("Idle", true); }
		for (auto& r : refills) { r.tCooldown = 0; r.tCollect = 0; }
		for (auto& f : feathers) f.tCooldown = 0;
		for (auto& c : cassettes) { c.tCooldown = 1.0f; c.tWiggle = 0; }
		for (auto& c : coins) c.collected = false;
		debris.clear();
		dust.clear();
		AddedAll();
	}

	// ---------------------------------------------------------------- actualización
	void UpdateSolids(float dt)
	{
		for (auto& b : blocks)
			if (b.alive)
				b.Update(dt);
	}

	void Update(float dt)
	{
		// fresas
		for (auto& s : strawberries)
		{
			if (s.destroyed) continue;
			if (s.locked)
			{
				// Strawberry.Update: revisa las condiciones cada 0.1 s
				bool any;
				if (Calc::OnInterval(GeneralTimer, dt, 0.1) && unlockConditions(s, any) && any)
				{
					if (OnStrawberryUnlock) OnStrawberryUnlock(s);
					else { s.locked = false; s.unlockTimer = 0; }
				}
				continue;
			}
			// UnlockRoutine: rebote de escala durante 0.8 s
			if (s.unlockTimer >= 0)
			{
				s.unlockTimer += dt / .8f;
				float t = s.unlockTimer;
				s.scaleMultiplier = t < 1 ? 1 + sinf(t * TAU * 3) * .3f * (1 - t) : 1;
				if (t >= 1) s.unlockTimer = -1;
			}
			s.pointShadowAlpha = s.collected ? 0.5f : 1.0f;
			if (!s.collected || s.collecting)
			{
				s.particles.Spawn(s.position + Vec3(6 - Rng(0, 12), 6 - Rng(0, 12), 6 - Rng(0, 12)), Vec3(), 1);
				s.particles.Update(dt);
			}
		}

		// resortes (pegados a un bloque: se mueven con él)
		for (auto& s : springs)
		{
			s.position = s.base + (s.attach >= 0 ? World::actorDisplacement[s.attach] : Vec3());
			if (s.model) s.model->Update(dt);
			if (s.tCooldown > 0) s.tCooldown -= dt;
		}

		// cristales
		for (auto& r : refills)
		{
			if (r.tCooldown > 0)
			{
				r.tCooldown -= dt;
				if (r.tCooldown <= 0)
					Audio::Play(r.isDouble ? "sfx_dashcrystal_double_return" : "sfx_dashcrystal_return", r.position);
			}
			if (r.tCollect > 0) r.tCollect -= dt * 3.0f;
			r.pointShadowAlpha = r.tCooldown <= 0 ? 1 : 0;
			r.particles.Spawn(r.position + Vec3(6 - Rng(0, 12), 6 - Rng(0, 12), 6 - Rng(0, 12)), Vec3(), 1);
			r.particles.Update(dt);
		}

		// plumas
		for (auto& f : feathers)
		{
			if (f.tCooldown > 0)
			{
				f.tCooldown -= dt;
				if (f.tCooldown <= 0)
					Audio::Play("sfx_feather_reappear", f.position);
			}
			f.pointShadowAlpha = f.tCooldown <= 0 ? 1 : 0;
			f.particles.Spawn(f.position + Vec3(6 - Rng(0, 12), 6 - Rng(0, 12), 6 - Rng(0, 12)), Vec3(), 1);
			f.particles.Update(dt);
		}

		// cassettes
		for (auto& c : cassettes)
		{
			c.pointShadowAlpha = c.IsCollected() ? 0.5f : 1.0f;
			Calc::ApproachRef(c.tCooldown, 0, dt);
			Calc::ApproachRef(c.tWiggle, 0, dt / 0.7f);
		}

		// banderas
		for (auto& f : flags)
			if (f.checkpoint == currentCheckpoint)
			{
				Calc::ApproachRef(f.tWiggle, 0, dt / 0.7f);
				if (f.modelOn) f.modelOn->Update(dt);
			}

		// polvo (Dust.cs)
		for (size_t i = 0; i < dust.size();)
		{
			Dust& d = dust[i];
			d.position += d.velocity * dt;
			d.velocity.z += 10 * dt;
			Vec2 vxy = Calc::Approach(d.velocity.XY(), Vec2(), 200 * dt);
			d.velocity = Vec3(vxy, d.velocity.z);
			d.percent += dt / d.duration;
			if (d.percent >= 1.0f) { dust[i] = dust.back(); dust.pop_back(); }
			else i++;
		}

		// escombros (Debris.cs)
		for (size_t i = 0; i < debris.size();)
		{
			Debris& d = debris[i];
			d.position += d.velocity * dt;
			d.velocity.z -= 200 * dt;
			Vec2 vxy = Calc::Approach(d.velocity.XY(), Vec2(), 400 * dt);
			d.velocity = Vec3(vxy, d.velocity.z);
			d.timer += dt;
			if (d.timer > 2.0f) { debris[i] = debris.back(); debris.pop_back(); }
			else i++;
		}
	}

	void Pickups(Player& p)
	{
		Vec3 waist = p.Position + Vec3::UnitZ() * 3;
		auto near = [&](Vec3 at, float radius) { return (waist - at).LengthSquared() < radius * radius; };

		for (auto& s : strawberries)
			if (!s.destroyed && !s.collected && !s.collecting && !s.locked && near(s.position, 12))
			{
				s.collecting = true;
				p.StrawbGet(&s);
				return;
			}
		for (auto& s : springs)
			if (s.tCooldown <= 0 && near(s.position, 16))
			{
				s.tCooldown = 1.0f;
				Audio::Play("sfx_springboard", s.position);
				if (s.model) s.model->Play("Spring", true);
				p.Spring(s.position);
				if (s.attach >= 0 && blocks[s.attach].kind == Block::Falling)
					blocks[s.attach].Trigger();
			}
		for (auto& r : refills)
		{
			int count = r.isDouble ? 2 : 1;
			if (r.tCooldown <= 0 && p.Dashes() < count && near(r.position, 20))
			{
				p.RefillDash(count);
				r.tCooldown = 4;
				r.tCollect = 1.0f;
				Game::HitStun = 0.05f;
				Audio::Play(r.isDouble ? "sfx_dashcrystal_double" : "sfx_dashcrystal", r.position);
			}
		}
		for (auto& f : feathers)
			if (f.tCooldown <= 0 && near(f.position, 16))
			{
				f.tCooldown = 1.5f;
				p.FeatherGet(f.position);
			}
		for (auto& c : coins)
			if (!c.collected && near(c.position, 20))
			{
				c.collected = true;
				Audio::Play(AnyCoinRemaining() ? "sfx_touch_switch_any" : "sfx_touch_switch_last", c.position);
			}
		for (auto& c : cassettes)
			if (!c.IsCollected() && c.tCooldown <= 0 && near(c.position, 10))
			{
				p.Stop();
				p.EnterCassette(&c);
				c.tWiggle = 1.0f;
				return;
			}
		for (auto& f : flags)
			if (f.checkpoint != currentCheckpoint && near(f.position, 16))
			{
				Audio::Play("sfx_checkpoint", f.position);
				currentCheckpoint = f.checkpoint;
				Save.record.checkpoint = f.checkpoint;   // Save.CurrentRecord.Checkpoint = Name
				Save.Write();
				f.tWiggle = 1;
			}
	}

	// ---------------------------------------------------------------- dibujo
	void CollectSprites(Vec3 camPos, Vec3 camLeft, Vec3 camUp)
	{
		static C3D_Tex* gradient = Gfx::GetTexture("spr_gradient", false);
		static C3D_Tex* ring = Gfx::GetTexture("spr_ring", false);
		const Color4 halo = Color4::FromRGB(0xeed14f);

		for (auto& s : strawberries)
		{
			if (s.destroyed || s.locked || !IsVisible(s.position, 16)) continue;
			if (s.pointShadowAlpha > 0)
				AddShadowSprite(s.position + Vec3::UnitZ(), s.pointShadowAlpha);
			if (s.collected) continue;
			Vec3 haloPos = s.position + Vec3::UnitZ() * 2 + s.ModelTransform().Translation();
			Sprites::Billboard(gradient, haloPos, 12, halo * 0.40f, camLeft, camUp);
			float pulse = s.Pulse();
			if (pulse < 1.0f)
				Sprites::Billboard(gradient, haloPos, Ease::CubeOut(pulse) * 20, halo * (0.40f * (1.0f - pulse)), camLeft, camUp);
			s.particles.CollectSprites(s.position, camPos, camLeft, camUp);
		}

		for (auto& r : refills)
		{
			if (!IsVisible(r.position, 30)) continue;
			if (r.pointShadowAlpha > 0) AddShadowSprite(r.position + Vec3::UnitZ(), r.pointShadowAlpha);
			if (r.tCollect > 0)
			{
				// Sprite.CreateFlat: anillos horizontales que se expanden
				float size = (1.0f - Ease::CubeIn(r.tCollect)) * 50;
				float a = r.tCollect;
				Vec3 at = r.position - Vec3::UnitZ() * 3;
				auto flat = [&](float sz, Color4 c)
				{
					Sprites::Quad(ring, at + Vec3(-1, 1, 0) * sz, at + Vec3(1, 1, 0) * sz, at + Vec3(1, -1, 0) * sz, at + Vec3(-1, -1, 0) * sz, c);
				};
				flat(size * 0.75f, Color4() * (a * a));
				flat(size, Color4() * a);
			}
			r.particles.CollectSprites(r.position, camPos, camLeft, camUp);
		}

		for (auto& f : feathers)
		{
			if (!IsVisible(f.position, 20) || f.tCooldown > 0) continue;
			AddShadowSprite(f.position + Vec3::UnitZ(), f.pointShadowAlpha);
			f.particles.CollectSprites(f.position, camPos, camLeft, camUp);
			Vec3 haloPos = f.position + Vec3::UnitZ() * 2 + f.ModelTransform().Translation();
			Sprites::Billboard(gradient, haloPos, 10, halo * 0.50f, camLeft, camUp);
		}

		for (auto& c : cassettes)
		{
			if (!IsVisible(c.position, 30)) continue;
			AddShadowSprite(c.position + Vec3::UnitZ(), c.pointShadowAlpha);
			if (c.tWiggle > 0)
			{
				Sprites::Billboard(ring, c.position, c.tWiggle * c.tWiggle * 40, Color4() * .4f, camLeft, camUp, true);
				Sprites::Billboard(ring, c.position, c.tWiggle * 50, Color4() * .4f, camLeft, camUp, true);
			}
		}
		for (auto& c : coins)
		{
			if (!IsVisible(c.position, 20)) continue;
			AddShadowSprite(c.position + Vec3::UnitZ(), 1);
			if (!c.collected)
				Sprites::Billboard(gradient, c.position + Vec3::UnitZ() * 2, 10, Color4::FromRGB(0x5fcde4) * 0.50f, camLeft, camUp);
		}

		for (auto& f : flags)
		{
			if (!IsVisible(f.position + Vec3(0, 0, 10), 30)) continue;
			Vec3 haloPos = f.position + Vec3::UnitZ() * 16;
			Color4 col = Color4::FromRGB(f.checkpoint == currentCheckpoint ? 0x7fde46 : 0xdf5ab4) * .4f;
			Sprites::Billboard(gradient, haloPos, 12, col * 0.40f, camLeft, camUp);
			if (f.tWiggle > 0)
			{
				Sprites::Billboard(ring, haloPos, f.tWiggle * f.tWiggle * 40, col, camLeft, camUp, true);
				Sprites::Billboard(ring, haloPos, f.tWiggle * 50, col, camLeft, camUp, true);
			}
		}

		for (auto& b : blocks)
			if (b.kind == Block::DoubleDash && b.ready && !b.satisfied && b.pulse > 0)
			{
				Color4 col = Color4::FromRGB(b.pulseColor);
				Vec3 haloPos = b.position + Vec3::UnitZ() * 2;
				Sprites::Billboard(gradient, haloPos, 50 * b.pulse, col * 0.50f, camLeft, camUp, true);
				Sprites::Billboard(ring, b.position, b.pulse * b.pulse * 40, col * .4f, camLeft, camUp, true);
				Sprites::Billboard(ring, b.position, b.pulse * 50, col * .4f, camLeft, camUp, true);
			}

		for (auto& d : debris)
			Sprites::Billboard(Gfx::GetTexture(d.image, false), d.position, 2, Color4(), camLeft, camUp);

		static C3D_Tex* dustTex[5] = {
			Gfx::GetTexture("spr_dust_0", false), Gfx::GetTexture("spr_dust_1", false), Gfx::GetTexture("spr_dust_2", false),
			Gfx::GetTexture("spr_dust_3", false), Gfx::GetTexture("spr_dust_4", false) };
		for (auto& d : dust)
			Sprites::Billboard(dustTex[d.image], d.position, 4 * (1.0f - d.percent), d.color, camLeft, camUp);
	}

	void DrawModels(bool onlyCollecting)
	{
		for (auto& s : strawberries)
		{
			if (s.destroyed || s.locked || !s.model || s.collected) continue;
			if (onlyCollecting != s.collecting) continue;
			if (!onlyCollecting && !IsVisible(s.position, 16)) continue;
			s.model->Draw(s.WorldMatrix());
		}
		if (onlyCollecting) return;

		// BaseTranslation (RotX 90°) va al final de cada cadena
		const Mat4 base = Mat4::RotateX(PI / 2);
		for (auto& s : springs)
			if (s.model && IsVisible(s.position, 20))
				s.model->Draw(Mat4::Translate(s.position) * Mat4::RotateZ(s.facing.Angle() + PI / 2) * s.model->Transform * base);
		for (auto& r : refills)
			if (r.model && r.tCooldown <= 0 && IsVisible(r.position, 10))
				r.model->Draw(Mat4::Translate(r.position) * r.ModelTransform() * base);
		for (auto& f : feathers)
			if (f.model && f.tCooldown <= 0 && IsVisible(f.position, 10))
				f.model->Draw(Mat4::Translate(f.position) * f.ModelTransform() * base);
		for (auto& c : cassettes)
			if (c.model && !c.IsCollected() && IsVisible(c.position, 10))
				c.model->Draw(Mat4::Translate(c.position) * c.ModelTransform() * base);
		for (auto& c : coins)
			if (c.model && !c.collected && IsVisible(c.position, 10))
			{
				// Coin: color 0x5fcde4 sobre su textura, escala 6, gira y flota
				float t = GeneralTimer;
				c.model->MaterialColors.assign(c.model->MaterialColors.size(), 0x5fcde4);
				c.model->Draw(Mat4::Translate(c.position) * Mat4::RotateZ(t * 3.0f) *
					Mat4::Translate(Vec3::UnitZ() * (sinf(t * 2.0f) * 2)) * Mat4::Scale(6.0f) * base);
			}
		for (auto& f : flags)
		{
			SkinnedModel* m = f.checkpoint == currentCheckpoint ? f.modelOn : f.modelOff;
			if (m && IsVisible(f.position + Vec3(0, 0, 10), 30))
				m->Draw(Mat4::Translate(f.position) * m->Transform * base);
		}
	}
}

// ---------------------------------------------------------------- ParticleSystem
void ParticleSystem::Spawn(Vec3 position, Vec3 velocity, float rateMultiplier)
{
	accumulator += rateMultiplier * theme.rate * deltaTime;
	while (accumulator > 0)
	{
		if (particles.size() >= maxParticles)
			particles.erase(particles.begin());
		particles.push_back({ position, velocity, theme.life });
		accumulator--;
	}
}

void ParticleSystem::Update(float dt)
{
	deltaTime = dt;
	for (size_t i = particles.size(); i-- > 0;)
	{
		Particle& p = particles[i];
		p.life -= dt;
		p.velocity += theme.gravity * dt;
		p.position += p.velocity * dt;
		if (p.life <= 0)
			particles.erase(particles.begin() + i);
	}
}

void ParticleSystem::CollectSprites(Vec3 source, Vec3 camPos, Vec3 camLeft, Vec3 camUp)
{
	if ((camPos - source).LengthSquared() > 400 * 400) return;   // MaximumDistance
	C3D_Tex* tex = Gfx::GetTexture(theme.sprite, false);
	for (auto& p : particles)
		if (p.life > 0)
			Sprites::Billboard(tex, p.position, theme.size * p.life / theme.life, Color4(), camLeft, camUp);
}

// ---------------------------------------------------------------- Strawberry
float Strawberry::Pulse() const
{
	return Calc::ClampedMap(fmodf(Actors::GeneralTimer, 3), 0, 0.25f, 0, 1);
}

Mat4 Strawberry::ModelTransform() const
{
	float scale = 3.0f;
	if (!collected && !collecting)
		scale += Ease::BackIn(Ease::UpDown(Pulse())) * 0.50f;
	scale *= scaleMultiplier;
	if (collecting)
		return Mat4::Scale(scale);
	float t = Actors::GeneralTimer;
	return Mat4::RotateZ(t * 3.0f) * Mat4::Translate(Vec3::UnitZ() * (sinf(t * 2.0f) * 2)) * Mat4::Scale(scale);
}

Mat4 Strawberry::WorldMatrix() const
{
	return Mat4::Translate(position) * ModelTransform() * Mat4::RotateX(PI / 2);
}

bool Cassette::IsCollected() const
{
	return !map.empty() && Save.record.completedSubMaps.count(map) > 0;
}

Mat4 Cassette::ModelTransform() const
{
	float wiggle = 1 + sinf(tWiggle * TAU * 2) * .8f * tWiggle;
	float t = Actors::GeneralTimer;
	return Mat4::RotateZ(t * 3.0f) * Mat4::Translate(Vec3::UnitZ() * (sinf(t * 2.0f) * 2)) * Mat4::Scale(2.5f * wiggle);
}

Mat4 Refill::ModelTransform() const
{
	float t = Actors::GeneralTimer;
	return Mat4::RotateZ(t * 3.0f) * Mat4::Translate(Vec3::UnitZ() * (sinf(t * 2.0f) * 2)) * Mat4::Scale(2.0f);
}

Mat4 Feather::ModelTransform() const
{
	float t = Actors::GeneralTimer;
	return Mat4::RotateZ(t * 3.0f) * Mat4::Translate(Vec3::UnitZ() * (sinf(t * 2.0f) * 2)) * Mat4::Scale(2.0f);
}

// ---------------------------------------------------------------- Block (Solid.cs y derivados)
Vec3 Block::LocalMin() const { return World::solids[World::actorSolid[index]].min; }
Vec3 Block::LocalMax() const { return World::solids[World::actorSolid[index]].max; }

bool Block::HasPlayerRider() const
{
	return Actors::player && Actors::player->RidingPlatformCheck(index);
}

void Block::Transformed()
{
	CollisionSolid& s = World::solids[World::actorSolid[index]];
	s.offset = position;
	World::actorDisplacement[index] = position - start;
}

void Block::Added()
{
	origin = position;
	// CassetteBlock(startOn)
	if (kind == Cassette)
		World::solids[World::actorSolid[index]].collidable = (flags & 16) != 0;
	// Solid.Transparent: la cámara no lo considera (BreakBlock de vidrio, IntroCar)
	World::solids[World::actorSolid[index]].transparent = Transparent();
	if (kind == Falling)
	{
		// EndPosition: rayo de 200 hacia abajo desde el centro, si no toca otro FallingBlock
		RayHit hit;
		CollisionSolid& self = World::solids[World::actorSolid[index]];
		self.collidable = false;
		if (World::SolidRayCast(position, -Vec3::UnitZ(), 200, hit))
		{
			int a = World::solids[hit.solid].actor;
			if (a < 0 || Actors::blocks[a].kind != Falling)
			{
				hasEnd = true;
				endPosition = hit.point - Vec3::UnitZ() * LocalMin().z;
			}
		}
		self.collidable = true;
	}
	Transformed();
}

void Block::MoveTo(Vec3 targetPos)
{
	Vec3 delta = targetPos - position;
	CollisionSolid& s = World::solids[World::actorSolid[index]];
	if (s.collidable && delta.LengthSquared() > 0.001f)
	{
		Player* p = Actors::player;
		if (p && p->RidingPlatformCheck(index))
		{
			s.collidable = false;
			p->RidingPlatformSetVelocity(velocity);
			p->RidingPlatformMoved(delta);
			s.collidable = true;
		}
	}
	position += delta;
	Transformed();
}

void Block::Destroy()
{
	alive = false;
	visible = false;
	World::actorAlive[index] = false;
	World::solids[World::actorSolid[index]].collidable = false;
}

void Block::Shatter(Vec3 direction)
{
	// BreakBlock.Break: escombros según el volumen
	Vec3 size = LocalMax() - LocalMin();
	int amount = (int)((size.x * size.y * size.z) / 200);
	static const char* glass[] = { "spr_shard_0", "spr_shard_1", "spr_shard_2" };
	static const char* wood[] = { "spr_wood_shard_0", "spr_wood_shard_1", "spr_wood_shard_2" };
	const char** options = Transparent() ? glass : wood;
	if (Secret()) Audio::Play("sfx_secret", position);
	Audio::Play(Transparent() ? "sfx_glassbreak" : "sfx_breakable_wall_wood", position);
	for (int i = 0; i < amount && Actors::debris.size() < 256; i++)
	{
		Vec3 offset(Actors::Rng(0, size.x), Actors::Rng(0, size.y), Actors::Rng(0, size.z));
		Vec3 at = position + offset - size * 0.5f;
		Actors::debris.push_back({ at, direction * Actors::Rng(100, 400), options[rand() % 3], 0 });
	}
	Destroy();
}

// SpringPhysics.cs (Spring-It-On)
static float springVelocity(float x, float vel, float frequency, float halflife, float dt)
{
	const float eps = 1.401298e-45f;
	float s = (2.0f * PI * frequency) * (2.0f * PI * frequency);
	float d = (4.0f * 0.69314718056f) / (halflife + eps);
	float c = 0;
	float y = d / 2.0f;
	auto fastNegExp = [](float v) { return 1.0f / (1.0f + v + 0.48f * v * v + 0.235f * v * v * v); };
	auto fastAtan = [](float v)
	{
		float z = fabsf(v);
		float w = z > 1.0f ? 1.0f / z : z;
		float r = (PI / 4.0f) * w - w * (w - 1) * (0.2447f + 0.0663f * w);
		return copysignf(z > 1.0f ? PI / 2.0f - r : r, v);
	};
	float disc = s - (d * d) / 4.0f;
	if (fabsf(disc) < eps)
	{
		float j0 = x - c, j1 = vel + j0 * y, e = fastNegExp(y * dt);
		return -y * j0 * e - y * dt * j1 * e + j1 * e;
	}
	if (disc > 0)
	{
		float w = sqrtf(disc);
		float j = sqrtf((vel + y * (x - c)) * (vel + y * (x - c)) / (w * w + eps) + (x - c) * (x - c));
		float p = fastAtan((vel + (x - c) * y) / (-(x - c) * w + eps));
		j = (x - c) > 0.0f ? j : -j;
		float e = fastNegExp(y * dt);
		return -y * j * e * cosf(w * dt + p) - w * j * e * sinf(w * dt + p);
	}
	float y0 = (d + sqrtf(d * d - 4 * s)) / 2.0f, y1 = (d - sqrtf(d * d - 4 * s)) / 2.0f;
	float j1 = (c * y0 - x * y0 - vel) / (y1 - y0), j0 = x - j1 - c;
	return -y0 * j0 * fastNegExp(y0 * dt) - y1 * j1 * fastNegExp(y1 * dt);
}

void Block::Update(float dt)
{
	// Solid.Update
	if (velocity.LengthSquared() > .001f)
		MoveTo(position + velocity * dt);
	if (tShake > 0)
	{
		tShake -= dt;
		if (tShake <= 0) shake = Vec3();
		else if (Calc::OnInterval(Game::Elapsed, dt, .02f))
			shake = Vec3(Actors::Rng(-1, 1), Actors::Rng(-1, 1), 0);
	}

	switch (kind)
	{
	case Falling:
		if (fstate == 0)
		{
			if (triggered || HasPlayerRider())
			{
				Audio::Play("sfx_fallingblock_shake", position);
				fstate = 1;
				tShake = .4f;
				if (Secret()) Audio::Play("sfx_secret", position);
			}
		}
		else if (fstate == 1)
		{
			if (tShake <= 0) { Audio::Play("sfx_fallingblock_fall", position); fstate = 2; }
		}
		else if (fstate == 2)
		{
			Calc::ApproachRef(velocity.z, -160, 600 * dt);
			if (hasEnd)
			{
				if (position.z <= endPosition.z)
				{
					Audio::Play("sfx_fallingblock_land", position);
					fstate = 3;
					tShake = .2f;
					velocity = Vec3();
					MoveTo(endPosition);
				}
			}
			else if (LocalMax().z + position.z < World::DeathPlane - 10)
			{
				if (Actors::submap || group[0] || Secret())
					Destroy();
				else
				{
					respawnDelay = 5.0f;
					velocity = Vec3();
					fstate = 4;
					visible = false;
				}
			}
		}
		else if (fstate == 4)
		{
			respawnDelay -= dt;
			if (respawnDelay <= 0)
			{
				MoveTo(start);
				tShake = 0.2f;
				triggered = false;
				fstate = 0;
				visible = true;
			}
		}
		break;

	case Floaty:
	{
		Player* p = Actors::player;
		bool riding = HasPlayerRider();
		if (!hasRider && riding)
		{
			hasRider = true;
			velocity += p->PreviousVelocity() * .8f;
		}
		else if (hasRider && !riding)
		{
			if (p) velocity -= p->Velocity() * .8f;
			hasRider = false;
		}
		// fricción
		const float friction = 200, threshold = 1;
		if (velocity.LengthSquared() > threshold * threshold)
			velocity = Calc::Approach(velocity, velocity.Normalized() * threshold, friction * dt);
		// resorte hacia el origen
		Vec3 diff = position - origin;
		Vec3 n = diff.Normalized();
		float vel = Dot(velocity, n);
		float oldVel = vel;
		vel = springVelocity(diff.Length(), vel, 1.2f, 3.0f, dt);
		velocity += n * (vel - oldVel);
		break;
	}

	case Moving:
		Calc::ApproachRef(lerp, target, dt / (Slow() ? 2 : 1));
		if (lerp == target)
			target = 1 - target;
		MoveTo(Lerp(start, end, -(cosf(PI * lerp) - 1) / 2));   // Utils.SineInOut
		break;

	case Traffic:
	{
		const float Acceleration = 400, MaxSpeed = 600, ReturnSpeed = 50;
		if (rstate == 0)
		{
			if (HasPlayerRider()) { Audio::Play("sfx_zipmover_start", position); tShake = .15f; timer = .15f; rstate = 1; }
		}
		else if (rstate == 1)
		{
			timer -= dt;
			if (timer <= 0) { normal = (end - position).Normalized(); rstate = 2; }
		}
		if (rstate == 2)
		{
			if (position != end && Dot((end - position).Normalized(), normal) >= 0)
				velocity = Calc::Approach(velocity, normal * MaxSpeed, Acceleration * dt);
			else
			{
				velocity = Vec3();
				MoveTo(end);
				Audio::Play("sfx_zipmover_stop", position);
				tShake = .2f;
				timer = .8f;
				rstate = 3;
			}
		}
		else if (rstate == 3)
		{
			timer -= dt;
			if (timer <= 0) { Audio::Play("sfx_zipmover_retract_start", position); normal = (start - position).Normalized(); rstate = 4; }
		}
		if (rstate == 4)
		{
			if (Dot((start - position).Normalized(), normal) >= 0)
				velocity = normal * ReturnSpeed;
			else
			{
				velocity = Vec3();
				MoveTo(start);
				Audio::Play("sfx_zipmover_retract_stop", position);
				tShake = .1f;
				timer = .5f;
				rstate = 5;
			}
		}
		else if (rstate == 5)
		{
			timer -= dt;
			if (timer <= 0) rstate = 0;
		}
		break;
	}

	case Gate:
	{
		// GateBlock: se abre cuando no quedan monedas (el nivel 1 no tiene)
		const float Acceleration = 400, MaxSpeed = 200;
		if (!opened && !Actors::AnyCoinRemaining()) { opened = true; tShake = .2f; timer = .2f; rstate = 1; Audio::Play("sfx_touch_switch_gate_open_move", position); }
		else if (rstate == 1)
		{
			timer -= dt;
			if (timer <= 0) { normal = (end - position).Normalized(); rstate = 2; }
		}
		if (rstate == 2)
		{
			if (position != end && Dot((end - position).Normalized(), normal) >= 0)
				velocity = Calc::Approach(velocity, normal * MaxSpeed, Acceleration * dt);
			else { velocity = Vec3(); MoveTo(end); tShake = .2f; rstate = 3; Audio::Play("sfx_touch_switch_gate_finish", position); }
		}
		break;
	}

	case IntroCar:
	{
		// IntroCar.Update: se hunde 1.5 con alguien encima, a 20 u/s
		bool riding = HasPlayerRider();
		if (!hasRider && riding) { hasRider = true; Audio::Play("sfx_car_down", position); }
		else if (hasRider && !riding) { hasRider = false; Audio::Play("sfx_car_up", position); }
		Vec3 targetPos = hasRider ? start - Vec3::UnitZ() * 1.5f : start;
		MoveTo(Calc::Approach(position, targetPos, 20 * dt));
		break;
	}

	case DoubleDash:
		if (!satisfied && !ready && Actors::player && Actors::player->Dashes() >= 2 && HasPlayerRider())
		{
			pulseColor = Actors::player->HairColor();
			ready = true;
			pulse = 1;
			tShake = 1.0f;
			Audio::Play("sfx_secret", position);
		}
		if (ready && pulse > 0)
		{
			pulse -= dt;
			if (pulse <= 0) satisfied = true;
		}
		break;
	}
}

// ---------------------------------------------------------------- sombra puntual
// fresas ya guardadas: blancas translúcidas (Strawberry.Update cuando IsCollected)
void Actors::DrawGhosts(C3D_Tex* white)
{
	const u32 c = 0x99ddf4;
	const float a = 0.70f;
	u32 r = (u32)(((c >> 16) & 0xFF) * a), g = (u32)(((c >> 8) & 0xFF) * a), b = (u32)((c & 0xFF) * a);
	u32 abgr = ((u32)(a * 255) << 24) | (b << 16) | (g << 8) | r;
	for (auto& s : strawberries)
		if (s.collected && !s.destroyed && !s.collecting && s.model && IsVisible(s.position, 16))
			s.model->DrawTinted(s.WorldMatrix(), abgr, white);
	const Mat4 base = Mat4::RotateX(PI / 2);
	for (auto& c : cassettes)
		if (c.IsCollected() && c.collectedModel && IsVisible(c.position, 10))
			c.collectedModel->DrawTinted(Mat4::Translate(c.position) * c.ModelTransform() * base, 0x80808080, white);
	for (auto& c : coins)
		if (c.collected && c.model && IsVisible(c.position, 10))
		{
			// collectedColor 0xf141df * 0.5 (premultiplicado)
			float t = GeneralTimer;
			c.model->DrawTinted(Mat4::Translate(c.position) * Mat4::RotateZ(t * 3.0f) *
				Mat4::Translate(Vec3::UnitZ() * (sinf(t * 2.0f) * 2)) * Mat4::Scale(6.0f) * base, 0x806F2078, white);
		}
}

void AddShadowSprite(Vec3 position, float strength)
{
	static C3D_Tex* circle = Gfx::GetTexture("spr_circle", false);
	RayHit hit;
	if (!World::SolidRayCast(position, -Vec3::UnitZ(), 1000, hit))
		return;
	Vec3 a = Cross(hit.normal, Vec3(0, 1, 0));
	Vec3 b = Cross(hit.normal, Vec3(1, 0, 0));
	Vec3 at = hit.point + Vec3(0, 0, 0.01f);
	float size = Calc::Map(hit.distance, 0, 50, 3, 2);
	Color4 color = Color4::FromRGB(0x1d0b44) * (0.50f * strength);
	Sprites::Quad(circle, at + (-a - b) * size, at + (a - b) * size, at + (a + b) * size, at + (-a + b) * size, color);
}

// ---------------------------------------------------------------- Hair.cs
static std::vector<Vec3> sphereVerts;
static std::vector<u16> sphereIdx;

static void createSphere(int sliceCount, int stackCount)
{
	sphereVerts.clear();
	sphereIdx.clear();
	sphereVerts.push_back(Vec3(0, 0, 1));
	for (int i = 0; i < stackCount - 1; i++)
	{
		float phi = PI * (i + 1) / (float)stackCount;
		for (int j = 0; j < sliceCount; j++)
		{
			float theta = 2.0f * PI * j / (float)sliceCount;
			sphereVerts.push_back(Vec3(sinf(phi) * cosf(theta), sinf(phi) * sinf(theta), cosf(phi)));
		}
	}
	u16 v1 = (u16)sphereVerts.size();
	sphereVerts.push_back(Vec3(0, 0, -1));
	for (int i = 0; i < sliceCount; ++i)
	{
		u16 i0 = i + 1, i1 = (i + 1) % sliceCount + 1;
		sphereIdx.insert(sphereIdx.end(), { 0, i1, i0 });
		i0 = i + sliceCount * (stackCount - 2) + 1;
		i1 = (i + 1) % sliceCount + sliceCount * (stackCount - 2) + 1;
		sphereIdx.insert(sphereIdx.end(), { v1, i0, i1 });
	}
	for (int j = 0; j < stackCount - 2; j++)
	{
		int j0 = j * sliceCount + 1, j1 = (j + 1) * sliceCount + 1;
		for (int i = 0; i < sliceCount; i++)
		{
			u16 a = j0 + i, b = j0 + (i + 1) % sliceCount, c = j1 + (i + 1) % sliceCount, d = j1 + i;
			sphereIdx.insert(sphereIdx.end(), { a, b, c, a, c, d });
		}
	}
}

struct HairVertex { float pos[3], uv[2]; s8 nrm[4]; };
static HairVertex* hairPool[2];
static u16* hairIdx;
static int hairPoolIndex;
static constexpr int MaxHairNodes = 18;

void Hair::Init()
{
	if (sphereVerts.empty())
	{
		createSphere(6, 6);
		for (auto& p : hairPool)
			p = (HairVertex*)linearAlloc(sizeof(HairVertex) * sphereVerts.size() * MaxHairNodes);
		hairIdx = (u16*)linearAlloc(sizeof(u16) * sphereIdx.size() * MaxHairNodes);
		for (int n = 0; n < MaxHairNodes; n++)
			for (size_t k = 0; k < sphereIdx.size(); k++)
				hairIdx[n * sphereIdx.size() + k] = (u16)(sphereIdx[k] + n * sphereVerts.size());
		GSPGPU_FlushDataCache(hairIdx, sizeof(u16) * sphereIdx.size() * MaxHairNodes);
	}
	nodes.clear();
}

void Hair::Update(float dt, const Mat4& positionTransform)
{
	wave += dt * 4.0f;
	Vec3 offsetPerNode = Forward * ForwardOffsetPerNode + Vec3(0, 0, -1 * (1 - Roundness));
	const Vec3 origin(0, 1.0f, -.4f);

	nodes.resize(Nodes);
	nodes[0] = positionTransform.TransformPoint(origin);

	Vec3 target = nodes[0], prev = nodes[0];
	const float maxdist = 0.8f;
	Vec3 side = Mat4::RotateZ(-PI / 2).TransformNormal(Forward);
	// plano con normal Forward que pasa por nodes[0] + Forward
	Vec3 planeN = Forward;
	float planeD = -Dot(Forward, nodes[0] + Forward);

	for (int i = 1; i < (int)nodes.size(); i++)
	{
		float p = i / (float)nodes.size();
		target += side * (sinf(wave + i * 0.50f) * 0.50f * p);
		nodes[i] += (target - nodes[i]) * 0.25f;
		float dist = Dot(planeN, nodes[i]) + planeD;
		if (dist < 0)
			nodes[i] -= planeN * dist;
		if (Grounded)
		{
			float distanceToGround = Calc::Lerp(4, 6, p);
			if (nodes[i].z < nodes[0].z - distanceToGround)
				nodes[i].z = nodes[0].z - distanceToGround;
		}
		if ((nodes[i] - prev).Length() > maxdist)
			nodes[i] = prev + (nodes[i] - prev).Normalized() * maxdist;
		target = nodes[i] + offsetPerNode;
		prev = nodes[i];
	}
}

void Hair::Draw(float alpha)
{
	if (nodes.empty()) return;
	hairPoolIndex ^= 1;
	HairVertex* out = hairPool[hairPoolIndex];
	int n = (int)nodes.size() < MaxHairNodes ? (int)nodes.size() : MaxHairNodes;
	Mat4 angle = Mat4::RotateZ(Forward.XY().Angle());
	for (int i = 0; i < n; i++)
	{
		float lerp = i / (float)nodes.size();
		float xz = Calc::Lerp(Calc::Lerp(3.2f, 3, Roundness), 1, lerp);
		float y = Calc::Lerp(Calc::Lerp(4, 3, Roundness), Calc::Lerp(2, 1, Roundness), lerp);
		// Scale * angle * Translation (fila) => T * angle * S
		Mat4 t = Mat4::Translate(nodes[i]) * angle * Mat4::Scale(Vec3(xz, y, xz) * Squish);
		for (const Vec3& v : sphereVerts)
		{
			Vec3 p = t.TransformPoint(v);
			Vec3 nn = v;   // la normal de la esfera unitaria (el original no la transforma)
			HairVertex& hv = *out++;
			hv.pos[0] = p.x; hv.pos[1] = p.y; hv.pos[2] = p.z;
			hv.uv[0] = hv.uv[1] = 0.5f;
			hv.nrm[0] = (s8)(nn.x * 127); hv.nrm[1] = (s8)(nn.y * 127); hv.nrm[2] = (s8)(nn.z * 127); hv.nrm[3] = 0;
		}
	}
	GSPGPU_FlushDataCache(hairPool[hairPoolIndex], sizeof(HairVertex) * sphereVerts.size() * n);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, Gfx::scene.modelView, &Gfx::view);
	Gfx::SetSceneParams(0);                         // Hair.Materials[0].Effects = 0
	{
		// premultiplicado: color * alpha
		u32 r = (u32)(((Color >> 16) & 0xFF) * alpha), g = (u32)(((Color >> 8) & 0xFF) * alpha), b = (u32)((Color & 0xFF) * alpha);
		Gfx::SetLitTexEnv(((u32)(alpha * 255) << 24) | (b << 16) | (g << 8) | r);
	}
	C3D_CullFace(GPU_CULL_NONE);
	C3D_TexBind(0, Sprites::White());
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, hairPool[hairPoolIndex], sizeof(HairVertex), 3, 0x210);
	C3D_DrawElements(GPU_TRIANGLES, (int)sphereIdx.size() * n, C3D_UNSIGNED_SHORT, hairIdx);
	C3D_CullFace(GPU_CULL_BACK_CCW);
}
