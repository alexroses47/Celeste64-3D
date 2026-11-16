#include "scenes.h"
#include "audio.h"
#include "gfx.h"
#include "input.h"
#include "menu.h"
#include "model.h"
#include "save.h"
#include "sprites.h"
#include <stdio.h>

static constexpr float K = 240.0f / 360.0f;   // el original dibuja a 640x360
static constexpr float FOCAL_UI = 80.0f;

static Vec2 textureSize(const char* key)
{
	// tools/convert.py guarda el tamaño original de las imágenes que estira a potencia de 2
	char path[96];
	snprintf(path, sizeof(path), "romfs:/tex/%s.size", key);
	FILE* f = fopen(path, "r");
	int w = 1, h = 1;
	if (f) { fscanf(f, "%d %d", &w, &h); fclose(f); }
	return Vec2((float)w, (float)h);
}

static void lookAt(C3D_Mtx* out, Vec3 eye, Vec3 target)
{
	Mtx_LookAt(out, FVec3_New(eye.x, eye.y, eye.z), FVec3_New(target.x, target.y, target.z), FVec3_New(0, 0, 1), false);
}

// ---------------------------------------------------------------- partes compartidas
namespace UI
{
	void Overlay(double elapsed)
	{
		static C3D_Tex* overlay = nullptr;
		static C3D_Tex* vignette = nullptr;
		if (!overlay)
		{
			overlay = Gfx::GetTexture("ui_overlay", true);
			C3D_TexSetFilter(overlay, GPU_LINEAR, GPU_LINEAR);
			vignette = Gfx::GetTexture("ui_vignette", false);
		}
		Vec2 scroll = -Vec2(1.25f, 0.9f) * (float)(elapsed * 0.05);
		Sprites::SetAdditive(true);
		Sprites::Image2DUV(overlay, Vec2(0, 0), Vec2(400, 0), Vec2(400, 240), Vec2(0, 240),
			Vec2(scroll.x, 1 - scroll.y), Vec2(scroll.x + 1, -scroll.y), Color4() * 0.10f);
		Sprites::SetAdditive(false);
		Sprites::Image2D(vignette, 0, 0, 400, 240, Color4() * 0.30f);
	}

	// UI.Prompt: ícono del botón + texto, alineados a la derecha abajo
	void Prompts(const char* confirm, const char* cancel)
	{
		static C3D_Tex* iconA = Gfx::GetTexture("ui_confirm", false);
		static C3D_Tex* iconB = Gfx::GetTexture("ui_cancel", false);
		const float size = 28 * K;
		float x = 400 - 16 * K, y = 240 - 4 * K - size;
		auto prompt = [&](C3D_Tex* icon, const char* label)
		{
			float w = Sprites::TextWidth(label);
			x -= w;
			Sprites::Text2D(label, x, y + size / 2 - Sprites::LineHeight() / 2, Color4());
			x -= size;
			Sprites::Image2D(icon, x, y, size, size, Color4());
			x -= 8 * K;
		};
		prompt(iconB, cancel);
		prompt(iconA, confirm);
	}

	void Version()
	{
		// la licencia del contenido pide que quede claro que no es del equipo de Celeste
		float y = 240 - 4 * K - Sprites::LineHeight();
		Sprites::Text2D("3DS port by alexroses47 - unofficial fan port", 4 * K, y - Sprites::LineHeight(), Color4() * 0.25f);
		Sprites::Text2D("v1.2.0 3DS", 4 * K, y, Color4() * 0.25f);
	}
}

// ---------------------------------------------------------------- Titlescreen.cs
namespace Title
{
	static ModelData logoData;
	static SkinnedModel* logo;
	static float easing, inputDelay, elapsed;
	static Vec2 wobble;

	// créditos del port: solo al encender, antes del título (A los salta)
	constexpr float SplashTime = 3.5f;
	static float splash = SplashTime;

	void Enter()
	{
		if (!logo && logoData.Load("logo"))
			logo = new SkinnedModel(&logoData);
		easing = 0;
		inputDelay = 5.0f;
		elapsed = 0;
		wobble = Vec2();
		if (splash <= 0)
			Audio::PlayMusic("mus_title");
	}

