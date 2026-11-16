// Menu.cs: menú vertical con submenús, interruptores, selectores y deslizadores.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "math3d.h"

class Menu
{
public:
	struct Item
	{
		enum Kind { Option, Submenu, Toggle, MultiSelect, Slider, Spacer } kind = Option;
		std::string label;
		std::function<void()> action;            // Option / Toggle
		std::function<bool()> getBool;           // Toggle
		std::vector<std::string> options;        // MultiSelect
		std::function<int()> getInt;             // MultiSelect / Slider
		std::function<void(int)> setInt;
		int min = 0, max = 10;                   // Slider
		Menu* submenu = nullptr;

		bool Selectable() const { return kind != Spacer; }
		std::string Label() const;
	};

	std::string Title;
	int Index = 0;
	bool Focused = true;

	Menu& AddOption(const std::string& label, std::function<void()> action);
	Menu& AddSubmenu(const std::string& label, Menu* submenu);
	Menu& AddToggle(const std::string& label, std::function<void()> toggle, std::function<bool()> get);
	Menu& AddMultiSelect(const std::string& label, std::vector<std::string> options, std::function<int()> get, std::function<void(int)> set);
	Menu& AddSlider(const std::string& label, int min, int max, std::function<int()> get, std::function<void(int)> set);
	Menu& AddSpacer();

	bool IsInMainMenu() const { return submenus.empty(); }
	void CloseSubMenus() { submenus.clear(); }
	// lee Controls (Confirm/Cancel) y las direcciones de menú (D-pad / Circle Pad)
	void Update(float dt, bool up, bool down, bool left, bool right);
	// centrado en (x, y) de la pantalla de arriba
	void Render(float x, float y);

private:
	std::vector<Item> items;
	std::vector<Menu*> submenus;
	float time = 0;
	Menu* Current() { return submenus.empty() ? this : submenus.back(); }
	void HandleInput(bool up, bool down, bool left, bool right);
	Vec2 Size() const;
	void RenderItems(float x, float y);
};
