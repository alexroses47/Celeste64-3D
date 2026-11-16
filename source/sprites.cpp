#include "sprites.h"
#include "gfx.h"
#include <stdio.h>
#include <string.h>
#include <vector>

#include "sprite_shbin.h"

void logf_(const char* fmt, ...);

namespace Sprites
{
	struct SVertex { float pos[3], uv[2]; u8 color[4]; };
	struct Batch { C3D_Tex* tex; u32 first, count; };

	static constexpr u32 PoolVerts = 16384;
	static DVLB_s* dvlb;
	static shaderProgram_s program;
	static int uProj, uModelView;
	static SVertex* pools[2];
	static int poolIndex;
	static u32 cursor;
	static std::vector<Batch> batches3D, batchesPost;
	static C3D_Tex* pendTex;
	static u32 pendFirst, pendCount;
	static C3D_Tex whiteTex;

	struct Glyph { u32 code; float x, y, w, advance; };
	static std::vector<Glyph> glyphs;
	static C3D_Tex* fontTex;
	static C3D_Tex* fontOutlineTex;
	static float fontTexW = 1, fontTexH = 1, fontH = 1;

	C3D_Tex* White() { return &whiteTex; }

	void Init()
	{
		dvlb = DVLB_ParseFile((u32*)sprite_shbin, sprite_shbin_size);
		shaderProgramInit(&program);
		shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
		uProj = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
		uModelView = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
		for (auto& p : pools)
			p = (SVertex*)linearAlloc(sizeof(SVertex) * PoolVerts);

		C3D_TexInit(&whiteTex, 8, 8, GPU_RGBA8);
		memset(whiteTex.data, 0xFF, whiteTex.size);
		GSPGPU_FlushDataCache(whiteTex.data, whiteTex.size);
		C3D_TexSetFilter(&whiteTex, GPU_NEAREST, GPU_NEAREST);

		// fuente del juego (tools/convert.py: convert_font)
		FILE* f = fopen("romfs:/font_ui.bin", "rb");
		if (f)
		{
			u32 h[4];
			fread(h, 4, 4, f);
			fontTexW = (float)h[1];
			fontTexH = (float)h[2];
			fontH = (float)h[3];
			glyphs.resize(h[0]);
			for (auto& g : glyphs)
			{
				fread(&g.code, 4, 1, f);
				fread(&g.x, 4, 4, f);
			}
			fclose(f);
		}
		fontTex = Gfx::GetTexture("font_ui", false);
		fontOutlineTex = Gfx::GetTexture("font_ui_outline", false);
	}

	void BeginFrame()
	{
		poolIndex ^= 1;
		cursor = 0;
		batches3D.clear();
		batchesPost.clear();
	}

	// la GPU lee memoria lineal sin pasar por la caché del CPU
	static void flush(u32 first, u32 count)
	{
		GSPGPU_FlushDataCache(&pools[poolIndex][first], count * sizeof(SVertex));
	}

	static SVertex* alloc(u32 n)
	{
		if (cursor + n > PoolVerts) return nullptr;
		SVertex* v = &pools[poolIndex][cursor];
		cursor += n;
		return v;
	}

	static void setVertex(SVertex& v, Vec3 p, float u, float t, Color4 c)
	{
		v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
		v.uv[0] = u; v.uv[1] = t;
		v.color[0] = (u8)(Calc::Clamp(c.r, 0, 1) * 255);
		v.color[1] = (u8)(Calc::Clamp(c.g, 0, 1) * 255);
		v.color[2] = (u8)(Calc::Clamp(c.b, 0, 1) * 255);
		v.color[3] = (u8)(Calc::Clamp(c.a, 0, 1) * 255);
	}

