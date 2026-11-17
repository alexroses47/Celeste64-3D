// Controles del original (Controls.cs + ControlsConfig.cs) sobre los botones de la 3DS.
//   Jump  = B, X   (South, North)
//   Dash  = Y, A   (West, East)
//   Move  = Circle Pad (zona muerta 0.35)
//   Old 3DS: Climb = L, R (hombros)   Camera = D-pad
//   New 3DS: Climb = ZL, ZR (gatillos) Camera = C-Stick, L/R giran, D-pad también
//            (el original usa el stick derecho para la cámara y gatillos/hombros para escalar)
//   Confirm = A, Cancel = B (filtro Nintendo del original)
#pragma once
#include <3ds.h>
#include "math3d.h"

struct VirtualAction
{
	float buffer = 0;      // segundos que dura un "Pressed" sin consumir
	bool down = false;
	float tPressed = 0;

	bool Down() const { return down; }
	bool Pressed() const { return tPressed > 0; }
	bool ConsumePress() { bool p = tPressed > 0; tPressed = 0; return p; }
};

namespace Controls
{
	extern Vec2 Move;       // convención Foster: Y positivo = abajo
	extern Vec2 Camera;
	extern VirtualAction Jump, Dash, Climb, Confirm, Cancel;

	// una vez por cuadro dibujado
	void Sample();
	bool NewLayout();     // New 3DS: C-Stick, ZL/ZR
	u32 ClimbMask();
	// al final de cada paso lógico de 1/60 s
	void Step(float delta);
}
