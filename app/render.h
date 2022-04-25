#pragma once

struct cu_render
{
	virtual ~cu_render() = default;

	virtual void set_overlay(bool val) = 0;

	virtual cu_winsize update() = 0;
	virtual void display() = 0;
	virtual void reshape(int width, int height, float scale) = 0;
	virtual void initialize() = 0;
};

cu_render* cu_render_new(font_manager_ft *manager, cu_cellgrid *cg);
