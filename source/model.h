// Modelos con huesos (.mdl generado por tools/skinned.py) y su reproducción de animaciones.
// Equivale a SkinnedTemplate + SkinnedModel del original.
#pragma once
#include <3ds.h>
#include <citro3d.h>
#include <vector>
#include "math3d.h"

struct ModelData
{
	struct Material { char name[32]; char tex[32]; C3D_Tex* texture; };
	struct Part { u32 material, istart, icount; };
	struct Joint { s32 parent; float invBind[12]; float rest[10]; char name[16]; };
	struct Anim { char name[32]; u32 frames; float duration; u32 offset; };

	u32 numVerts = 0, numIdx = 0;
	Mat4 root;
	std::vector<Material> materials;
	std::vector<Part> parts;
	std::vector<Joint> joints;
	std::vector<Mat4> invBind;
	std::vector<Anim> anims;
	std::vector<float> animData;
	void* verts = nullptr;    // memoria lineal
	u16* indices = nullptr;   // memoria lineal

	bool Load(const char* name);
	int JointIndex(const char* name) const;
};

constexpr int MaxJoints = 24;

class SkinnedModel
{
public:
	const ModelData* Data;
	float Rate = 1.0f;
	Mat4 Transform = Mat4::Identity();   // Model.Transform del original
	std::vector<u32> MaterialColors;     // RGB por material (Model.Materials[i].Color)
	std::vector<float> MaterialEffects;  // DefaultMaterial.Effects

	explicit SkinnedModel(const ModelData* data);

	int IndexOf(const char* name) const;
	void SetBlendDuration(const char* from, const char* to, float duration, bool twoWay = true);
	void SetLooping(const char* name, bool looping);
	void Play(const char* name, bool restart = false);
	void Play(int index, bool restart = false);
	void Play(const char* a, const char* b, float blend);
	void Clear() { playing.clear(); }
	void Update(float delta);

	// matrices globales de cada hueso (ya con la raíz), válidas después de Update()
	const Mat4& JointMatrix(int i) const { return global[i]; }

	// dibuja con el shader de huesos; world = Transform * BaseTranslation ya aplicado por quien llama
	void Draw(const Mat4& world) const;
	// todo de un color (premultiplicado, 0xAABBGGRR) sobre textura blanca: estelas y fresas fantasma
	void DrawTinted(const Mat4& world, u32 abgr, C3D_Tex* white) const;
	// copia la pose actual de otro modelo del mismo tipo (Trail.Model.SetBlendedWeights)
	void CopyPose(const SkinnedModel& other);

private:
	struct Playing { int index; float time, duration, blend, blendDuration; bool loops; };
	std::vector<Playing> playing;
	std::vector<std::pair<std::pair<int, int>, float>> blendDurations;
	std::vector<std::pair<int, bool>> loops;
	Mat4 global[MaxJoints];
	Mat4 skin[MaxJoints];

	Playing Request(int index);
	float BlendDurationFor(int a, int b) const;
	bool LoopsFor(int index) const;
	void SamplePose(float out[][10]) const;
};
