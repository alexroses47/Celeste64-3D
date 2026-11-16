#include "world.h"
#include <stdio.h>
#include <string.h>

void logf_(const char* fmt, ...);

namespace World
{
	std::vector<CollisionSolid> solids;
	std::vector<Vec3> verts;
	std::vector<CollisionFace> faces;
	std::vector<Volume> volumes;
	std::vector<int> actorSolid;
	std::vector<Vec3> actorDisplacement;
	std::vector<bool> actorAlive;
	std::vector<u16> dynamicList;
	u32 statRayCasts, statWallChecks, statFacesTested;

	Vec3 VolumeShift(const Volume& v)
	{
		return v.attach >= 0 && v.attach < (int)actorDisplacement.size() ? actorDisplacement[v.attach] : Vec3();
	}

	// ---------------------------------------------------------------- cuadrícula 2D (SolidGrid)
	static constexpr float Cell = 32.0f;
	static float gridMinX, gridMinY;
	static int gridW, gridH;
	static std::vector<std::vector<u16>> grid;
	static std::vector<u32> stamp;
	static u32 stampCounter;

	static void buildGrid()
	{
		Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
		for (auto& s : solids) if (s.actor < 0) { lo = Min(lo, s.min); hi = Max(hi, s.max); }
		gridMinX = lo.x; gridMinY = lo.y;
		gridW = (int)((hi.x - lo.x) / Cell) + 1;
		gridH = (int)((hi.y - lo.y) / Cell) + 1;
		grid.assign(gridW * gridH, {});
		dynamicList.clear();
		for (size_t i = 0; i < solids.size(); i++)
		{
			const auto& s = solids[i];
			if (s.actor >= 0) { dynamicList.push_back((u16)i); continue; }
			int x0 = (int)((s.min.x - gridMinX) / Cell), x1 = (int)((s.max.x - gridMinX) / Cell);
			int y0 = (int)((s.min.y - gridMinY) / Cell), y1 = (int)((s.max.y - gridMinY) / Cell);
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++)
					grid[y * gridW + x].push_back((u16)i);
		}
		stamp.assign(solids.size(), 0);
	}

	// sólidos cuya celda toca el rectángulo [min, max] (sin repetir), más todos los dinámicos
	template <typename F>
	static void query(float minX, float minY, float maxX, float maxY, F&& fn)
	{
		for (u16 i : dynamicList)
		{
			const CollisionSolid& s = solids[i];
			if (s.max.x + s.offset.x < minX || s.min.x + s.offset.x > maxX ||
				s.max.y + s.offset.y < minY || s.min.y + s.offset.y > maxY)
				continue;
			fn(i);
		}
		stampCounter++;
		int x0 = (int)floorf((minX - gridMinX) / Cell), x1 = (int)floorf((maxX - gridMinX) / Cell);
		int y0 = (int)floorf((minY - gridMinY) / Cell), y1 = (int)floorf((maxY - gridMinY) / Cell);
		if (x0 < 0) x0 = 0;
		if (y0 < 0) y0 = 0;
		if (x1 >= gridW) x1 = gridW - 1;
		if (y1 >= gridH) y1 = gridH - 1;
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++)
				for (u16 i : grid[y * gridW + x])
				{
					if (stamp[i] == stampCounter) continue;
					stamp[i] = stampCounter;
					fn(i);
				}
	}

	bool LoadCollision(FILE* f)
	{
		u32 ns, nv, nf;
		if (fread(&ns, 4, 1, f) != 1 || fread(&nv, 4, 1, f) != 1 || fread(&nf, 4, 1, f) != 1)
			return false;
		solids.resize(ns);
		for (auto& s : solids)
		{
			float b[6];
			fread(b, 4, 6, f);
			s.min = Vec3(b[0], b[1], b[2]);
			s.max = Vec3(b[3], b[4], b[5]);
			fread(&s.fstart, 4, 1, f);
			fread(&s.fcount, 4, 1, f);
			fread(&s.actor, 4, 1, f);
		}
		verts.resize(nv);
		fread(verts.data(), sizeof(Vec3), nv, f);
		faces.resize(nf);
		for (auto& fc : faces)
		{
			float p[4];
			fread(p, 4, 4, f);
			fc.normal = Vec3(p[0], p[1], p[2]);
			fc.d = p[3];
			fread(&fc.vstart, 4, 1, f);
			fread(&fc.vcount, 4, 1, f);
		}
		u32 nvol;
		fread(&nvol, 4, 1, f);
		volumes.resize(nvol);
		for (auto& v : volumes)
		{
			float b[9];
			fread(&v.kind, 4, 1, f);
			fread(b, 4, 9, f);
			v.min = Vec3(b[0], b[1], b[2]);
			v.max = Vec3(b[3], b[4], b[5]);
			v.extra = Vec3(b[6], b[7], b[8]);
			fread(&v.attach, 4, 1, f);
		}
		int maxActor = -1;
		for (auto& s : solids) if (s.actor > maxActor) maxActor = s.actor;
		actorSolid.assign(maxActor + 1, -1);
		actorDisplacement.assign(maxActor + 1, Vec3());
		actorAlive.assign(maxActor + 1, true);
		for (size_t i = 0; i < solids.size(); i++)
			if (solids[i].actor >= 0) actorSolid[solids[i].actor] = (int)i;
		buildGrid();
		logf_("colision: %lu solidos, %lu caras, %lu volumenes, cuadricula %dx%d\n", ns, nf, nvol, gridW, gridH);
		return true;
	}

	// ---------------------------------------------------------------- Utils.cs
	static inline float distanceToPlane(Vec3 p, const CollisionFace& f) { return Dot(f.normal, p) + f.d; }

	static bool rayIntersectsTriangle(Vec3 origin, Vec3 direction, Vec3 v0, Vec3 v1, Vec3 v2, float& t)
	{
		t = 0;
		Vec3 edge1 = v1 - v0, edge2 = v2 - v0;
		Vec3 normal = Cross(edge1, edge2);
		float dot = Dot(normal, direction);
		if (fabsf(dot) < 1.401298e-45f)   // float.Epsilon
			return false;
		t = Dot(v0 - origin, normal) / dot;
		if (t < 0)
			return false;
		Vec3 p = origin + direction * t;
		Vec3 toPoint = p - v0;
		float d11 = Dot(edge1, edge1), d12 = Dot(edge1, edge2), d22 = Dot(edge2, edge2);
		float d1p = Dot(edge1, toPoint), d2p = Dot(edge2, toPoint);
		float den = d11 * d22 - d12 * d12;
		float u = (d22 * d1p - d12 * d2p) / den;
		float v = (d11 * d2p - d12 * d1p) / den;
		float w = 1 - u - v;
		return u >= 0 && v >= 0 && w >= 0 && (u + v + w) <= 1;
	}

	// intersección del segmento con el plano horizontal z = planeZ (Utils.PlaneLineIntersection con flatPlane)
	static bool flatLineIntersection(float planeZ, Vec3 a, Vec3 b, Vec3& out)
	{
		Vec3 edge = b - a;
		float t = -(a.z - planeZ) / edge.z;
		if (t >= 0 && t <= 1)
		{
			out = a + edge * t;
			return true;
		}
		return false;
	}

	static bool flatTriangleIntersection(float planeZ, Vec3 v0, Vec3 v1, Vec3 v2, Vec3& l0, Vec3& l1)
	{
		l0 = l1 = Vec3();
		int index = 0;
		Vec3 p;
		if (flatLineIntersection(planeZ, v0, v1, p)) { l0 = p; index++; }
		if (flatLineIntersection(planeZ, v1, v2, p)) { if (index == 0) l0 = p; else l1 = p; index++; }
		if (flatLineIntersection(planeZ, v2, v0, p)) { if (index == 0) l0 = p; else l1 = p; index++; }
		return index > 0;
	}

	// Foster Line.ClosestPoint (segmento 2D)
	static Vec2 closestPointOnSegment(Vec2 a, Vec2 b, Vec2 p)
	{
		Vec2 v = b - a;
		float len2 = v.LengthSquared();
		if (len2 == 0) return a;
		float t = Dot(p - a, v) / len2;
		t = Calc::Clamp(t, 0, 1);
		return a + v * t;
	}

	// ---------------------------------------------------------------- consultas
	bool SolidRayCast(Vec3 worldPoint, Vec3 direction, float distance, RayHit& hit, bool ignoreBackfaces, bool ignoreTransparent)
	{
		const Vec3 point = worldPoint;
		statRayCasts++;
		hit = RayHit();
		bool found = false;
		float closest = 0;

		Vec3 p1 = point + direction * distance;
		Vec3 bmin = Min(point, p1) - Vec3(1, 1, 1), bmax = Max(point, p1) + Vec3(1, 1, 1);

		query(bmin.x, bmin.y, bmax.x, bmax.y, [&](u16 si)
		{
			const CollisionSolid& s = solids[si];
			if (!s.collidable) return;
			if (s.transparent && ignoreTransparent) return;
			Vec3 smin = s.min + s.offset, smax = s.max + s.offset;
			if (smax.x < bmin.x || smin.x > bmax.x || smax.y < bmin.y || smin.y > bmax.y || smax.z < bmin.z || smin.z > bmax.z)
				return;
			const Vec3 point = worldPoint - s.offset;   // espacio local del sólido

			for (u32 fi = s.fstart; fi < s.fstart + s.fcount; fi++)
			{
				const CollisionFace& face = faces[fi];
				statFacesTested++;
				if (ignoreBackfaces && Dot(face.normal, direction) >= 0)
					continue;
				if (distanceToPlane(point, face) > distance)
					continue;
				for (u32 i = 0; i + 2 < face.vcount; i++)
				{
					float dist;
					if (rayIntersectsTriangle(point, direction,
						verts[face.vstart], verts[face.vstart + i + 1], verts[face.vstart + i + 2], dist))
					{
						if (dist > distance)
							continue;
						hit.intersections++;
						if (found && dist > closest)
							continue;
						hit.point = worldPoint + direction * dist;
						hit.normal = face.normal;
						hit.distance = dist;
						hit.solid = si;
						closest = dist;
						found = true;
						break;
					}
				}
			}
		});
		return found;
	}

	int SolidWallCheck(Vec3 point, float radius, WallHit out[8])
	{
		statWallChecks++;
		int count = 0;
		float radiusSquared = radius * radius;
		const Vec3 worldPoint = point;
		query(point.x - radius, point.y - radius, point.x + radius, point.y + radius, [&](u16 si)
		{
			if (count >= 8) return;
			const CollisionSolid& s = solids[si];
			if (!s.collidable) return;
			const Vec3 point = worldPoint - s.offset;
			const Vec2 flatPoint = point.XY();
			// WorldBounds.Inflate(radius).Contains(point)
			if (point.x < s.min.x - radius || point.x > s.max.x + radius ||
				point.y < s.min.y - radius || point.y > s.max.y + radius ||
				point.z < s.min.z - radius || point.z > s.max.z + radius)
				return;

			for (u32 fi = s.fstart; fi < s.fstart + s.fcount && count < 8; fi++)
			{
				const CollisionFace& face = faces[fi];
				statFacesTested++;
				// ignora planos completamente horizontales
				if (face.normal.z <= -1 || face.normal.z >= 1)
					continue;
				float dp = distanceToPlane(point, face);
				if (dp < 0 || dp > radius)
					continue;

				bool have = false;
				WallHit best;
				for (u32 i = 0; i + 2 < face.vcount; i++)
				{
					Vec3 l0, l1;
					if (flatTriangleIntersection(point.z, verts[face.vstart], verts[face.vstart + i + 1], verts[face.vstart + i + 2], l0, l1))
					{
						Vec3 next(closestPointOnSegment(l0.XY(), l1.XY(), flatPoint), point.z);
						Vec3 diff = point - next;
						if (diff.LengthSquared() > radiusSquared)
							continue;
						Vec3 pushout = diff.Normalized() * (radius - diff.Length());
						if (have && pushout.LengthSquared() < best.pushout.LengthSquared())
							continue;
						best.pushout = pushout;
						best.point = next + s.offset;
						best.normal = face.normal;
						best.solid = si;
						have = true;
					}
				}
				if (have)
					out[count++] = best;
			}
		});
		return count;
	}

	bool SolidWallCheckNearest(Vec3 point, float radius, WallHit& hit)
	{
		WallHit hits[8];
		int n = SolidWallCheck(point, radius, hits);
		if (n <= 0) return false;
		hit = hits[0];
		for (int i = 1; i < n; i++)
			if (hits[i].pushout.LengthSquared() > hit.pushout.LengthSquared())   // el que más empuja
				hit = hits[i];
		return true;
	}

	bool SolidWallCheckClosestToNormal(Vec3 point, float radius, Vec3 normal, WallHit& hit)
	{
		WallHit hits[8];
		int n = SolidWallCheck(point, radius, hits);
		if (n <= 0) return false;
		hit = hits[0];
		for (int i = 1; i < n; i++)
			if (Dot(hits[i].normal, normal) > Dot(hit.normal, normal))
				hit = hits[i];
		return true;
	}

	const Volume* OverlapsVolume(VolumeKind kind, Vec3 point, bool (*predicate)(const Volume&, void*), void* user)
	{
		for (const Volume& v : volumes)
		{
			if (v.kind != kind) continue;
			if (v.attach >= 0 && v.attach < (int)actorAlive.size() && !actorAlive[v.attach]) continue;
			if (v.Contains(point, VolumeShift(v)) && (!predicate || predicate(v, user)))
				return &v;
		}
		return nullptr;
	}
}
