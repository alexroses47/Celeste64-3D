#include "input.h"

namespace Controls
{
	Vec2 Move, Camera;
	VirtualAction Jump{ .1f }, Dash{ .1f }, Climb{ 0 }, Confirm{ 0 }, Cancel{ 0 };

	static void sampleAction(VirtualAction& a, u32 held, u32 down, u32 mask)
	{
		a.down = (held & mask) != 0;
		if (down & mask)
			a.tPressed = a.buffer > 0 ? a.buffer : 1.0f / 60.0f;
	}

	static int newLayout = -1;   // -1 sin revisar, 0 Old 3DS, 1 New 3DS

	bool NewLayout()
	{
		if (newLayout < 0)
		{
			bool isNew = false;
			APT_CheckNew3DS(&isNew);
			newLayout = isNew ? 1 : 0;
		}
		return newLayout == 1;
	}

	u32 ClimbMask() { return NewLayout() ? (KEY_ZL | KEY_ZR) : (KEY_L | KEY_R); }

	void Sample()
	{
		u32 held = hidKeysHeld(), down = hidKeysDown();
		sampleAction(Jump, held, down, KEY_B | KEY_X);
		sampleAction(Dash, held, down, KEY_Y | KEY_A);
		sampleAction(Climb, held, down, ClimbMask());
		sampleAction(Confirm, held, down, KEY_A);
		sampleAction(Cancel, held, down, KEY_B);

		circlePosition cp;
		hidCircleRead(&cp);
		Vec2 v(cp.dx / 150.0f, -cp.dy / 150.0f);
		float len = v.Length();
		if (len > 1) v = v / len;
		Move = len < 0.35f ? Vec2() : v;

		Camera = Vec2();
		if (held & KEY_DLEFT) Camera.x -= 1;
		if (held & KEY_DRIGHT) Camera.x += 1;
		if (held & KEY_DUP) Camera.y -= 1;
		if (held & KEY_DDOWN) Camera.y += 1;
		if (NewLayout())
		{
			// C-Stick como el stick derecho del original (zona muerta 0.35) y L/R para girar
			circlePosition cs;
			hidCstickRead(&cs);
			Vec2 c(cs.dx / 140.0f, -cs.dy / 140.0f);
			float cl = c.Length();
			if (cl > 1) c = c / cl;
			if (cl >= 0.35f) Camera += c;
			if (held & KEY_L) Camera.x -= 1;
			if (held & KEY_R) Camera.x += 1;
			Camera.x = Calc::Clamp(Camera.x, -1, 1);
			Camera.y = Calc::Clamp(Camera.y, -1, 1);
		}
	}

	void Step(float delta)
	{
		VirtualAction* all[] = { &Jump, &Dash, &Climb, &Confirm, &Cancel };
		for (auto* a : all)
			if (a->tPressed > 0)
				a->tPressed -= delta;
	}
}
