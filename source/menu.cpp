#include "menu.h"
#include "audio.h"
#include "input.h"
#include "sprites.h"

// Menu.cs a escala 240/360
static constexpr float K = 240.0f / 360.0f;
static constexpr float Spacing = 4 * K;
static constexpr float SpacerHeight = 12 * K;
static constexpr float TitleScale = 0.75f;   // (el texto no se escala; el título va en gris)

std::string Menu::Item::Label() const
{
	switch (kind)
	{
	case Toggle: return label + " : " + (getBool && getBool() ? " ON" : "OFF");
	case MultiSelect: return label + " : " + (options.empty() ? "" : options[getInt()]);
	case Slider:
	{
		int v = getInt();
		return label + " [" + std::string(v - min, '|') + std::string(max - v, '.') + "]";
	}
	default: return label;
	}
}

Menu& Menu::AddOption(const std::string& label, std::function<void()> action)
{
	Item it; it.kind = Item::Option; it.label = label; it.action = action;
	items.push_back(it);
	return *this;
}

Menu& Menu::AddSubmenu(const std::string& label, Menu* submenu)
{
	Item it; it.kind = Item::Submenu; it.label = label; it.submenu = submenu;
	items.push_back(it);
	return *this;
}

Menu& Menu::AddToggle(const std::string& label, std::function<void()> toggle, std::function<bool()> get)
{
	Item it; it.kind = Item::Toggle; it.label = label; it.action = toggle; it.getBool = get;
	items.push_back(it);
	return *this;
}

Menu& Menu::AddMultiSelect(const std::string& label, std::vector<std::string> options, std::function<int()> get, std::function<void(int)> set)
{
	Item it; it.kind = Item::MultiSelect; it.label = label; it.options = options; it.getInt = get; it.setInt = set;
	items.push_back(it);
	return *this;
}

Menu& Menu::AddSlider(const std::string& label, int min, int max, std::function<int()> get, std::function<void(int)> set)
{
	Item it; it.kind = Item::Slider; it.label = label; it.min = min; it.max = max; it.getInt = get; it.setInt = set;
	items.push_back(it);
	return *this;
}

Menu& Menu::AddSpacer()
{
	Item it; it.kind = Item::Spacer;
	items.push_back(it);
	return *this;
}

void Menu::HandleInput(bool up, bool down, bool left, bool right)
{
	if (items.empty()) return;
	int was = Index, step = 0;
	if (down) step = 1;
	if (up) step = -1;
	int n = (int)items.size();
	Index += step;
	while (!items[((Index % n) + n) % n].Selectable())
		Index += step;
	Index = ((Index % n) + n) % n;
	if (was != Index)
		Audio::Play(step < 0 ? "main_menu_roll_up" : "main_menu_roll_down");

	Item& it = items[Index];
	int slide = left ? -1 : (right ? 1 : 0);
	if (slide)
	{
		if (it.kind == Item::MultiSelect)
		{
			int count = (int)it.options.size();
			it.setInt((it.getInt() + slide + count) % count);
			Audio::Play("ui_move");
		}
		else if (it.kind == Item::Slider)
		{
			it.setInt(Calc::Clamp(it.getInt() + slide, it.min, it.max));
			Audio::Play("ui_move");
		}
	}

	if (Controls::Confirm.Pressed())
	{
		bool consumed = false;
		switch (it.kind)
		{
		case Item::Option:
			if (it.action) { Audio::Play("ui_select"); it.action(); consumed = true; }
			break;
		case Item::Submenu:
			Audio::Play("main_menu_toggle_on");
			it.submenu->Index = 0;
			submenus.push_back(it.submenu);
			consumed = true;
			break;
		case Item::Toggle:
			Audio::Play(it.getBool() ? "main_menu_toggle_off" : "main_menu_toggle_on");
			it.action();
			consumed = true;
			break;
		default: break;
		}
		if (consumed) Controls::Confirm.tPressed = Controls::Cancel.tPressed = 0;
	}
}

void Menu::Update(float dt, bool up, bool down, bool left, bool right)
{
	time += dt;
	if (!Focused) return;
	Menu* cur = Current();
	cur->time = time;
	cur->HandleInput(up, down, left, right);
	if (!IsInMainMenu() && Controls::Cancel.ConsumePress())
	{
		Audio::Play("main_menu_toggle_off");
		submenus.pop_back();
	}
}

Vec2 Menu::Size() const
{
	Vec2 size;
	float lh = Sprites::LineHeight();
	if (!Title.empty())
	{
		size.x = Sprites::TextWidth(Title.c_str());
		size.y += lh + SpacerHeight + Spacing;
	}
	for (auto& it : items)
	{
		if (it.kind == Item::Spacer) size.y += SpacerHeight;
		else
		{
			size.x = fmaxf(size.x, Sprites::TextWidth(it.Label().c_str()));
			size.y += lh;
		}
		size.y += Spacing;
	}
	if (!items.empty()) size.y -= Spacing;
	return size;
}

void Menu::RenderItems(float cx, float cy)
{
	Vec2 size = Size();
	float y = cy - size.y / 2;
	float lh = Sprites::LineHeight();
	if (!Title.empty())
	{
		float w = Sprites::TextWidth(Title.c_str());
		Sprites::Text2D(Title.c_str(), cx - w / 2, y, Color4::FromRGB(0x808080));
		y += lh + SpacerHeight + Spacing;
	}
	for (int i = 0; i < (int)items.size(); i++)
	{
		if (items[i].kind == Item::Spacer) { y += SpacerHeight; continue; }
		std::string text = items[i].Label();
		Color4 color(1, 1, 1, 1);
		if (Index == i && Focused)
			color = Color4::FromRGB(fmodf(time, 0.2f) >= 0.1f ? 0x84FF54 : 0xFCFF59);
		float w = Sprites::TextWidth(text.c_str());
		Sprites::Text2D(text.c_str(), cx - w / 2, y, color);
		y += lh + Spacing;
	}
}

void Menu::Render(float x, float y)
{
	Current()->RenderItems(x, y);
}
