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

	void Sample()
	{
		u32 held = hidKeysHeld(), down = hidKeysDown();
		sampleAction(Jump, held, down, KEY_B | KEY_X);
		sampleAction(Dash, held, down, KEY_Y | KEY_A);
		sampleAction(Climb, held, down, KEY_L | KEY_R);
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
	}

	void Step(float delta)
	{
		VirtualAction* all[] = { &Jump, &Dash, &Climb, &Confirm, &Cancel };
		for (auto* a : all)
			if (a->tPressed > 0)
				a->tPressed -= delta;
	}
}
