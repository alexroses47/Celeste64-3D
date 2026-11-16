// Matemáticas básicas para el port. Convención de vector columna (M * v), matrices por filas.
// Las cadenas de System.Numerics del original (fila: v * A * B) se escriben aquí como B * A.
#pragma once
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
constexpr float PI = 3.14159265358979f;
constexpr float TAU = 6.28318530717959f;
constexpr float DEG2RAD = PI / 180.0f;

struct Vec2
{
	float x = 0, y = 0;
	constexpr Vec2() = default;
	constexpr Vec2(float x, float y) : x(x), y(y) {}
	Vec2 operator+(Vec2 o) const { return { x + o.x, y + o.y }; }
	Vec2 operator-(Vec2 o) const { return { x - o.x, y - o.y }; }
	Vec2 operator*(float s) const { return { x * s, y * s }; }
	Vec2 operator/(float s) const { return { x / s, y / s }; }
	Vec2 operator-() const { return { -x, -y }; }
	Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
	Vec2& operator-=(Vec2 o) { x -= o.x; y -= o.y; return *this; }
	Vec2& operator*=(float s) { x *= s; y *= s; return *this; }
	bool operator==(Vec2 o) const { return x == o.x && y == o.y; }
	bool operator!=(Vec2 o) const { return !(*this == o); }
	float LengthSquared() const { return x * x + y * y; }
	float Length() const { return sqrtf(LengthSquared()); }
	// Foster: Normalized() de un vector cero devuelve cero
	Vec2 Normalized() const { float l = Length(); return l > 0 ? *this / l : Vec2(); }
	// Foster Vec2.Angle(): atan2(y, x)
	float Angle() const { return atan2f(y, x); }
	static constexpr Vec2 Zero() { return { 0, 0 }; }
	static constexpr Vec2 UnitX() { return { 1, 0 }; }
	static constexpr Vec2 UnitY() { return { 0, 1 }; }
};
inline Vec2 operator*(float s, Vec2 v) { return v * s; }
inline float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }

struct Vec3
{
	float x = 0, y = 0, z = 0;
	constexpr Vec3() = default;
	constexpr Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
	constexpr Vec3(Vec2 v, float z) : x(v.x), y(v.y), z(z) {}
	Vec3 operator+(Vec3 o) const { return { x + o.x, y + o.y, z + o.z }; }
	Vec3 operator-(Vec3 o) const { return { x - o.x, y - o.y, z - o.z }; }
	Vec3 operator*(float s) const { return { x * s, y * s, z * s }; }
	Vec3 operator*(Vec3 o) const { return { x * o.x, y * o.y, z * o.z }; }
	Vec3 operator/(float s) const { return { x / s, y / s, z / s }; }
	Vec3 operator-() const { return { -x, -y, -z }; }
	Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
	Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
	Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
	bool operator==(Vec3 o) const { return x == o.x && y == o.y && z == o.z; }
	bool operator!=(Vec3 o) const { return !(*this == o); }
	float LengthSquared() const { return x * x + y * y + z * z; }
	float Length() const { return sqrtf(LengthSquared()); }
	Vec3 Normalized() const { float l = Length(); return l > 0 ? *this / l : Vec3(); }
	Vec2 XY() const { return { x, y }; }
	static constexpr Vec3 Zero() { return { 0, 0, 0 }; }
	static constexpr Vec3 One() { return { 1, 1, 1 }; }
	static constexpr Vec3 UnitX() { return { 1, 0, 0 }; }
	static constexpr Vec3 UnitY() { return { 0, 1, 0 }; }
	static constexpr Vec3 UnitZ() { return { 0, 0, 1 }; }
};
inline Vec3 operator*(float s, Vec3 v) { return v * s; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline Vec3 Min(Vec3 a, Vec3 b) { return { fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z) }; }
inline Vec3 Max(Vec3 a, Vec3 b) { return { fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z) }; }
inline Vec3 Lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

struct Quat
{
	float x = 0, y = 0, z = 0, w = 1;
};

struct Mat4
{
	float m[4][4];

	static Mat4 Identity()
	{
		Mat4 r{};
		r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1;
		return r;
	}
	static Mat4 Translate(Vec3 t) { Mat4 r = Identity(); r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z; return r; }
	static Mat4 Scale(Vec3 s) { Mat4 r = Identity(); r.m[0][0] = s.x; r.m[1][1] = s.y; r.m[2][2] = s.z; return r; }
	static Mat4 Scale(float s) { return Scale(Vec3(s, s, s)); }
	// mismas direcciones que Matrix.CreateRotationX/Y/Z
	static Mat4 RotateX(float a) { Mat4 r = Identity(); float c = cosf(a), s = sinf(a); r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c; return r; }
	static Mat4 RotateY(float a) { Mat4 r = Identity(); float c = cosf(a), s = sinf(a); r.m[0][0] = c; r.m[0][2] = s; r.m[2][0] = -s; r.m[2][2] = c; return r; }
	static Mat4 RotateZ(float a) { Mat4 r = Identity(); float c = cosf(a), s = sinf(a); r.m[0][0] = c; r.m[0][1] = -s; r.m[1][0] = s; r.m[1][1] = c; return r; }
	static Mat4 FromTRS(Vec3 t, Quat q, Vec3 s)
	{
		float x = q.x, y = q.y, z = q.z, w = q.w;
		Mat4 r = Identity();
		r.m[0][0] = (1 - 2 * (y * y + z * z)) * s.x; r.m[0][1] = 2 * (x * y - z * w) * s.y; r.m[0][2] = 2 * (x * z + y * w) * s.z;
		r.m[1][0] = 2 * (x * y + z * w) * s.x; r.m[1][1] = (1 - 2 * (x * x + z * z)) * s.y; r.m[1][2] = 2 * (y * z - x * w) * s.z;
		r.m[2][0] = 2 * (x * z - y * w) * s.x; r.m[2][1] = 2 * (y * z + x * w) * s.y; r.m[2][2] = (1 - 2 * (x * x + y * y)) * s.z;
		r.m[0][3] = t.x; r.m[1][3] = t.y; r.m[2][3] = t.z;
		return r;
	}