	// dos triángulos A-B-C, A-C-D; esquinas de textura como Foster (arriba-izq, arriba-der, abajo-der, abajo-izq)
	static bool pushQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, float u0, float t0, float u1, float t1, Color4 col, u32& first)
	{
		SVertex* v = alloc(6);
		if (!v) return false;
		first = (u32)(v - pools[poolIndex]);
		setVertex(v[0], a, u0, t0, col);
		setVertex(v[1], b, u1, t0, col);
		setVertex(v[2], c, u1, t1, col);
		setVertex(v[3], a, u0, t0, col);
		setVertex(v[4], c, u1, t1, col);
		setVertex(v[5], d, u0, t1, col);
		return true;
	}

	void Clear3D() { batches3D.clear(); batchesPost.clear(); }

	void Quad(C3D_Tex* tex, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Color4 color, bool post)
	{
		if (!tex) return;
		u32 first;
		if (!pushQuad(a, b, c, d, 0, 1, 1, 0, color, first)) return;
		std::vector<Batch>& list = post ? batchesPost : batches3D;
		if (!list.empty() && list.back().tex == tex && list.back().first + list.back().count == first)
			list.back().count += 6;
		else
			list.push_back({ tex, first, 6 });
	}

	void QuadUV(C3D_Tex* tex, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec2 uvA, Vec2 uvC, Color4 color)
	{
		if (!tex) return;
		u32 first;
		if (!pushQuad(a, b, c, d, uvA.x, uvA.y, uvC.x, uvC.y, color, first)) return;
		batches3D.push_back({ tex, first, 6 });
	}

	void Billboard(C3D_Tex* tex, Vec3 at, float size, Color4 color, Vec3 camLeft, Vec3 camUp, bool post)
	{
		Vec3 left = camLeft * size, up = camUp * size;
		Quad(tex, at + left + up, at - left + up, at - left - up, at + left - up, color, post);
	}

	static void bindSprites(const C3D_Mtx* proj, const C3D_Mtx* modelView)
	{
		C3D_BindProgram(&program);
		Gfx::MarkUnbound();
		C3D_AttrInfo* attr = C3D_GetAttrInfo();
		AttrInfo_Init(attr);
		AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
		AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
		AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);
		C3D_BufInfo* buf = C3D_GetBufInfo();
		BufInfo_Init(buf);
		BufInfo_Add(buf, pools[poolIndex], sizeof(SVertex), 3, 0x210);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProj, proj);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uModelView, modelView);

		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		for (int i = 1; i < 4; i++)
			C3D_TexEnvInit(C3D_GetTexEnv(i));

		// SpriteRenderer: BlendMode.Premultiply, sin descarte por alfa
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
		C3D_AlphaTest(false, GPU_ALWAYS, 0);
		C3D_CullFace(GPU_CULL_NONE);
	}

	static void restoreState()
	{
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		C3D_AlphaTest(true, GPU_GREATER, 0x7F);
	}

	void Flush3D()
	{
		if (batches3D.empty() && batchesPost.empty()) return;
		bindSprites(&Gfx::projection, &Gfx::view);
		flush(0, cursor);
		// DepthCompare.Less sin escritura (aquí la profundidad va al revés: GREATER)
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		for (const Batch& b : batches3D)
		{
			C3D_TexBind(0, b.tex);
			C3D_DrawArrays(GPU_TRIANGLES, b.first, b.count);
		}
		// Post: DepthCompare.Always
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		for (const Batch& b : batchesPost)
		{
			C3D_TexBind(0, b.tex);
			C3D_DrawArrays(GPU_TRIANGLES, b.first, b.count);
		}
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		restoreState();
	}

	// ---------------------------------------------------------------- 2D
	static C3D_Mtx ortho, identity;

	void Begin2D()
	{
		pendCount = 0;
		Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
		Mtx_Identity(&identity);
		bindSprites(&ortho, &identity);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	}

	void Begin2DBottom()
	{
		pendCount = 0;
		Mtx_OrthoTilt(&ortho, 0.0f, 320.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
		Mtx_Identity(&identity);
		bindSprites(&ortho, &identity);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
	}

	void Begin2DTarget(float width, float height)
	{
		pendCount = 0;
		Mtx_Ortho(&ortho, 0.0f, width, 0.0f, height, 1.0f, -1.0f, true);
		Mtx_Identity(&identity);
		bindSprites(&ortho, &identity);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
	}

	void SetAdditive(bool on)
	{
		Flush2D();
		if (on) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
		else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
	}



	// lote 2D pendiente: se dibuja de una vez al cambiar de textura o de estado (como el Batcher de Foster)

	void Flush2D()
	{
		if (!pendCount) return;
		flush(pendFirst, pendCount);
		C3D_TexBind(0, pendTex);
		C3D_DrawArrays(GPU_TRIANGLES, pendFirst, pendCount);
		pendCount = 0;
	}

	static void queue2D(C3D_Tex* tex, u32 first, u32 count)
	{
		if (tex != pendTex || first != pendFirst + pendCount)
		{
			Flush2D();
			pendTex = tex;
			pendFirst = first;
		}
		pendCount += count;
	}

	static void draw2D(C3D_Tex* tex, Vec2 a, Vec2 b, Vec2 c, Vec2 d, float u0, float t0, float u1, float t1, Color4 col)
	{
		u32 first;
		if (!tex || !pushQuad(Vec3(a, 0), Vec3(b, 0), Vec3(c, 0), Vec3(d, 0), u0, t0, u1, t1, col, first)) return;
		queue2D(tex, first, 6);
	}

	void Image2DUV(C3D_Tex* tex, Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec2 uvA, Vec2 uvC, Color4 color)
	{
		draw2D(tex, a, b, c, d, uvA.x, uvA.y, uvC.x, uvC.y, color);
	}

	void Rect2D(float x, float y, float w, float h, Color4 color)
	{
		draw2D(&whiteTex, Vec2(x, y), Vec2(x + w, y), Vec2(x + w, y + h), Vec2(x, y + h), 0, 1, 1, 0, color);
	}

	void Triangle2D(Vec2 a, Vec2 b, Vec2 c, Color4 color)
	{
		SVertex* v = alloc(3);
		if (!v) return;
		u32 first = (u32)(v - pools[poolIndex]);
		setVertex(v[0], Vec3(a, 0), 0, 0, color);
		setVertex(v[1], Vec3(b, 0), 0, 0, color);
		setVertex(v[2], Vec3(c, 0), 0, 0, color);
		queue2D(&whiteTex, first, 3);
	}

	void Image2D(C3D_Tex* tex, float x, float y, float w, float h, Color4 color)
	{
		draw2D(tex, Vec2(x, y), Vec2(x + w, y), Vec2(x + w, y + h), Vec2(x, y + h), 0, 1, 1, 0, color);
	}

	static const Glyph* findGlyph(char ch)
	{
		unsigned c = (unsigned char)ch;
		if (c >= 32 && c - 32 < glyphs.size() && glyphs[c - 32].code == c) return &glyphs[c - 32];
		for (const Glyph& g : glyphs)
			if (g.code == c) return &g;
		return nullptr;
	}

	float LineHeight() { return fontH; }

	float TextWidth(const char* text)
	{
		return TextSize(text).x;
	}

	Vec2 TextSize(const char* text)
	{
		float w = 0, line = 0;
		int lines = 1;
		for (const char* c = text; *c; c++)
		{
			if (*c == '\n') { w = fmaxf(w, line); line = 0; lines++; continue; }
			const Glyph* g = findGlyph(*c);
			line += g ? g->advance : fontH * 0.4f;
		}
		return Vec2(fmaxf(w, line), lines * fontH);
	}

	static void textPass(const char* text, float x, float y, Color4 color, int maxChars, float scale = 1, bool outline = false)
	{
		C3D_Tex* tex = outline ? fontOutlineTex : fontTex;
		const float pad = outline ? 1.0f : 0.0f;   // el contorno se sale 1 px a cada lado
		float x0 = x;
		int n = 0;
		for (const char* c = text; *c && (maxChars < 0 || n < maxChars); c++, n++)
		{
			if (*c == '\n') { x = x0; y += fontH * scale; continue; }
			const Glyph* g = findGlyph(*c);
			if (!g) { x += fontH * 0.4f * scale; continue; }
			if (*c != ' ')
			{
				float u0 = (g->x - pad) / fontTexW, u1 = (g->x + g->w + pad) / fontTexW;
				float t0 = 1.0f - g->y / fontTexH, t1 = 1.0f - (g->y + fontH) / fontTexH;
				float w = (g->w + 2 * pad) * scale, h = fontH * scale, x0 = x - pad * scale;
				draw2D(tex, Vec2(x0, y), Vec2(x0 + w, y), Vec2(x0 + w, y + h), Vec2(x0, y + h), u0, t0, u1, t1, color);
			}
			x += g->advance * scale;
		}
	}

	float Text2D(const char* text, float x, float y, Color4 color, bool outline, int maxChars, float scale)
	{
		// UI.Text: contorno negro (ya dibujado en la tira de contorno) y el texto encima
		if (outline)
			textPass(text, x, y, Color4(1, 1, 1, 1) * color.a, maxChars, scale, true);
		textPass(text, x, y, color, maxChars, scale);
		return TextWidth(text) * scale;
	}

	void RectRounded2D(float x, float y, float w, float h, float r, Color4 color)
	{
		Rect2D(x, y + r, w, h - 2 * r, color);
		Rect2D(x + r, y, w - 2 * r, r, color);
		Rect2D(x + r, y + h - r, w - 2 * r, r, color);
	}

	void End2D()
	{
		Flush2D();
		restoreState();
	}
}