	Result Update(float dt)
	{
		if (splash > 0)
		{
			if (Controls::Confirm.ConsumePress() && splash < SplashTime - 0.5f)
				splash = 0.5f;
			splash -= dt;
			if (splash <= 0)
				Audio::PlayMusic("mus_title");
			return None;
		}
		elapsed += dt;
		easing = Calc::Approach(easing, 1, dt / 5.0f);
		inputDelay = Calc::Approach(inputDelay, 0, dt);
		wobble += (Controls::Camera - wobble) * (1 - powf(.1f, dt));
		if (Controls::Confirm.ConsumePress())
		{
			Audio::Play("main_menu_first_input");
			return ToOverworld;
		}
		if (Controls::Cancel.ConsumePress())
			return Exit;
		return None;
	}

	static void renderSplash()
	{
		Sprites::Begin2D();
		Sprites::Rect2D(0, 0, 400, 240, Color4(0, 0, 0, 1));
		float t = SplashTime - splash;
		float a = Calc::Clamp(fminf(t / 0.5f, splash / 0.5f), 0, 1);
		auto centered = [&](const char* text, float y, float scale, Color4 color)
		{
			Sprites::Text2D(text, 200 - Sprites::TextWidth(text) * scale / 2, y, color * a, true, -1, scale);
		};
		centered("Celeste 64", 72, 2.0f, Color4(1, 1, 1, 1));
		centered("3DS port by alexroses47", 112, 1.0f, Color4(1, 1, 1, 1));
		centered("Unofficial fan port.", 160, 1.0f, Color4(0.6f, 0.6f, 0.6f, 1));
		centered("Not made or endorsed by the Celeste team.", 176, 1.0f, Color4(0.6f, 0.6f, 0.6f, 1));
		Sprites::End2D();
	}

	void Render(float iod)
	{
		if (splash > 0)
		{
			renderSplash();
			return;
		}
		// cámara: de (0,-200,60) a (0,-80,50) en 5 s, mirando a (0,0,70)
		Vec3 camFrom(0, -200, 60), camTo(0, -80, 50);
		Vec3 eye = Lerp(camFrom, camTo, Ease::CubeOut(easing));
		Mtx_PerspStereoTilt(&Gfx::projection, C3D_AngleFromDegrees(45), C3D_AspectRatioTop, 10, 300, iod, FOCAL_UI, false);
		lookAt(&Gfx::view, eye, Vec3(0, 0, 70));

		if (logo)
		{
			// ModelMatrix = Scale(10) * RotX(wobble.y) * RotZ(wobble.x) * T(0,0,53) * RotZ(-(1-ease)*10)
			Mat4 m = Mat4::RotateZ(-(1.0f - Ease::CubeOut(easing)) * 10) * Mat4::Translate(Vec3(0, 0, 53)) *
				Mat4::RotateZ(wobble.x) * Mat4::RotateX(wobble.y) * Mat4::Scale(10) * Mat4::RotateX(PI / 2);
			Gfx::SetSun(-Vec3::UnitZ());
			Gfx::SetFogColor(0xffffff);
			C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
			C3D_CullFace(GPU_CULL_NONE);
			logo->Draw(m);
			Gfx::SetSun(Vec3(0, -0.7f, -1.0f));
			Gfx::SetFogColor(0xdceaf0);
		}

		Sprites::Begin2D();
		UI::Overlay(elapsed);
		if (inputDelay <= 0)
		{
			UI::Prompts("Confirm", "Exit");
			UI::Version();
		}
		// aparece desde negro (BlendMode.Subtract con blanco)
		if (easing < 1)
			Sprites::Rect2D(0, 0, 400, 240, Color4(0, 0, 0, 1) * (1 - Ease::CubeOut(easing)));
		Sprites::End2D();
	}
}

// ---------------------------------------------------------------- Overworld.cs
namespace Overworld
{
	// la postal mide 480x320 a 360p: 320x213 en la 3DS, dentro de una textura de 512x256
	static constexpr float CardW = 480 * K, CardH = 320 * K;
	static C3D_Tex cardTex;
	static C3D_RenderTarget* cardTarget;

	enum State { Selecting, Selected, Entering, Restarting };
	static State state;
	static float slide, selectedEase, cameraCloseUpEase, highlightEase, selectionEase, elapsed;
	static Vec2 wobble(0, -1);
	static Menu entryMenu, restartMenu;
	static bool hasRecord, complete, restart;

