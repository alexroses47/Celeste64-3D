#include "cutscene.h"
#include "audio.h"
#include "save.h"
#include "gfx.h"
#include "input.h"
#include "player.h"
#include "sprites.h"
#include <stdio.h>
#include <string.h>
#include <map>

void logf_(const char* fmt, ...);

// ---------------------------------------------------------------- Language.Dialog (inglés)
namespace Dialog
{
	static std::map<std::string, std::vector<Line>> dialog;

	bool Load()
	{
		if (!dialog.empty()) return true;   // una sola vez
		FILE* f = fopen("romfs:/dialog.txt", "r");
		if (!f) return false;
		char buf[512];
		std::vector<Line>* current = nullptr;
		while (fgets(buf, sizeof(buf), f))
		{
			std::string s(buf);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
			if (s.empty()) continue;
			if (s[0] == '@') { current = &dialog[s.substr(1)]; continue; }
			if (!current) continue;
			size_t bar = s.find('|');
			size_t bar2 = bar == std::string::npos ? bar : s.find('|', bar + 1);
			Line line;
			line.face = s.substr(0, bar);
			line.voice = bar2 == std::string::npos ? "" : s.substr(bar + 1, bar2 - bar - 1);
			std::string text = bar2 == std::string::npos ? s : s.substr(bar2 + 1);
			for (size_t i = 0; i + 1 < text.size(); i++)
				if (text[i] == '\\' && text[i + 1] == 'n') text.replace(i, 2, "\n");
			line.text = text;
			current->push_back(line);
		}
		fclose(f);
		logf_("dialogos: %d conversaciones\n", (int)dialog.size());
		return true;
	}

	const std::vector<Line>* Lines(const std::string& key)
	{
		auto it = dialog.find(key);
		return it == dialog.end() ? nullptr : &it->second;
	}
}

// ---------------------------------------------------------------- NPC
static const char* talkFlag(int type) { return type == Npc::Granny ? "GRANNY" : (type == Npc::Theo ? "THEO" : "BADELINE"); }

std::string Npc::NextConversation() const
{
	const char* prefix = type == Granny ? "Granny" : (type == Theo ? "Theo" : "Baddy");
	return prefix + std::to_string(Save.record.GetFlag(talkFlag(type)) + 1);
}

void Npc::CheckForDialog()
{
	if (type == Sign) { interactEnabled = true; return; }
	interactEnabled = Dialog::Lines(NextConversation()) != nullptr;
}

// ---------------------------------------------------------------- cinemática
namespace Cutscenes
{
	std::vector<Npc> npcs;
	static ModelData grannyData, theoData, badelineData, signData;

	struct Step
	{
		enum Kind { MoveToDistance, Face, Say, Wait, LookAtLerp, PositionLerp, Unlock, Wiggle, RestoreCamera, IncFlag } kind;
		int actor = -1;         // -1 = Madeline, si no índice de npc
		Vec3 target;
		float duration = 0;
		std::string key;
	};

	struct Saying
	{
		std::string face, text;
		int characters = 0;
		float ease = 0;
		bool talking = false;
		int phase = 0;          // 0 entra, 1 escribe, 2 espera, 3 sale
		float counter = 0;
	};

	static bool active = false, freeze = false;
	static std::vector<Step> steps;
	static size_t stepIndex;
	static int stepPhase;
	static float stepTimer;
	static float ease;           // franjas de cine
	static int easePhase;        // 0 entrando, 1 pasos, 2 terminado
	static float timer;
	static Saying saying;
	static std::vector<Dialog::Line> sayQueue;
	static size_t sayIndex;
	static Npc* talkingNpc;
	static Strawberry* unlocking;
	static Vec3 wasLookAt, wasPosition, fromVec;

	bool Active() { return active; }
	bool FreezeGame() { return active && freeze; }

	static void start(bool freezeGame)
	{
		active = true;
		freeze = freezeGame;
		stepIndex = 0;
		stepPhase = 0;
		stepTimer = 0;
		ease = 0;
		easePhase = 0;
		timer = 0;
		saying = Saying();
	}

