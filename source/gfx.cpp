#include "gfx.h"
#include <tex3ds.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_map>

#include "scene_shbin.h"
#include "level_shbin.h"
#include "skinned_shbin.h"

void logf_(const char* fmt, ...);

namespace Gfx
{
	SceneShader scene;
	SkinnedShader skinned;
	C3D_Mtx projection, view;

	static DVLB_s* sceneDvlb;
	static DVLB_s* levelDvlb;
	LevelShader level;
	static DVLB_s* skinnedDvlb;
	static std::unordered_map<std::string, C3D_Tex*> textures;
	static int bound = 0;   // 1 = escenario, 2 = huesos

	// World.Render: SunDirection = normalize(0, -0.7, -1)
	static Vec3 Sun = Vec3(0, -0.7f, -1.0f).Normalized();
	static u32 FogABGR = 0xFFF0EADC;

	void SetSun(Vec3 dir) { Sun = dir.Normalized(); }
	void SetFogColor(u32 rgb) { FogABGR = ColorABGR(rgb); }

	void Init()
	{
		sceneDvlb = DVLB_ParseFile((u32*)scene_shbin, scene_shbin_size);
		shaderProgramInit(&scene.program);
		shaderProgramSetVsh(&scene.program, &sceneDvlb->DVLE[0]);
		scene.proj = shaderInstanceGetUniformLocation(scene.program.vertexShader, "projection");
		scene.modelView = shaderInstanceGetUniformLocation(scene.program.vertexShader, "modelView");
		scene.sun = shaderInstanceGetUniformLocation(scene.program.vertexShader, "sun");
		scene.params = shaderInstanceGetUniformLocation(scene.program.vertexShader, "params");
		scene.offset = shaderInstanceGetUniformLocation(scene.program.vertexShader, "offset");

		levelDvlb = DVLB_ParseFile((u32*)level_shbin, level_shbin_size);
		shaderProgramInit(&level.program);
		shaderProgramSetVsh(&level.program, &levelDvlb->DVLE[0]);
		level.mvp = shaderInstanceGetUniformLocation(level.program.vertexShader, "mvp");
		level.fog = shaderInstanceGetUniformLocation(level.program.vertexShader, "fog");
		level.lightScale = shaderInstanceGetUniformLocation(level.program.vertexShader, "lightScale");

		skinnedDvlb = DVLB_ParseFile((u32*)skinned_shbin, skinned_shbin_size);
		shaderProgramInit(&skinned.program);
		shaderProgramSetVsh(&skinned.program, &skinnedDvlb->DVLE[0]);
		skinned.proj = shaderInstanceGetUniformLocation(skinned.program.vertexShader, "projection");
		skinned.view = shaderInstanceGetUniformLocation(skinned.program.vertexShader, "view");
		skinned.model = shaderInstanceGetUniformLocation(skinned.program.vertexShader, "model");
		skinned.sun = shaderInstanceGetUniformLocation(skinned.program.vertexShader, "sun");
		skinned.params = shaderInstanceGetUniformLocation(skinned.program.vertexShader, "params");
		skinned.bones = shaderInstanceGetUniformLocation(skinned.program.vertexShader, "bones");

		C3D_AlphaTest(true, GPU_GREATER, 0x7F);
	}

	u32 ColorABGR(u32 rgb, float alpha)
	{
		u32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
		u32 a = (u32)(Calc::Clamp(alpha, 0, 1) * 255.0f);
		return (a << 24) | (b << 16) | (g << 8) | r;
	}

