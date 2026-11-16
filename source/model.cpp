#include "model.h"
#include "gfx.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

void logf_(const char* fmt, ...);

static Mat4 rows34(const float* f)
{
	Mat4 m = Mat4::Identity();
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			m.m[r][c] = f[r * 4 + c];
	return m;
}

bool ModelData::Load(const char* name)
{
	char path[96];
	snprintf(path, sizeof(path), "romfs:/models/%s.mdl", name);
	FILE* f = fopen(path, "rb");
	if (!f) { logf_("no existe %s\n", path); return false; }

	struct { char magic[4]; u32 version, nVerts, nIdx, nParts, nJoints, nAnims, nMats; } h;
	fread(&h, sizeof(h), 1, f);
	if (memcmp(h.magic, "C64M", 4) != 0 || h.version != 1 || h.nJoints > MaxJoints)
	{
		logf_("%s: formato inválido\n", path);
		fclose(f);
		return false;
	}
	numVerts = h.nVerts;
	numIdx = h.nIdx;

	float r[12];
	fread(r, sizeof(r), 1, f);
	root = rows34(r);

	materials.resize(h.nMats);
	for (auto& m : materials)
	{
		fread(m.name, 32, 1, f);
		fread(m.tex, 32, 1, f);
		m.texture = m.tex[0] ? Gfx::GetTexture(m.tex) : nullptr;
	}
	parts.resize(h.nParts);
	fread(parts.data(), sizeof(Part), h.nParts, f);

	joints.resize(h.nJoints);
	invBind.resize(h.nJoints);
	for (u32 i = 0; i < h.nJoints; i++)
	{
		Joint& j = joints[i];
		fread(&j.parent, 4, 1, f);
		fread(j.invBind, 4, 12, f);
		fread(j.rest, 4, 10, f);
		fread(j.name, 16, 1, f);
		invBind[i] = rows34(j.invBind);
	}

	anims.resize(h.nAnims);
	u32 totalFloats = 0;
	for (auto& a : anims)
	{
		fread(a.name, 32, 1, f);
		fread(&a.frames, 4, 1, f);
		fread(&a.duration, 4, 1, f);
		fread(&a.offset, 4, 1, f);
		u32 end = a.offset + a.frames * h.nJoints * 10;
		if (end > totalFloats) totalFloats = end;
	}
	animData.resize(totalFloats);
	fread(animData.data(), 4, totalFloats, f);

	const u32 vsize = 32;
	verts = linearAlloc(vsize * numVerts);
	fread(verts, vsize, numVerts, f);
	indices = (u16*)linearAlloc(sizeof(u16) * numIdx);
	fread(indices, sizeof(u16), numIdx, f);
	fclose(f);

	logf_("modelo %s: %lu vertices, %lu huesos, %lu animaciones\n", name, numVerts, (u32)joints.size(), (u32)anims.size());
	return true;
}

int ModelData::JointIndex(const char* name) const
{
	for (size_t i = 0; i < joints.size(); i++)
		if (strcmp(joints[i].name, name) == 0)
			return (int)i;
	return -1;
}

// ---------------------------------------------------------------- SkinnedModel

SkinnedModel::SkinnedModel(const ModelData* data) : Data(data)
{
	MaterialColors.assign(data->materials.size(), 0xFFFFFF);
	MaterialEffects.assign(data->materials.size(), 1.0f);
	for (int i = 0; i < MaxJoints; i++)
		global[i] = skin[i] = Mat4::Identity();
}

int SkinnedModel::IndexOf(const char* name) const
{
	for (size_t i = 0; i < Data->anims.size(); i++)
		if (strcasecmp(Data->anims[i].name, name) == 0)
			return (int)i;
	return -1;
}

void SkinnedModel::SetBlendDuration(const char* from, const char* to, float duration, bool twoWay)
{
	int a = IndexOf(from), b = IndexOf(to);
	if (a < 0 || b < 0) return;
	blendDurations.push_back({ { a, b }, duration });
	if (twoWay)
		blendDurations.push_back({ { b, a }, duration });
}

void SkinnedModel::SetLooping(const char* name, bool looping)
{
	int i = IndexOf(name);
	if (i >= 0) loops.push_back({ i, looping });
}