	void Interact(Npc& npc)
	{
		steps.clear();
		talkingNpc = &npc;
		int idx = (int)(&npc - &npcs[0]);
		if (npc.type == Npc::Sign)
		{
			// Signpost.Talk: mirar al letrero y leer
			steps.push_back({ Step::Face, -1, npc.position });
			Step say{ Step::Say };
			say.key = npc.dialog;
			steps.push_back(say);
		}
		else
		{
			// Granny/Theo/Badeline.Conversation
			Step move{ Step::MoveToDistance, -1, npc.position };
			move.duration = 16;
			steps.push_back(move);
			steps.push_back({ Step::Face, -1, Vec3() });       // Madeline mira al personaje
			Step faceBack{ Step::Face, idx, Vec3() };           // y el personaje a Madeline
			steps.push_back(faceBack);
			Step say{ Step::Say };
			say.key = npc.NextConversation();
			steps.push_back(say);
			Step inc{ Step::IncFlag, idx };
			steps.push_back(inc);
		}
		start(false);
	}

	void StrawberryUnlock(Strawberry& s)
	{
		// Strawberry.UnlockRoutine (congela el juego)
		steps.clear();
		unlocking = &s;
		talkingNpc = nullptr;
		wasLookAt = Game::Camera.LookAt;
		wasPosition = Game::Camera.Position;
		Vec3 toNormal = (Game::Camera.Position - s.position).Normalized();
		if (s.playUnlockSound) Audio::Play("sfx_secret", s.position);
		steps.push_back({ Step::Wait, -1, Vec3(), 0.1f });
		steps.push_back({ Step::LookAtLerp, -1, s.position, 0.2f });
		steps.push_back({ Step::Wait, -1, Vec3(), 0.2f });
		steps.push_back({ Step::PositionLerp, -1, s.position + toNormal * 70, 1.0f });
		steps.push_back({ Step::Wait, -1, Vec3(), 0.1f });
		steps.push_back({ Step::Unlock });
		steps.push_back({ Step::Wiggle, -1, Vec3(), 0.8f });
		steps.push_back({ Step::Wait, -1, Vec3(), 0.7f });
		steps.push_back({ Step::RestoreCamera });
		start(true);
		easePhase = 1;   // el desbloqueo no muestra franjas
		ease = 0;
	}

	static Vec3 actorPos(int a) { return a < 0 ? Actors::player->Position : npcs[a].position; }

