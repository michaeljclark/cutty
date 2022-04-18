#pragma once

enum cu_render_flag
{
	cu_render_background = (1 << 0)
};

struct cu_render
{
	int flags;

	inline cu_render() : flags(cu_render_background) {}
	virtual ~cu_render() = default;

	virtual void set_overlay(bool val) = 0;
	virtual MVGCanvas* get_canvas() = 0;
	virtual ui9::Root* get_ui9root() = 0;

	virtual cu_winsize update() = 0;
	virtual void display() = 0;
	virtual void reshape(int width, int height, float scale) = 0;
	virtual void initialize() = 0;
};

cu_render* cu_render_new(font_manager_ft *manager, cu_cellgrid *cg);