	static void buildMenus()
	{
		entryMenu = Menu();
		hasRecord = Save.record.checkpoint > 0 || !Save.record.strawberries.empty() || Save.record.deaths > 0 || Save.record.time > 1;
		if (hasRecord)
		{
			entryMenu.AddOption("Continue", nullptr);
			entryMenu.AddOption("Restart", nullptr);
		}
		else
			entryMenu.AddOption("Start", nullptr);
		complete = Save.record.strawberries.size() >= 30;   // Levels.json: Strawberries = 30
		restartMenu = Menu();
		restartMenu.AddOption("Cancel", nullptr);
		restartMenu.AddOption("Clear Save & Restart Level", nullptr);
	}

	void Enter(bool startOnLastSelected)
	{
		if (!cardTarget)
		{
			C3D_TexInitVRAM(&cardTex, 512, 256, GPU_RGBA8);
			C3D_TexSetFilter(&cardTex, GPU_LINEAR, GPU_LINEAR);
			cardTarget = C3D_RenderTargetCreateFromTex(&cardTex, GPU_TEXFACE_2D, 0, -1);
		}
		buildMenus();
		state = Selecting;
		slide = 0;
		selectedEase = 0;
		highlightEase = 0;
		selectionEase = 0;
		restart = false;
		if (startOnLastSelected)
		{
			state = Selected;
			selectedEase = 1;
			highlightEase = selectionEase = 1;
		}
		cameraCloseUpEase = 1.0f;
		Audio::PlayMusic("mus_title");
	}

	bool RestartRequested() { return restart; }

	Result Update(float dt)
	{
		elapsed += dt;
		wobble += (Controls::Camera - wobble) * (1 - powf(.1f, dt));
		Calc::ApproachRef(cameraCloseUpEase, state == Entering ? 1 : 0, dt);
		Calc::ApproachRef(selectedEase, state != Selecting ? 1 : 0, 8 * dt);
		Calc::ApproachRef(highlightEase, 1.0f, dt * 8.0f);   // una sola postal: siempre resaltada
		Calc::ApproachRef(selectionEase, (state == Selected || state == Restarting) ? 1.0f : 0.0f, dt * 4.0f);

		u32 d = hidKeysDown();
		bool up = d & (KEY_DUP | KEY_CPAD_UP), down = d & (KEY_DDOWN | KEY_CPAD_DOWN);
		entryMenu.Focused = state == Selected;
		if (selectionEase >= 0.5f && state == Selected)
			entryMenu.Update(dt, up, down, false, false);

		switch (state)
		{
		case Selecting:
			if (Controls::Confirm.ConsumePress())
			{
				state = Selected;
				entryMenu.Index = 0;
				Audio::Play("main_menu_postcard_flip");
			}
			else if (Controls::Cancel.ConsumePress())
				return ToTitle;
			break;
		case Selected:
			if (Controls::Confirm.ConsumePress() && selectionEase > 0.5f)
			{
				if (hasRecord && entryMenu.Index == 1)
				{
					Audio::Play("main_menu_restart_confirm_popup");
					restartMenu.Index = 0;
					state = Restarting;
				}
				else
				{
					Audio::Play("main_menu_start_game");
					Audio::StopMusic();
					state = Entering;
				}
			}
			else if (Controls::Cancel.ConsumePress())
			{
				Audio::Play("main_menu_postcard_flip_back");
				state = Selecting;
			}
			break;
		case Restarting:
			restartMenu.Update(dt, up, down, false, false);
			if (Controls::Confirm.ConsumePress())
			{
				if (restartMenu.Index == 1)
				{
					Audio::Play("main_menu_start_game");
					Audio::StopMusic();
					restart = true;
					state = Entering;
				}
				else
				{
					Audio::Play("main_menu_restart_cancel");
					state = Selected;
				}
			}
			else if (Controls::Cancel.ConsumePress())
			{
				Audio::Play("main_menu_restart_cancel");
				state = Selected;
			}
			break;
		case Entering:
			if (cameraCloseUpEase >= 1.0f)
				return EnterLevel;
			break;
		}
		return None;
	}