	// devuelve true cuando el paso terminó
	static bool runStep(Step& st, float dt)
	{
		Player* p = Actors::player;
		switch (st.kind)
		{
		case Step::MoveToDistance:
		{
			Vec2 npcXY = st.target.XY();
			if (stepPhase == 0)
			{
				Vec2 normal = (p->Position.XY() - npcXY).Normalized();
				st.target = Vec3(npcXY + normal * st.duration, 0);
				stepPhase = (p->Position.XY() - st.target.XY()).Length() > 4 ? 1 : 2;
			}
			if (stepPhase == 1)
			{
				// Face(actor, destino) antes de correr
				Vec2 facing = (st.target.XY() - p->Position.XY()).Normalized();
				Vec2 current = p->Facing;
				if (fabsf(facing.Angle() - current.Angle()) > 0.05f)
				{
					current = Calc::AngleToVector(Calc::AngleApproach(current.Angle(), facing.Angle(), TAU * 1.5f * dt));
					p->SetTargetFacing(current);
					return false;
				}
				p->Model->Play("Run");
				stepPhase = 2;
			}
			Vec2 v2 = p->Position.XY();
			if (v2 != st.target.XY())
			{
				v2 = Calc::Approach(v2, st.target.XY(), 50 * dt);
				p->Position = Vec3(v2, p->Position.z);
				return false;
			}
			p->Model->Play("Idle");
			return true;
		}

		case Step::Face:
		{
			Vec3 target = st.target;
			if (st.actor < 0 && talkingNpc && talkingNpc->type != Npc::Sign) target = talkingNpc->position;
			if (st.actor >= 0) target = p->Position;
			Vec3 from = actorPos(st.actor);
			Vec2 facing = (target - from).XY().Normalized();
			Vec2 current = st.actor < 0 ? p->Facing : npcs[st.actor].facing;
			if (stepPhase == 0) { stepPhase = 1; st.target = target; }
			if (fabsf(facing.Angle() - current.Angle()) > 0.05f)
			{
				current = Calc::AngleToVector(Calc::AngleApproach(current.Angle(), facing.Angle(), TAU * 1.5f * dt));
				if (st.actor < 0) p->SetTargetFacing(current);
				else npcs[st.actor].facing = current;
				return false;
			}
			return true;
		}

		case Step::Say:
		{
			if (stepPhase == 0)
			{
				const auto* lines = Dialog::Lines(st.key);
				sayQueue = lines ? *lines : std::vector<Dialog::Line>();
				sayIndex = 0;
				for (auto& l : sayQueue)
					if (!l.voice.empty()) Audio::PreloadVoice(l.voice.c_str());
				stepPhase = 1;
				saying = Saying();
			}
			if (sayIndex >= sayQueue.size())
				return true;
			Saying& s = saying;
			if (s.text.empty() && s.phase == 0 && s.ease == 0)
			{
				s.face = sayQueue[sayIndex].face;
				s.text = sayQueue[sayIndex].text;
			}
			switch (s.phase)
			{
			case 0:
				s.ease += dt * 10.0f;
				if (s.ease >= 1.0f)
				{
					s.phase = 1;
					s.talking = s.text.size() > 3;
					if (!sayQueue[sayIndex].voice.empty())
						Audio::PlayVoice(sayQueue[sayIndex].voice.c_str());
				}
				break;
			case 1:
				s.counter += 30 * dt;
				while (s.counter >= 1) { s.characters++; s.counter -= 1; }
				if (Controls::Confirm.Pressed() || Controls::Cancel.Pressed())
				{
					s.characters = (int)s.text.size();
					s.phase = 2;
					s.talking = false;
					Controls::Confirm.tPressed = Controls::Cancel.tPressed = 0;
					break;
				}
				if (s.characters >= (int)s.text.size()) { s.characters = (int)s.text.size(); s.phase = 2; s.talking = false; }
				break;
			case 2:
				if (Controls::Confirm.Pressed() || Controls::Cancel.Pressed())
				{
					Controls::Confirm.tPressed = Controls::Cancel.tPressed = 0;
					Audio::Play("ui_dialog_advance");
					s.phase = 3;
				}
				break;
			case 3:
				s.ease -= dt * 10.0f;
				if (s.ease <= 0)
				{
					saying = Saying();
					sayIndex++;
				}
				break;
			}
			return false;
		}

		case Step::Wait:
			stepTimer += dt;
			return stepTimer >= st.duration;

		case Step::LookAtLerp:
		{
			if (stepPhase == 0) { fromVec = Game::Camera.LookAt; stepPhase = 1; stepTimer = 0; }
			float t = fminf(stepTimer, 1.0f);
			Game::Camera.LookAt = Lerp(fromVec, st.target, -(cosf(PI * t) - 1) / 2);
			stepTimer += dt / st.duration;
			return stepTimer >= 1.0f;
		}

		case Step::PositionLerp:
		{
			if (stepPhase == 0) { fromVec = Game::Camera.Position; stepPhase = 1; stepTimer = 0; }
			float t = fminf(stepTimer, 1.0f);
			Game::Camera.Position = Lerp(fromVec, st.target, -(cosf(PI * t) - 1) / 2);
			stepTimer += dt / st.duration;
			return stepTimer >= 1.0f;
		}

		case Step::Unlock:
			if (unlocking)
			{
				unlocking->locked = false;
				Audio::Play("sfx_berry_appear", unlocking->position);
			}
			return true;

		case Step::Wiggle:
		{
			stepTimer += dt / st.duration;
			float t = stepTimer;
			if (unlocking)
				unlocking->scaleMultiplier = t < 1 ? 1 + sinf(t * TAU * 3) * .3f * (1 - t) : 1;
			return t >= 1;
		}

		case Step::RestoreCamera:
			Game::Camera.LookAt = wasLookAt;
			Game::Camera.Position = wasPosition;
			unlocking = nullptr;
			return true;

		case Step::IncFlag:
			Save.record.IncFlag(talkFlag(npcs[st.actor].type));
			for (auto& n : npcs) n.CheckForDialog();
			return true;
		}
		return true;
	}