	Mat4 operator*(const Mat4& o) const
	{
		Mat4 r;
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++)
				r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j] + m[i][3] * o.m[3][j];
		return r;
	}
	Vec3 TransformPoint(Vec3 v) const
	{
		return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3],
		         m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3],
		         m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] };
	}
	Vec3 TransformNormal(Vec3 v) const
	{
		return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z,
		         m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
		         m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z };
	}
	Vec3 Translation() const { return { m[0][3], m[1][3], m[2][3] }; }
};

// ---------------------------------------------------------------- Foster.Calc
namespace Calc
{
	inline float Approach(float from, float target, float amount)
	{
		return from > target ? fmaxf(from - amount, target) : fminf(from + amount, target);
	}
	inline void ApproachRef(float& v, float target, float amount) { v = Approach(v, target, amount); }
	inline Vec2 Approach(Vec2 from, Vec2 target, float amount)
	{
		if (from == target) return target;
		Vec2 diff = target - from;
		if (diff.LengthSquared() <= amount * amount) return target;
		return from + diff.Normalized() * amount;
	}
	inline Vec3 Approach(Vec3 from, Vec3 target, float amount)
	{
		if (from == target) return target;
		Vec3 diff = target - from;
		if (diff.LengthSquared() <= amount * amount) return target;
		return from + diff.Normalized() * amount;
	}
	inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
	inline float Map(float v, float min, float max, float newMin, float newMax)
	{
		return (v - min) / (max - min) * (newMax - newMin) + newMin;
	}
	inline float ClampedMap(float v, float min, float max, float newMin = 0, float newMax = 1)
	{
		return Clamp((v - min) / (max - min), 0, 1) * (newMax - newMin) + newMin;
	}
	inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
	inline float Sign(float v) { return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f); }
	// MathF.Round redondea al par más cercano: rintf con el modo por defecto hace lo mismo
	inline float Snap(float v, float interval) { return rintf(v / interval) * interval; }
	// Foster: ((b - a - PI) % TAU + TAU) % TAU - PI  (el % de C# es fmod)
	inline float AngleDiff(float a, float b)
	{
		return fmodf(fmodf(b - a - PI, TAU) + TAU, TAU) - PI;
	}
	inline float AngleLerp(float start, float end, float percent) { return start + AngleDiff(start, end) * percent; }
	inline float Angle(Vec2 v) { return atan2f(v.y, v.x); }
	inline float AngleApproach(float val, float target, float maxMove)
	{
		float diff = AngleDiff(val, target);
		if (fabsf(diff) < maxMove) return target;
		return val + Clamp(diff, -maxMove, maxMove);
	}
	inline Vec2 AngleToVector(float angle, float length = 1) { return { cosf(angle) * length, sinf(angle) * length }; }
	inline Vec2 RotateToward(Vec2 dir, Vec2 target, float maxAngleDelta, float maxMagnitudeDelta)
	{
		float angle = dir.Angle();
		float len = dir.Length();
		if (maxAngleDelta > 0) angle = AngleApproach(angle, target.Angle(), maxAngleDelta);
		if (maxMagnitudeDelta > 0) len = Approach(len, target.Length(), maxMagnitudeDelta);
		return AngleToVector(angle, len);
	}
	// Time.OnInterval: floor((t - delta) / i) < floor(t / i)
	inline bool OnInterval(double time, double delta, double interval)
	{
		return floor((time - delta) / interval) < floor(time / interval);
	}
	// Utils.Lerp3 del original
	inline float Lerp3(float a, float b, float c, float d, float t)
	{
		if (t < 1.0f / 3) return Lerp(a, b, t * 3);
		if (t < 2.0f / 3) return Lerp(b, c, (t - 1.0f / 3) * 3);
		return Lerp(c, d, (t - 2.0f / 3) * 3);
	}
	inline float YoYo(float v) { return v <= 0.5f ? v * 2 : 1 - (v - 0.5f) * 2; }
}

// ---------------------------------------------------------------- Foster.Ease
namespace Ease
{
	inline float CubeIn(float t) { return t * t * t; }
	inline float CubeOut(float t) { return 1 - CubeIn(1 - t); }
	inline float BackIn(float t) { return t * t * (2.70158f * t - 1.70158f); }
	inline float UpDown(float eased) { return eased <= 0.5f ? eased * 2 : 1 - (eased - .5f) * 2; }
}