	// Entry.Redraw: imagen del frente o del reverso (los textos se dibujan encima en Render)
	void PrepareTextures()
	{
		static C3D_Tex* front = Gfx::GetTexture("ui_postcard", false);
		static C3D_Tex* back = Gfx::GetTexture("ui_postcard_back", false);
		static C3D_Tex* completeTex = Gfx::GetTexture("ui_complete", false);
		static Vec2 frontSize = textureSize("ui_postcard"), backSize = textureSize("ui_postcard_back");
		static Vec2 completeSize = textureSize("ui_complete");

		C3D_RenderTargetClear(cardTarget, C3D_CLEAR_ALL, 0x00000000, 0);
		C3D_FrameDrawOn(cardTarget);
		Sprites::Begin2DTarget(512, 256);
		// la postal va arriba a la izquierda de la textura; en esta proyección +Y es hacia arriba
		const float top = 256;
		bool backSide = selectionEase >= 0.5f;
		C3D_Tex* img = backSide ? back : front;
		Vec2 isz = backSide ? backSize : frontSize;
		float scale = fmaxf(CardW / isz.x, CardH / isz.y);
		float w = isz.x * scale, h = isz.y * scale;
		float u0 = (w - CardW) / 2 / w, v0 = (h - CardH) / 2 / h;   // recorte para cubrir
		Sprites::Image2DUV(img, Vec2(0, top), Vec2(CardW, top), Vec2(CardW, top - CardH), Vec2(0, top - CardH),
			Vec2(u0, 1 - v0), Vec2(1 - u0, v0), Color4());
		if (!backSide && complete && completeTex)
		{
			// fresa de nivel completo, abajo a la derecha (escala 0.5)
			Vec2 csz = completeSize * 0.5f * K;
			float x = CardW - 50 * K - csz.x / 2, y = top - CardH;
			Sprites::Image2DUV(completeTex, Vec2(x, y + csz.y), Vec2(x + csz.x, y + csz.y), Vec2(x + csz.x, y), Vec2(x, y),
				Vec2(0, 1), Vec2(1, 0), Color4());
		}
		if (backSide)
			Sprites::Rect2D(0, top - CardH, CardW, CardH, Color4(0, 0, 0, 1) * 0.25f);
		Sprites::End2D();
	}