float SkinnedModel::BlendDurationFor(int a, int b) const
{
	for (auto it = blendDurations.rbegin(); it != blendDurations.rend(); ++it)
		if (it->first.first == a && it->first.second == b)
			return it->second;
	return 0.01f;
}

bool SkinnedModel::LoopsFor(int index) const
{
	for (auto it = loops.rbegin(); it != loops.rend(); ++it)
		if (it->first == index)
			return it->second;
	return true;
}

SkinnedModel::Playing SkinnedModel::Request(int index)
{
	Playing it = { index, 0, 0, 0, 0, true };
	for (size_t i = 0; i < playing.size(); i++)
		if (playing[i].index == index)
		{
			it = playing[i];
			playing.erase(playing.begin() + i);
			break;
		}
	it.duration = Data->anims[index].duration;
	it.loops = LoopsFor(index);
	return it;
}

void SkinnedModel::Play(const char* name, bool restart)
{
	int i = IndexOf(name);
	if (i >= 0) Play(i, restart);
}

void SkinnedModel::Play(int index, bool restart)
{
	Playing it = Request(index);
	if (playing.empty())
		it.blend = 1;
	if (restart || it.blend <= 0)
		it.time = 0;
	int last = playing.empty() ? -1 : playing.back().index;
	it.blendDuration = BlendDurationFor(index, last);
	playing.push_back(it);
}

void SkinnedModel::Play(const char* nameA, const char* nameB, float blendAmount)
{
	int a = IndexOf(nameA), b = IndexOf(nameB);
	if (a < 0 || b < 0) return;
	Playing pa = Request(a), pb = Request(b);
	pa.blendDuration = pb.blendDuration = 0;
	pa.blend = 1.0f - blendAmount;
	pb.blend = blendAmount;
	playing.clear();
	playing.push_back(pa);
	playing.push_back(pb);
}

void SkinnedModel::Update(float delta)
{
	float blendDuration = playing.empty() ? 0 : playing.back().blendDuration;
	for (size_t i = 0; i < playing.size(); i++)
	{
		Playing& it = playing[i];
		it.time += Rate * delta;
		if (it.time >= it.duration)
			it.time = it.loops ? it.time - it.duration : it.duration;
		if (blendDuration > 0)
			it.blend = Calc::Approach(it.blend, i == playing.size() - 1 ? 1.0f : 0.0f, delta / blendDuration);
	}

	// pose mezclada (local) de cada hueso
	const int nj = (int)Data->joints.size();
	float pose[MaxJoints][10];
	SamplePose(pose);

	for (int j = 0; j < nj; j++)
	{
		const float* p = pose[j];
		Quat q = { p[3], p[4], p[5], p[6] };
		Mat4 local = Mat4::FromTRS(Vec3(p[0], p[1], p[2]), q, Vec3(p[7], p[8], p[9]));
		int parent = Data->joints[j].parent;
		global[j] = (parent >= 0 ? global[parent] : Data->root) * local;
		skin[j] = global[j] * Data->invBind[j];
	}
}

