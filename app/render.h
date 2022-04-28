#pragma once

struct tty_render
{
	virtual ~tty_render() = default;

	virtual void set_overlay(bool val) = 0;

	virtual tty_winsize update() = 0;
	virtual void display() = 0;
	virtual void reshape(int width, int height, float scale) = 0;
	virtual void initialize() = 0;
};

tty_render* tty_render_new(font_manager_ft *manager, tty_cellgrid *cg);