	void Render(float iod)
	{
		// cámara del original: (0, -100 + 30*CubeIn(closeUp), 0) mirando al origen
		Vec3 eye(0, -100 + 30 * Ease::CubeIn(cameraCloseUpEase), 0);
		Mtx_PerspStereoTilt(&Gfx::projection, C3D_AngleFromDegrees(45), C3D_AspectRatioTop, 1, 1000, iod, 90, false);
		lookAt(&Gfx::view, eye, Vec3());

		// la postal mide 80x53 en el plano XZ
		float shift = Ease::CubeIn(1.0f - highlightEase) * 30 - Ease::CubeIn(selectionEase) * 20;
		float t = selectionEase;
		float rotation = t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;   // Ease.Cube.InOut
		Mat4 m = Mat4::Translate(Vec3(-slide * 60, shift, 0)) *
			Mat4::RotateZ((state == Entering ? -1 : 1) * rotation * PI) *
			Mat4::RotateZ(wobble.x * highlightEase) * Mat4::RotateX(wobble.y * highlightEase) *
			Mat4::Scale(Vec3(selectionEase >= 0.5f ? -1.0f : 1.0f, 1, 1));
		float cw = CardW / 6.0f / K / 2, ch = CardH / 6.0f / K / 2;
		Vec3 a = m.TransformPoint(Vec3(-cw, 0, ch)), b = m.TransformPoint(Vec3(cw, 0, ch));
		Vec3 c = m.TransformPoint(Vec3(cw, 0, -ch)), d = m.TransformPoint(Vec3(-cw, 0, -ch));

		// la postal ocupa u en [0, 320/512] y las filas de arriba de la textura
		const float uMax = CardW / 512.0f, vBottom = 1.0f - CardH / 256.0f;
		Sprites::Clear3D();
		Sprites::QuadUV(&cardTex, a, b, c, d, Vec2(0, 1), Vec2(uMax, vBottom), Color4());
		Sprites::Flush3D();

		// dónde cae la postal en la pantalla (la cámara mira por +Y sin girar)
		const float focal = 120.0f / tanf(22.5f * DEG2RAD);
		auto project = [&](Vec3 p) { float z = p.y - eye.y; return Vec2(200 + p.x / z * focal, 120 - p.z / z * focal); };
		Vec2 tl = project(Vec3(-cw - slide * 60, shift, ch)), br = project(Vec3(cw - slide * 60, shift, -ch));
		float sw = br.x - tl.x, sh = br.y - tl.y;
		float k = sw / CardW;   // escala de la postal en pantalla respecto a su textura

		Sprites::Begin2D();
		const float pad = 16 * K * k;
		if (selectionEase < 0.5f && state != Entering)
		{
			// nombre del nivel a escala 2, abajo a la izquierda
			Sprites::Text2D("Forsaken City", tl.x + pad, br.y - pad - Sprites::LineHeight() * 2 * k, Color4(), true, -1, 2 * k);
		}
		else if (selectionEase >= 0.9f && state != Entering)
		{
			Sprites::Text2D("Created for the 6th anniversary\nby the Celeste Team", tl.x + 32 * K * k, tl.y + 24 * K * k,
				Color4(0, 0, 0, 1) * 0.7f, false, -1, k);
			// estadísticas (escala 1.3)
			static C3D_Tex* iconStrawb = Gfx::GetTexture("spr_icon_strawberry", false);
			static C3D_Tex* iconSkull = Gfx::GetTexture("spr_icon_skull", false);
			static C3D_Tex* iconTimer = Gfx::GetTexture("spr_icon_stopwatch", false);
			float s = 1.3f * k, isz = 30 * K * s, lh = Sprites::LineHeight() * s;
			float cx = (tl.x + br.x) / 2, cy = (tl.y + br.y) / 2 - pad;
			char buf[32];
			snprintf(buf, sizeof(buf), "x%02d", (int)Save.record.strawberries.size());
			float wS = isz * 0.7f + Sprites::TextWidth(buf) * s;
			float rowY = cy - isz / 2 - 4 * s - isz / 2;
			Sprites::Image2D(iconStrawb, cx - 8 * s - wS, rowY, isz, isz, Color4());
			Sprites::Text2D(buf, cx - 8 * s - wS + isz * 0.7f, rowY + isz / 2 - lh / 2, Color4(), true, -1, s);
			snprintf(buf, sizeof(buf), "x%03d", Save.record.deaths);
			Sprites::Image2D(iconSkull, cx + 8 * s, rowY, isz, isz, Color4());
			Sprites::Text2D(buf, cx + 8 * s + isz * 0.7f, rowY + isz / 2 - lh / 2, Color4(), true, -1, s);
			int ms = (int)(Save.record.time * 1000);
			int hh = ms / 3600000, mm = (ms / 60000) % 60, ss = (ms / 1000) % 60, mil = ms % 1000;
			if (hh > 0) snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%03d", hh, mm, ss, mil);
			else snprintf(buf, sizeof(buf), "%02d:%02d:%03d", mm, ss, mil);
			float wT = isz * 0.7f + Sprites::TextWidth(buf) * s;
			float rowT = cy + 4 * s;
			Sprites::Image2D(iconTimer, cx - wT / 2, rowT, isz, isz, Color4());
			Sprites::Text2D(buf, cx - wT / 2 + isz * 0.7f, rowT + isz / 2 - lh / 2, Color4(), true, -1, s);
			// menú: abajo al centro
			entryMenu.Render(cx, br.y - 8 * K * k - Sprites::LineHeight() * (hasRecord ? 1.2f : 0.6f));
		}
		(void)sh;

		if (state == Restarting)
		{
			Sprites::Rect2D(0, 0, 400, 240, Color4(0, 0, 0, 1) * 0.90f);
			restartMenu.Render(200, 120);
		}
		UI::Overlay(elapsed);
		if (state != Entering)
		{
			UI::Prompts("Confirm", state == Selecting ? "Back" : "Cancel");
			UI::Version();
		}
		if (cameraCloseUpEase > 0)
			Sprites::Rect2D(0, 0, 400, 240, Color4(0, 0, 0, 1) * Ease::CubeIn(cameraCloseUpEase));
		Sprites::End2D();
	}
}