// mezcla de las animaciones activas: posición/escala lineales, rotación nlerp
void SkinnedModel::SamplePose(float out[][10]) const
{
	const int nj = (int)Data->joints.size();
	float total = 0;
	for (int j = 0; j < nj; j++)
		for (int k = 0; k < 10; k++)
			out[j][k] = 0;

	for (const Playing& it : playing)
	{
		if (it.blend <= 0) continue;
		const ModelData::Anim& a = Data->anims[it.index];
		float t = it.duration > 0 ? fmodf(it.time, it.duration) : 0;
		if (it.time >= it.duration && !it.loops) t = it.duration;
		float fpos = t * 30.0f;
		u32 f0 = (u32)fpos;
		if (f0 >= a.frames - 1) f0 = a.frames - 1;
		u32 f1 = f0 + 1 < a.frames ? f0 + 1 : f0;
		float fr = fpos - f0;
		if (fr > 1) fr = 1;
		const float* d0 = &Data->animData[a.offset + f0 * nj * 10];
		const float* d1 = &Data->animData[a.offset + f1 * nj * 10];

		for (int j = 0; j < nj; j++)
		{
			const float* p0 = d0 + j * 10;
			const float* p1 = d1 + j * 10;
			float* o = out[j];
			// rotación: alinear el signo con lo acumulado
			float dq = p0[3] * p1[3] + p0[4] * p1[4] + p0[5] * p1[5] + p0[6] * p1[6];
			float s1 = dq < 0 ? -1.0f : 1.0f;
			float q[4];
			for (int k = 0; k < 4; k++) q[k] = p0[3 + k] + (p1[3 + k] * s1 - p0[3 + k]) * fr;
			float acc = o[3] * q[0] + o[4] * q[1] + o[5] * q[2] + o[6] * q[3];
			float sa = acc < 0 ? -1.0f : 1.0f;
			for (int k = 0; k < 3; k++) o[k] += (p0[k] + (p1[k] - p0[k]) * fr) * it.blend;
			for (int k = 0; k < 4; k++) o[3 + k] += q[k] * sa * it.blend;
			for (int k = 7; k < 10; k++) o[k] += (p0[k] + (p1[k] - p0[k]) * fr) * it.blend;
		}
		total += it.blend;
	}

	for (int j = 0; j < nj; j++)
	{
		float* o = out[j];
		if (total <= 0)
		{
			memcpy(o, Data->joints[j].rest, sizeof(float) * 10);
			continue;
		}
		for (int k = 0; k < 3; k++) o[k] /= total;
		for (int k = 7; k < 10; k++) o[k] /= total;
		float l = sqrtf(o[3] * o[3] + o[4] * o[4] + o[5] * o[5] + o[6] * o[6]);
		if (l > 0) for (int k = 3; k < 7; k++) o[k] /= l;
		else { o[3] = o[4] = o[5] = 0; o[6] = 1; }
	}
}

void SkinnedModel::CopyPose(const SkinnedModel& other)
{
	for (int i = 0; i < MaxJoints; i++)
	{
		global[i] = other.global[i];
		skin[i] = other.skin[i];
	}
}

void SkinnedModel::DrawTinted(const Mat4& world, u32 abgr, C3D_Tex* white) const
{
	Gfx::BindSkinned();
	Gfx::UploadRows(Gfx::skinned.model, world, 3);
	const int nj = (int)Data->joints.size();
	C3D_FVec* bones = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, Gfx::skinned.bones, nj * 3);
	for (int j = 0; j < nj; j++)
		for (int r = 0; r < 3; r++)
		{
			C3D_FVec& v = bones[j * 3 + r];
			v.x = skin[j].m[r][0]; v.y = skin[j].m[r][1]; v.z = skin[j].m[r][2]; v.w = skin[j].m[r][3];
		}
	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, Data->verts, 32, 5, 0x43210);
	Gfx::SetSkinnedParams(0);
	Gfx::SetLitTexEnv(abgr);
	C3D_TexBind(0, white);
	for (const auto& part : Data->parts)
		C3D_DrawElements(GPU_TRIANGLES, part.icount, C3D_UNSIGNED_SHORT, Data->indices + part.istart);
}

void SkinnedModel::Draw(const Mat4& world) const
{
	Gfx::BindSkinned();
	Gfx::UploadRows(Gfx::skinned.model, world, 3);

	const int nj = (int)Data->joints.size();
	C3D_FVec* bones = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, Gfx::skinned.bones, nj * 3);
	for (int j = 0; j < nj; j++)
		for (int r = 0; r < 3; r++)
		{
			C3D_FVec& v = bones[j * 3 + r];
			v.x = skin[j].m[r][0];
			v.y = skin[j].m[r][1];
			v.z = skin[j].m[r][2];
			v.w = skin[j].m[r][3];
		}

	C3D_BufInfo* buf = C3D_GetBufInfo();
	BufInfo_Init(buf);
	BufInfo_Add(buf, Data->verts, 32, 5, 0x43210);

	for (const auto& part : Data->parts)
	{
		const auto& mat = Data->materials[part.material];
		if (!mat.texture) continue;
		Gfx::SetSkinnedParams(MaterialEffects[part.material]);
		Gfx::SetLitTexEnv(Gfx::ColorABGR(MaterialColors[part.material]));
		C3D_TexBind(0, mat.texture);
		C3D_DrawElements(GPU_TRIANGLES, part.icount, C3D_UNSIGNED_SHORT, Data->indices + part.istart);
	}
}
