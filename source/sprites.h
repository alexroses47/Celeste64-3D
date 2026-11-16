// Sprites y dibujo 2D (SpriteRenderer.cs + Batcher para la interfaz).
// Los colores son premultiplicados, como Foster: Color * 0.5 escala también el alfa.
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include "math3d.h"

struct Color4
{
	float r = 1, g = 1, b = 1, a = 1;
	constexpr Color4() = default;
	constexpr Color4(float r, float g, float b, float a) : r(r), g(g), b(b), a(a) {}
	static Color4 FromRGB(u32 rgb, float alpha = 1.0f)
	{
		return { ((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha };
	}
	Color4 operator*(float s) const { return { r * s, g * s, b * s, a * s }; }
};

namespace Sprites
{
	void Init();
	// al empezar cada cuadro (alterna el búfer de vértices)
	void BeginFrame();

	// ---- 3D: se acumulan y se dibujan con Flush3D en cada ojo
	void Clear3D();
	// post = se dibuja encima de todo (Sprite.Post del original)
	void Quad(C3D_Tex* tex, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Color4 color, bool post = false);
	void Billboard(C3D_Tex* tex, Vec3 at, float size, Color4 color, Vec3 camLeft, Vec3 camUp, bool post = false);
	// cuadrilátero 3D con UV propias: uvA = esquina a (arriba-izq), uvC = esquina c (abajo-der)
	void QuadUV(C3D_Tex* tex, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec2 uvA, Vec2 uvC, Color4 color);
	// dibuja los sprites 3D acumulados (profundidad sin escritura, mezcla premultiplicada)
	void Flush3D();

	// ---- 2D en píxeles de la pantalla de arriba (400x240): se dibuja de inmediato
	void Begin2D();
	// 2D sobre una textura de destino (sin la rotación de la pantalla de la 3DS)
	void Begin2DTarget(float width, float height);
	// mezcla aditiva para lo que se dibuje después (hasta End2D o SetAdditive(false))
	void SetAdditive(bool on);
	void Image2DUV(C3D_Tex* tex, Vec2 a, Vec2 b, Vec2 c, Vec2 d, Vec2 uvA, Vec2 uvC, Color4 color);
	void Rect2D(float x, float y, float w, float h, Color4 color);
	void Triangle2D(Vec2 a, Vec2 b, Vec2 c, Color4 color);
	void Image2D(C3D_Tex* tex, float x, float y, float w, float h, Color4 color);
	// texto con la fuente del juego (Renogare a escala 240p); admite '\n'. maxChars < 0 = todo
	float Text2D(const char* text, float x, float y, Color4 color, bool outline = true, int maxChars = -1, float scale = 1);
	float TextWidth(const char* text);
	Vec2 TextSize(const char* text);
	float LineHeight();
	void RectRounded2D(float x, float y, float w, float h, float r, Color4 color);
	void End2D();
	// dibuja ya lo 2D pendiente (antes de cambiar estados de la GPU a mano)
	void Flush2D();

	// textura blanca 8x8 para formas lisas
	C3D_Tex* White();
}