	void Update(float dt)
	{
		if (!active) return;
		timer += dt;
		if (easePhase == 0)
		{
			if (ease == 0) Audio::Play("sfx_readsign_in");
			ease = Calc::Approach(ease, 1, dt * 10);
			if (ease >= 1) easePhase = 1;
			return;
		}
		if (easePhase == 1)
		{
			while (stepIndex < steps.size())
			{
				if (!runStep(steps[stepIndex], dt))
					return;
				stepIndex++;
				stepPhase = 0;
				stepTimer = 0;
			}
			// (el original intenta bajar las franjas con "while (ease < 1)", que nunca corre: desaparecen de golpe)
			if (!freeze) Audio::Play("sfx_readsign_out");
			active = false;
			freeze = false;
			talkingNpc = nullptr;
		}
	}

	// ---------------------------------------------------------------- personajes
	void InitNpcs()
	{
		Dialog::Load();
		bool needGranny = false, needTheo = false, needBad = false, needSign = false;
		for (auto& n : npcs)
		{
			needGranny |= n.type == Npc::Granny;
			needTheo |= n.type == Npc::Theo;
			needBad |= n.type == Npc::Badeline;
			needSign |= n.type == Npc::Sign;
		}
		if (needGranny && !grannyData.verts) grannyData.Load("granny");
		if (needTheo && !theoData.verts) theoData.Load("theo");
		if (needBad && !badelineData.verts) badelineData.Load("badeline");
		if (needSign && !signData.verts) signData.Load("sign");

		for (auto& n : npcs)
		{
			const ModelData* d = n.type == Npc::Granny ? &grannyData : n.type == Npc::Theo ? &theoData :
				n.type == Npc::Badeline ? &badelineData : &signData;
			if (!d->verts) continue;
			n.model = new SkinnedModel(d);
			n.model->Play("idle");                      // NPC: Model.Play("idle")
			for (auto& e : n.model->MaterialEffects) e = 0.70f;
			n.interactHoverOffset = Vec3(0, -2, 16);
			n.interactRadius = 32;
			n.transform = Mat4::Translate(Vec3(0, 0, -1.5f)) * Mat4::Scale(3);
			if (n.type == Npc::Sign)
			{
				n.transform = Mat4::Translate(Vec3(0, 0, -1.5f)) * Mat4::Scale(4);
				n.interactHoverOffset = Vec3(0, 0, 16);
				n.interactRadius = 16;
				n.pushoutRadius = 6;
			}
			if (n.type == Npc::Badeline)
			{
				n.model->Play("Bad.Idle");
				for (size_t i = 0; i < d->materials.size(); i++)
					if (!strcmp(d->materials[i].name, "Hair"))
					{
						n.model->MaterialColors[i] = 0x9B3FB5;
						n.model->MaterialEffects[i] = 0;
					}
				n.hasHair = true;
				n.hair.Init();
				n.hair.Color = 0x9B3FB5;
				n.hair.ForwardOffsetPerNode = 0;
				n.hair.Nodes = 10;
			}
			n.CheckForDialog();
		}
	}

	void UpdateNpcs(float dt)
	{
		for (auto& n : npcs)
		{
			if (!n.model) continue;
			if (Actors::IsVisible(n.position + Vec3(0, 0, 4), 12))
				n.model->Update(dt);
			n.showTimer += dt;
			if (n.type == Npc::Badeline)
			{
				n.transform = Mat4::Translate(Vec3(0, 0, sinf(Actors::GeneralTimer * 2) * 1.0f - 1.5f)) * Mat4::Scale(3);
				static int head = -2;
				if (head == -2) head = n.model->Data->JointIndex("Head");
				Mat4 hm = n.ActorMatrix() * n.transform * Mat4::RotateX(PI / 2) * (head >= 0 ? n.model->JointMatrix(head) : Mat4::Identity());
				n.hair.Forward = -Vec3(n.facing, 0);
				n.hair.Update(dt, hm);
			}
		}
	}

	void LateUpdateNpcs()
	{
		for (auto& n : npcs)
		{
			if (!n.showHover && n.isPlayerOver)
				Audio::Play("ui_npc_popup");
			n.showHover = n.isPlayerOver;
			n.isPlayerOver = false;
		}
	}