	static void lerpStage(int id, u32 color, GPU_TEVOP_RGB factor)
	{
		C3D_TexEnv* env = C3D_GetTexEnv(id);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
		C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, factor);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
		C3D_TexEnvColor(env, color);
	}

	void SetLitTexEnv(u32 materialColor)
	{
		// etapa 0: textura * color del material
		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
		C3D_TexEnvColor(env, materialColor);
		// sombra hacia (4,27,44) según color.a, brillo hacia blanco según color.r,
		// niebla vertical hacia 0xdceaf0 según color.g
		lerpStage(1, 0xFF2C1B04, GPU_TEVOP_RGB_SRC_ALPHA);
		lerpStage(2, 0xFFFFFFFF, GPU_TEVOP_RGB_SRC_R);
		lerpStage(3, FogABGR, GPU_TEVOP_RGB_SRC_G);
	}

	void SetUnlitTexEnv()
	{
		C3D_TexEnv* env = C3D_GetTexEnv(0);
		C3D_TexEnvInit(env);
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
		for (int i = 1; i < 4; i++)
			C3D_TexEnvInit(C3D_GetTexEnv(i));
	}

	void SetSceneParams(float effects)
	{
		C3D_FVUnifSet(GPU_VERTEX_SHADER, scene.sun, Sun.x, Sun.y, Sun.z, 0);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, scene.params, 1.0f / 127.0f, 1.0f / 50.0f, 0.8f * effects, 0.10f * effects);
	}

	void SetSkinnedParams(float effects)
	{
		C3D_FVUnifSet(GPU_VERTEX_SHADER, skinned.sun, Sun.x, Sun.y, Sun.z, 0);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, skinned.params, 1.0f / 127.0f, 1.0f / 50.0f, 0.8f * effects, 0.10f * effects);
	}

	void MarkUnbound() { bound = 0; }

	void SetSceneOffset(Vec3 o) { C3D_FVUnifSet(GPU_VERTEX_SHADER, scene.offset, o.x, o.y, o.z, 0); }

	Vec3 WorldSun() { return Vec3(0, -0.7f, -1.0f).Normalized(); }

	void BindLevel()
	{
		if (bound != 3)
		{
			C3D_BindProgram(&level.program);
			C3D_AttrInfo* attr = C3D_GetAttrInfo();
			AttrInfo_Init(attr);
			AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
			AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
			AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);
			bound = 3;
		}
		C3D_FVUnifSet(GPU_VERTEX_SHADER, level.lightScale, 0.10f / 255.0f, 0, 0, 0.80f / 255.0f);
	}

	void SetLevelTransform(const C3D_Mtx& modelView, float offsetZ)
	{
		C3D_Mtx mvp;
		Mtx_Multiply(&mvp, &projection, &modelView);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, level.mvp, &mvp);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, level.fog, -1.0f / 50.0f, 1.0f - offsetZ / 50.0f, 0, 0);
	}

	void BindScene()
	{
		if (bound != 1)
		{
			C3D_BindProgram(&scene.program);
			C3D_AttrInfo* attr = C3D_GetAttrInfo();
			AttrInfo_Init(attr);
			AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
			AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
			AttrInfo_AddLoader(attr, 2, GPU_BYTE, 4);
			bound = 1;
		}
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, scene.proj, &projection);
		SetSceneOffset(Vec3());
	}

	void BindSkinned()
	{
		if (bound != 2)
		{
			C3D_BindProgram(&skinned.program);
			C3D_AttrInfo* attr = C3D_GetAttrInfo();
			AttrInfo_Init(attr);
			AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
			AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
			AttrInfo_AddLoader(attr, 2, GPU_BYTE, 4);
			AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4);
			AttrInfo_AddLoader(attr, 4, GPU_UNSIGNED_BYTE, 4);
			bound = 2;
		}
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, skinned.proj, &projection);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, skinned.view, &view);
	}

	C3D_Tex* GetTexture(const char* key, bool repeat)
	{
		auto it = textures.find(key);
		if (it != textures.end())
			return it->second;

		char path[96];
		snprintf(path, sizeof(path), "romfs:/tex/%s.t3x", key);
		C3D_Tex* tex = new C3D_Tex();
		FILE* f = fopen(path, "rb");
		Tex3DS_Texture t3x = f ? Tex3DS_TextureImportStdio(f, tex, NULL, false) : NULL;
		if (f) fclose(f);
		if (!t3x)
		{
			logf_("no se pudo cargar %s\n", path);
			delete tex;
			textures[key] = nullptr;
			return nullptr;
		}
		Tex3DS_TextureFree(t3x);
		if (repeat)
		{
			// Materials.cs: Nearest + Repeat
			C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetFilterMipmap(tex, GPU_NEAREST);
			C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);
		}
		else
		{
			C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
		textures[key] = tex;
		return tex;
	}

	void ToC3D(const Mat4& m, C3D_Mtx* out)
	{
		for (int r = 0; r < 4; r++)
		{
			out->r[r].x = m.m[r][0];
			out->r[r].y = m.m[r][1];
			out->r[r].z = m.m[r][2];
			out->r[r].w = m.m[r][3];
		}
	}

	void UploadRows(int location, const Mat4& m, int rows)
	{
		C3D_FVec* dst = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, location, rows);
		for (int r = 0; r < rows; r++)
		{
			dst[r].x = m.m[r][0];
			dst[r].y = m.m[r][1];
			dst[r].z = m.m[r][2];
			dst[r].w = m.m[r][3];
		}
	}
}
