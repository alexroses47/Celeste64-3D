// Estado de GPU compartido: shaders, TexEnv, texturas y matrices de cámara.
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "math3d.h"

namespace Gfx
{
	// uniformes del shader de escenario
	struct SceneShader { shaderProgram_s program; int proj, modelView, sun, params, offset; };
	// desplazamiento al mundo de la malla que se dibuja (niebla vertical)
	void SetSceneOffset(Vec3 o);
	// uniformes del shader con huesos
	struct SkinnedShader { shaderProgram_s program; int proj, view, model, sun, params, bones; };

	// shader de la geometría de los niveles (luz precalculada en el vértice)
	struct LevelShader { shaderProgram_s program; int mvp, fog, lightScale; };
	extern LevelShader level;
	void BindLevel();
	// modelView del trozo + altura del objeto para la niebla vertical
	void SetLevelTransform(const C3D_Mtx& modelView, float offsetZ);
	// dirección del sol en los niveles (para precalcular la luz)
	Vec3 WorldSun();

	extern SceneShader scene;
	extern SkinnedShader skinned;
	extern C3D_Mtx projection;   // del ojo actual
	extern C3D_Mtx view;

	void Init();

	// colores en formato 0xAABBGGRR (TexEnv)
	constexpr u32 White = 0xFFFFFFFF;
	u32 ColorABGR(u32 rgb, float alpha = 1.0f);

	// textura -> color * material, luego sombra, brillo y niebla vertical (Default.hlsl)
	void SetLitTexEnv(u32 materialColor = White);
	// solo la textura (skybox)
	void SetUnlitTexEnv();

	// sol y efectos (Default.hlsl: u_effects multiplica sombra y brillo)
	void SetSceneParams(float effects);
	void SetSkinnedParams(float effects);
	// RenderState.SunDirection / VerticalFogColor (el título usa otros valores)
	void SetSun(Vec3 dir);
	void SetFogColor(u32 rgb);

	void BindScene();
	void BindSkinned();
	// otro módulo cambió el programa de shader: la próxima Bind* debe reconfigurarse
	void MarkUnbound();

	// carga romfs:/tex/<clave>.t3x una sola vez
	C3D_Tex* GetTexture(const char* key, bool repeat = true);

	void ToC3D(const Mat4& m, C3D_Mtx* out);
	void UploadRows(int location, const Mat4& m, int rows);
}