	void CollectSprites(Vec3 camLeft, Vec3 camUp)
	{
		static C3D_Tex* interact = Gfx::GetTexture("spr_interact", false);
		for (auto& n : npcs)
		{
			if (!n.model) continue;
			if (Actors::IsVisible(n.position, 16))
				AddShadowSprite(n.position + Vec3::UnitZ(), 1);
			if (n.showHover)
			{
				Vec3 at = n.ActorMatrix().TransformPoint(n.interactHoverOffset) + Vec3::UnitZ() * sinf(n.showTimer * 8);
				Sprites::Billboard(interact, at, 2, Color4(), camLeft, camUp, true);
			}
		}
	}

	void DrawNpcs()
	{
		for (auto& n : npcs)
		{
			if (!n.model || !Actors::IsVisible(n.position + Vec3(0, 0, 6), 14)) continue;
			n.model->Draw(n.ActorMatrix() * n.transform * Mat4::RotateX(PI / 2));
			if (n.hasHair)
			{
				Gfx::BindScene();
				n.hair.Draw();
			}
		}
	}

	// ---------------------------------------------------------------- Cutscene.RenderUI
	static C3D_Tex* faceTexture(const std::string& face, bool talking)
	{
		// Faces/<personaje>/<Expresión>{Talk|Idle}{00|01}
		std::string src = "face_" + face;
		for (auto& c : src) if (c == '/') c = '_';
		auto exists = [](const std::string& key) { return Gfx::GetTexture(key.c_str(), false) != nullptr; };
		bool oddFrame = fmodf(timer, 0.6f) >= 0.3f;   // Calc.BetweenInterval(timer, 0.3)
		if (talking && exists(src + "Talk00"))
			return Gfx::GetTexture((src + (oddFrame && exists(src + "Talk01") ? "Talk01" : "Talk00")).c_str(), false);
		if (!talking && exists(src + "Idle00"))
		{
			bool blink = fmodf(timer, 3) > 2.8f;
			return Gfx::GetTexture((src + (blink && exists(src + "Idle01") ? "Idle01" : "Idle00")).c_str(), false);
		}
		if (oddFrame && exists(src + "01"))
			return Gfx::GetTexture((src + "01").c_str(), false);
		if (exists(src + "00"))
			return Gfx::GetTexture((src + "00").c_str(), false);
		return exists(src) ? Gfx::GetTexture(src.c_str(), false) : nullptr;
	}

	void RenderUI()
	{
		if (!active || freeze) return;
		const float k = 240.0f / 360.0f;                   // el original dibuja a 640x360
		const float BarSize = 40 * k, PortraitSize = 128 * k, TopOffset = 100 * k, EaseOffset = 32 * k, Padding = 8 * k;
		const Color4 black(0, 0, 0, 1), white(1, 1, 1, 1);

		Sprites::Rect2D(0, 0, 400, BarSize * ease, black);
		Sprites::Rect2D(0, 240 - BarSize * ease, 400, BarSize * ease, black);

		if (saying.ease > 0 && !saying.text.empty())
		{
			float e = Ease::CubeOut(Calc::Clamp(saying.ease, 0, 1));
			Vec2 size = Sprites::TextSize(saying.text.c_str());
			Vec2 pos = Vec2(200, TopOffset) - size / 2 - Vec2(Padding, Padding) + Vec2(0, EaseOffset * (1 - e));
			C3D_Tex* face = saying.face.empty() ? nullptr : faceTexture(saying.face, saying.talking);
			if (face) pos.x += PortraitSize / 3;
			float bw = size.x + Padding * 2, bh = size.y + Padding * 2;
			Sprites::RectRounded2D(pos.x, pos.y + 1, bw, bh, 2, black);
			Sprites::RectRounded2D(pos.x, pos.y, bw, bh, 2, white);
			if (face)
				Sprites::Image2D(face, pos.x - PortraitSize * 0.8f, pos.y + bh / 2 - PortraitSize / 2 - 10 * k, PortraitSize, PortraitSize, white);
			Sprites::Text2D(saying.text.c_str(), pos.x + Padding, pos.y + Padding, black, false, saying.characters);
		}
	}
}
