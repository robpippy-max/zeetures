/*
 *  SDLBackend.cpp
 *  openc2e
 *
 *  Created by Alyssa Milburn on Sun Oct 24 2004.
 *  Copyright (c) 2004 Alyssa Milburn. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 */

#include "SDLBackend.h"
#include "common/devenv.h"

#include "common/UiPinch.h"
#include "openc2e_icon.h"
#include <cstdio>
#include <cstdlib>

#include "common/Exception.h"
#include "common/NumericCast.h"
#include "common/backend/Keycodes.h"
#include "common/creaturesImage.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <array>
#include <cassert>
#include <filesystem>
#include <imgui.h>
#include <memory>

Backend* SDLBackend::get_instance() {
	static SDLBackend s_sdl_backend;
	return &s_sdl_backend;
}

SDLBackend::SDLBackend()
	: mainrendertarget(std::make_shared<SDLRenderTarget>(this)) {
	mainrendertarget->texture = nullptr;
}

static void ImGuiInit(SDL_Window* window, SDL_Renderer* renderer) {
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	io.IniFilename = nullptr; // don't save settings

	if (!ImGui_ImplSDL3_InitForSDLRenderer(window, renderer)) {
		throw Exception("Couldn't initialize ImGui SDL3");
	}
	if (!ImGui_ImplSDLRenderer3_Init(renderer)) {
		throw Exception("Couldn't initialize ImGui SDLRenderer");
	}

	io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;

	// the default font first (Fonts[0], used everywhere), then a bold sans
	// at a few exact sizes for the Hatchery's shelf labels, headings and card
	// (named "nursery N"; ImGuiUtils::NurseryFont finds them). From the
	// system's fonts when one is there; otherwise the default stands in.
	io.Fonts->AddFontDefault();
	{
		static const char* candidates[] = {
			"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
			"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
			"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
			"/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
			"/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
			"/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
			"C:\\Windows\\Fonts\\arialbd.ttf",
			"/System/Library/Fonts/Supplemental/Arial Bold.ttf",
		};
		for (const char* path : candidates) {
			if (!std::filesystem::exists(path))
				continue;
			for (int size : {8, 9, 10, 11, 15, 17}) {
				ImFontConfig cfg;
				// drawn at its own size through a nearest-filtered texture:
				// no oversampling, glyphs on whole pixels
				cfg.OversampleH = cfg.OversampleV = 1;
				cfg.PixelSnapH = true;
				snprintf(cfg.Name, sizeof(cfg.Name), "nursery %d", size);
				io.Fonts->AddFontFromFileTTF(path, (float)size, &cfg);
			}
			break;
		}
	}

	// fix for imgui#4768
	{
		ImGui_ImplSDLRenderer3_DestroyFontsTexture();
		auto* bd = ImGui_ImplSDLRenderer3_GetBackendData();
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		bd->FontTexture = SDL_CreateTexture(bd->Renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, width, height);
		if (bd->FontTexture == nullptr) {
			throw Exception("Couldn't create ImGui font texture");
		}
		SDL_UpdateTexture(bd->FontTexture, nullptr, pixels, 4 * width);
		SDL_SetTextureBlendMode(bd->FontTexture, SDL_BLENDMODE_BLEND);
		SDL_SetTextureScaleMode(bd->FontTexture, SDL_SCALEMODE_NEAREST);
		io.Fonts->SetTexID((ImTextureID)(intptr_t)bd->FontTexture);
	}
}

// Where the window opens (owner, 2026-11-10: on the monitor it was last on).
// OPENC2E_WINDOW_MEMORY names a file holding a point inside the monitor the
// window was last on; it is written when the window moves to another
// monitor and at shutdown, and read at start: when that point is on a
// monitor that is connected now, the window opens there. Otherwise
// OPENC2E_WINDOW_POS="x,y" (desktop coordinates), otherwise the window
// system decides.
static void rememberWindowDisplay(SDL_Window* window) {
	const char* file = getenv("OPENC2E_WINDOW_MEMORY");
	if (!file || !window)
		return;
	SDL_DisplayID display = SDL_GetDisplayForWindow(window);
	SDL_Rect bounds;
	if (!display || !SDL_GetDisplayBounds(display, &bounds))
		return;
	if (FILE* f = fopen(file, "w")) {
		fprintf(f, "%d,%d\n", bounds.x + bounds.w / 4, bounds.y + bounds.h / 4);
		fclose(f);
	}
}

static bool rememberedWindowPos(int& wx, int& wy) {
	const char* file = getenv("OPENC2E_WINDOW_MEMORY");
	if (!file)
		return false;
	FILE* f = fopen(file, "r");
	if (!f)
		return false;
	bool ok = fscanf(f, "%d,%d", &wx, &wy) == 2;
	fclose(f);
	if (!ok)
		return false;
	SDL_Point p{wx, wy};
	return SDL_GetDisplayForPoint(&p) != 0; // that monitor is connected now
}

void SDLBackend::init(const std::string& name, int32_t width, int32_t height) {
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		throw Exception(std::string("SDL error during initialization: ") + SDL_GetError());
	}

	window = SDL_CreateWindow(name.c_str(),
		width, height,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
	if (!window) {
		throw Exception(std::string("SDL error creating window: ") + SDL_GetError());
	}
	// OPENC2E_WINDOW_POS="x,y" places the window's top-left corner there
	// (desktop coordinates, so it can pick a monitor); otherwise the window
	// system decides
	int wx = 0, wy = 0;
	bool placed = rememberedWindowPos(wx, wy);
	if (!placed)
		if (const char* pos = getenv("OPENC2E_WINDOW_POS"); pos && sscanf(pos, "%d,%d", &wx, &wy) == 2)
			placed = true;
	if (placed)
		SDL_SetWindowPosition(window, wx, wy);
	// the window icon: openc2e's winged egg
	if (SDL_Surface* icon = SDL_CreateSurfaceFrom(openc2e_icon_size, openc2e_icon_size, SDL_PIXELFORMAT_RGBA32,
			(void*)openc2e_icon_rgba, openc2e_icon_size * 4)) {
		SDL_SetWindowIcon(window, icon);
		SDL_DestroySurface(icon);
	}
	// start maximized, on whichever monitor the window opened on; it is
	// created hidden so that the position above is where it first appears
	// (a window placed after it is shown stays where the window manager put it)
	SDL_ShowWindow(window);
	if (placed) {
		// the window manager may place a new window itself as it maps it:
		// move it again once shown, and wait for that before maximizing
		SDL_SetWindowPosition(window, wx, wy);
		SDL_SyncWindow(window);
	}
	SDL_MaximizeWindow(window);

	renderer = SDL_CreateRenderer(window, nullptr);
	if (!renderer) {
		throw Exception(std::string("SDL error creating renderer: ") + SDL_GetError());
	}

	SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
	fmt::print("* SDL Renderer: name={} max_texture_size={}x{} preferred_format={}\n",
		SDL_GetStringProperty(props, SDL_PROP_RENDERER_NAME_STRING, "unknown"),
		SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, -1),
		SDL_GetNumberProperty(props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, -1),
		SDL_GetPixelFormatName(((const SDL_PixelFormat*)SDL_GetPointerProperty(props, SDL_PROP_RENDERER_TEXTURE_FORMATS_POINTER, nullptr))[0]));

	SDL_HideCursor();
	SDL_StartTextInput(window);

	ImGuiInit(window, renderer);
}

void SDLBackend::shutdown() {
	rememberWindowDisplay(window);
	SDL_Quit();
}

static bool ImGuiConsumeEvent(const SDL_Event& event) {
	ImGui_ImplSDL3_ProcessEvent(&event);

	auto& io = ImGui::GetIO();

	static bool imguiHasCursorPrev = false;
	bool imguiHasCursor = io.WantCaptureMouse || io.WantCaptureKeyboard;
	if (imguiHasCursor != imguiHasCursorPrev) {
		if (imguiHasCursor) {
			io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
		} else {
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			SDL_HideCursor();
		}
		imguiHasCursorPrev = imguiHasCursor;
	}

	if (io.WantCaptureMouse && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_WHEEL ||
								   event.type == SDL_EVENT_PINCH_BEGIN || event.type == SDL_EVENT_PINCH_UPDATE || event.type == SDL_EVENT_PINCH_END)) {
		// a pinch over the interface is the interface's (UiPinch.h)
		if (event.type == SDL_EVENT_PINCH_UPDATE)
			UiPinch::Add(event.pinch.scale);
		return true;
	}
	if (io.WantCaptureKeyboard && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT)) {
		return true;
	}

	return false;
}

bool SDLBackend::pollEvent(BackendEvent& e) {
	SDL_Event event;
retry:
	if (!SDL_PollEvent(&event))
		return false;

	if (!SDL_ConvertEventToRenderCoordinates(renderer, &event)) {
		throw Exception(fmt::format("error converting event to render coordinates: {}", SDL_GetError()));
	}

	if (ImGuiConsumeEvent(event)) {
		goto retry;
	}

	switch (event.type) {
		case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			rememberWindowDisplay(window);
			goto retry;
		case SDL_EVENT_WINDOW_RESIZED:
			e.window_id = event.window.windowID;
			e.type = eventresizewindow;
			e.x = event.window.data1;
			e.y = event.window.data2;
			return true;

		case SDL_EVENT_MOUSE_MOTION:
			e.window_id = event.motion.windowID;
			e.type = eventmousemove;
			e.x = event.motion.x;
			e.y = event.motion.y - mainrendertarget->viewport_offset_top;
			e.xrel = event.motion.xrel;
			e.yrel = event.motion.yrel;
			e.button = 0;
			if (event.motion.state & SDL_BUTTON_MASK(1))
				e.button |= buttonleft;
			if (event.motion.state & SDL_BUTTON_MASK(2))
				e.button |= buttonmiddle;
			if (event.motion.state & SDL_BUTTON_MASK(3))
				e.button |= buttonright;
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			e.window_id = event.button.windowID;
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
				e.type = eventmousebuttondown;
			else
				e.type = eventmousebuttonup;
			switch (event.button.button) {
				case SDL_BUTTON_LEFT: e.button = buttonleft; break;
				case SDL_BUTTON_RIGHT: e.button = buttonright; break;
				case SDL_BUTTON_MIDDLE: e.button = buttonmiddle; break;
				default: goto retry;
			}
			e.x = event.button.x;
			e.y = event.button.y - mainrendertarget->viewport_offset_top;
			break;

		case SDL_EVENT_PINCH_UPDATE:
			e.window_id = event.pinch.windowID;
			e.type = eventpinch;
			e.pinch = event.pinch.scale;
			break;

		case SDL_EVENT_MOUSE_WHEEL:
			e.window_id = event.wheel.windowID;
			e.type = eventmousebuttondown;
			e.wheel_x = event.wheel.x;
			e.wheel_y = event.wheel.y;
			if (event.wheel.y > 0) {
				e.button = buttonwheeldown;
				break;
			} else if (event.wheel.y < 0) {
				e.button = buttonwheelup;
				break;
			} else if (event.wheel.x != 0) {
				e.button = 0; // horizontal-only scroll (touchpad)
				break;
			} else {
				goto retry;
			}
			break;

		case SDL_EVENT_TEXT_INPUT:
			e.window_id = event.text.windowID;
			e.type = eventtextinput;
			e.text = event.text.text;
			break;

		case SDL_EVENT_KEY_UP: {
			e.window_id = event.key.windowID;
			int key = translateScancode(event.key.scancode);
			if (key != -1) {
				e.type = eventrawkeyup;
				e.key = key;
				return true;
			}
			goto retry;
		}

		case SDL_EVENT_KEY_DOWN: {
			e.window_id = event.key.windowID;
			int key = translateScancode(event.key.scancode);
			if (key != -1) {
				e.type = eventrawkeydown;
				e.key = key;
				return true;
			}
			goto retry;
		}

		case SDL_EVENT_QUIT:
			e.type = eventquit;
			break;

		default:
			goto retry;
	}

	return true;
}

SDLRenderTarget::SDLRenderTarget(SDLBackend* parent_)
	: parent(parent_) {
}

SDLRenderTarget::~SDLRenderTarget() {
	if (texture != nullptr) {
		SDL_DestroyTexture(texture);
	}
}

void SDLRenderTarget::renderLine(float x1, float y1, float x2, float y2, Color color) {
	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_SetRenderDrawColor(parent->renderer, color.r, color.g, color.b, color.a);
	// (blended when see-through: SDL draws lines without blending unless
	// asked, so a colour's alpha was ignored and every line was solid)
	SDL_SetRenderDrawBlendMode(parent->renderer, color.a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
	SDL_RenderLine(parent->renderer, x1, y1 + viewport_offset_top, x2, y2 + viewport_offset_top);
}

Texture SDLBackend::createTexture(int32_t width, int32_t height) {
	// TODO: make sure we get an alpha-capable texture format. seems to always
	// happen by default, but should check just in case.
	Texture tex(
		SDL_CreateTexture(renderer, SDL_PIXELFORMAT_UNKNOWN, SDL_TEXTUREACCESS_STATIC, width, height),
		width, height, &SDL_DestroyTexture);
	if (!tex) {
		throw Exception(fmt::format("error creating texture: {}", SDL_GetError()));
	}
	// enable alpha blending
	SDL_SetTextureBlendMode(tex.as<SDL_Texture*>(), SDL_BLENDMODE_BLEND);
	// nearest pixel sampling for source textures
	SDL_SetTextureScaleMode(tex.as<SDL_Texture*>(), SDL_SCALEMODE_NEAREST);
	return tex;
}

struct SDLSurfaceDeleter {
	void operator()(SDL_Surface* ptr) {
		SDL_DestroySurface(ptr);
	}
};

void SDLBackend::updateTexture(Texture& tex, Rect2i location, const Image& image) {
	assert(tex);
	assert(image.data);
	assert(image.width > 0);
	assert(image.height > 0);
	if (image.format != if_index8) {
		assert(image.palette.data() == nullptr);
	}

	// map our format enum to SDL's format enum
	SDL_PixelFormat sdlformat = SDL_PIXELFORMAT_UNKNOWN;
	switch (image.format) {
		case if_index8:
			sdlformat = SDL_PIXELFORMAT_INDEX8;
			break;
		case if_rgb555:
			sdlformat = SDL_PIXELFORMAT_XRGB1555;
			break;
		case if_rgb565:
			sdlformat = SDL_PIXELFORMAT_RGB565;
			break;
		case if_bgr24:
			sdlformat = SDL_PIXELFORMAT_BGR24;
			break;
		case if_rgb24:
			sdlformat = SDL_PIXELFORMAT_RGB24;
			break;
		case if_rgba32:
			sdlformat = SDL_PIXELFORMAT_RGBA32;
			break;
	}

	// create initial surface
	std::unique_ptr<SDL_Surface, SDLSurfaceDeleter> surf{SDL_CreateSurfaceFrom(
		image.width,
		image.height,
		sdlformat,
		const_cast<uint8_t*>(image.data.data()),
		image.width * SDL_BYTESPERPIXEL(sdlformat) // pitch
		)};
	assert(surf);

	// set palette
	if (image.format == if_index8) {
		if (!image.palette) {
			throw Exception("Got indexed image without palette");
		}
		shared_array<SDL_Color> palette(image.palette.size());
		for (size_t i = 0; i < image.palette.size(); ++i) {
			palette[i].r = image.palette[i].r;
			palette[i].g = image.palette[i].g;
			palette[i].b = image.palette[i].b;
			palette[i].a = image.palette[i].a;
		}
		SDL_SetPaletteColors(SDL_CreateSurfacePalette(surf.get()), palette.data(), 0, palette.size());
	}

	// set colour-keying
	if (image.colorkey.a > 0) {
		if (image.colorkey.a != 255) {
			throw Exception("Expected alpha value of transparent color to be 255");
		}
		const Uint32 sdlcolorkey = SDL_MapRGB(
			SDL_GetPixelFormatDetails(surf->format),
			SDL_GetSurfacePalette(surf.get()),
			image.colorkey.r,
			image.colorkey.g,
			image.colorkey.b);
		SDL_SetSurfaceColorKey(surf.get(), true, sdlcolorkey);
	}

	// convert surface to texture format
	std::unique_ptr<SDL_Surface, SDLSurfaceDeleter> converted{
		SDL_ConvertSurface(surf.get(), tex.as<SDL_Texture*>()->format)};
	assert(converted);

	// update texture
	SDL_Rect rect;
	rect.x = location.x;
	rect.y = location.y;
	rect.w = location.width;
	rect.h = location.height;
	if (!SDL_UpdateTexture(tex.as<SDL_Texture*>(), location == Rect2i{} ? nullptr : &rect,
			converted->pixels, converted->pitch)) {
		throw Exception(fmt::format("error updating texture: {}", SDL_GetError()));
	};
}

int32_t SDLRenderTarget::getWidth() const {
	if (knownw < 0) {
		if (texture) {
			float width;
			SDL_GetTextureSize(texture, &width, nullptr);
			knownw = numeric_cast<int32_t>(width);
		} else {
			int width;
			SDL_GetWindowSizeInPixels(parent->window, &width, nullptr);
			knownw = numeric_cast<int32_t>(width / SDL_GetWindowDisplayScale(parent->window));
		}
	}
	return texture ? knownw : knownw - viewport_offset_right;
}

int32_t SDLRenderTarget::getHeight() const {
	if (knownh < 0) {
		if (texture) {
			float height;
			SDL_GetTextureSize(texture, nullptr, &height);
			assert(viewport_offset_top == 0);
			assert(viewport_offset_bottom == 0);
			knownh = numeric_cast<int32_t>(height);
		} else {
			int height;
			SDL_GetWindowSizeInPixels(parent->window, nullptr, &height);
			knownh = numeric_cast<int32_t>(height / SDL_GetWindowDisplayScale(parent->window));
		}
	}
	return texture ? knownh : knownh - viewport_offset_top - viewport_offset_bottom;
}

void SDLRenderTarget::setClip(Rect2f dest) {
	// TODO: SDL_RenderSetClipRectF doesn't seem to exist?
	SDL_Rect clip;
	clip.x = dest.x;
	clip.y = dest.y;
	clip.w = dest.width;
	clip.h = dest.height;
	if (!SDL_SetRenderClipRect(parent->renderer, dest == Rect2f{} ? nullptr : &clip)) {
		throw Exception(fmt::format("error in SDL_RenderSetClipRect(): {}", SDL_GetError()));
	}
}

void SDLRenderTarget::setViewportOffsetTop(int offset_top) {
	viewport_offset_top = offset_top;
}

void SDLRenderTarget::setViewportOffsetRight(int offset_right) {
	viewport_offset_right = offset_right;
}

void SDLRenderTarget::setViewportOffsetBottom(int offset_bottom) {
	viewport_offset_bottom = offset_bottom;
}

void SDLRenderTarget::renderTexture(const Texture& tex_, Rect2i src, Rect2f dest, RenderOptions options) {
	// dest is in window coordinates (callers have already added viewport_offset_top),
	// so the visible area ends at the viewport height plus that offset
	if (dest.right() + tex_.width <= 0 || dest.x >= getWidth() || dest.bottom() <= 0 || dest.y >= getHeight() + viewport_offset_top) {
		// cull non-visible textures
		return;
	}

	SDL_Texture* tex = const_cast<Texture&>(tex_).as<SDL_Texture*>();
	assert(tex);

	SDL_SetTextureAlphaMod(tex, options.alpha);
	SDL_SetTextureBlendMode(tex, options.multiply ? SDL_BLENDMODE_MOD : SDL_BLENDMODE_BLEND);
	if (options.smooth)
		SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
	SDL_FlipMode flip = options.mirror ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;

	SDL_FRect srcrect;
	srcrect.x = src.x;
	srcrect.y = src.y;
	srcrect.w = src.width;
	srcrect.h = src.height;

	SDL_FRect destrect;
	destrect.x = dest.x;
	destrect.y = dest.y;
	destrect.w = (options.override_drawsize ? options.overridden_drawwidth : dest.width) * options.scale;
	destrect.h = (options.override_drawsize ? options.overridden_drawheight : dest.height) * options.scale;

	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_RenderTextureRotated(parent->renderer, tex, &srcrect, &destrect, 0, nullptr, flip);
}

void SDLRenderTarget::renderCreaturesImage(creaturesImage& img, unsigned int frame, int x, int y, RenderOptions options) {
	// (culled by the size it is drawn at: a stretched image, like the light
	// filter, is far bigger than its picture)
	int drawnw = options.override_drawsize ? options.overridden_drawwidth : numeric_cast<int>(img.width(frame));
	int drawnh = options.override_drawsize ? options.overridden_drawheight : numeric_cast<int>(img.height(frame));
	if (x + drawnw * options.scale <= 0 || x >= numeric_cast<int>(getWidth()) || y + drawnh * options.scale <= 0 || y >= numeric_cast<int>(getHeight())) {
		return;
	}

	if (!img.getTextureForFrame(frame)) {
		img.getTextureForFrame(frame) = parent->createTextureFromImage(img.getImageForFrame(frame));
	}

	Rect2i src;
	src.x = img.getXOffsetForFrame(frame);
	src.y = img.getYOffsetForFrame(frame);
	src.width = img.width(frame);
	src.height = img.height(frame);

	Rect2f dest;
	dest.x = x;
	dest.y = y + viewport_offset_top;
	dest.width = (options.override_drawsize ? options.overridden_drawwidth : img.width(frame)) * options.scale;
	dest.height = (options.override_drawsize ? options.overridden_drawheight : img.height(frame)) * options.scale;

	return renderTexture(img.getTextureForFrame(frame), src, dest, options);
}

bool SDLRenderTarget::saveToFile(const std::string& path) {
	if (!texture)
		return false;
	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_Surface* shot = SDL_RenderReadPixels(parent->renderer, nullptr);
	SDL_SetRenderTarget(parent->renderer, nullptr);
	if (!shot)
		return false;
	// a plain 24-bit BMP, which every reader takes
	SDL_Surface* rgb = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_BGR24);
	SDL_DestroySurface(shot);
	if (!rgb)
		return false;
	bool ok = SDL_SaveBMP(rgb, path.c_str());
	SDL_DestroySurface(rgb);
	return ok;
}

void SDLRenderTarget::renderClear() {
	SDL_SetRenderDrawColor(parent->renderer, 0, 0, 0, 255);
	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_RenderClear(parent->renderer);
}

void SDLRenderTarget::blitRenderTarget(RenderTarget* s, Rect2f dest) {
	SDLRenderTarget* src = dynamic_cast<SDLRenderTarget*>(s);
	assert(src);

	SDL_FRect r;
	r.x = dest.x;
	r.y = dest.y + viewport_offset_top;
	r.w = dest.width;
	r.h = dest.height;
	SDL_SetRenderTarget(parent->renderer, texture);
	SDL_RenderTexture(parent->renderer, src->texture, nullptr, &r);
}

RenderTarget* SDLBackend::getMainRenderTarget() {
	return mainrendertarget.get();
}

std::shared_ptr<RenderTarget> SDLBackend::newRenderTarget(int32_t w, int32_t h) {
	SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_UNKNOWN, SDL_TEXTUREACCESS_TARGET, w, h);
	assert(texture);

	auto newtarget = std::make_shared<SDLRenderTarget>(this);
	newtarget->texture = texture;
	return std::dynamic_pointer_cast<RenderTarget>(newtarget);
}

// left out: menu, select, execute, snapshot, numeric keypad
struct _keytrans {
	int sdl;
	Openc2eKeycode openc2e;
};
static const std::array<_keytrans, 72> keytrans = {{
	{SDL_SCANCODE_BACKSPACE, OPENC2E_KEY_BACKSPACE},
	{SDL_SCANCODE_TAB, OPENC2E_KEY_TAB},
	{SDL_SCANCODE_CLEAR, OPENC2E_KEY_CLEAR},
	{SDL_SCANCODE_RETURN, OPENC2E_KEY_RETURN},
	{SDL_SCANCODE_RSHIFT, OPENC2E_KEY_SHIFT},
	{SDL_SCANCODE_LSHIFT, OPENC2E_KEY_SHIFT},
	{SDL_SCANCODE_RCTRL, OPENC2E_KEY_CTRL},
	{SDL_SCANCODE_LCTRL, OPENC2E_KEY_CTRL},
	{SDL_SCANCODE_PAUSE, OPENC2E_KEY_PAUSE},
	{SDL_SCANCODE_CAPSLOCK, OPENC2E_KEY_CAPSLOCK},
	{SDL_SCANCODE_ESCAPE, OPENC2E_KEY_ESCAPE},
	{SDL_SCANCODE_SPACE, OPENC2E_KEY_SPACE},
	{SDL_SCANCODE_PAGEUP, OPENC2E_KEY_PAGEUP},
	{SDL_SCANCODE_PAGEDOWN, OPENC2E_KEY_PAGEDOWN},
	{SDL_SCANCODE_END, OPENC2E_KEY_END},
	{SDL_SCANCODE_HOME, OPENC2E_KEY_HOME},
	{SDL_SCANCODE_LEFT, OPENC2E_KEY_LEFT},
	{SDL_SCANCODE_UP, OPENC2E_KEY_UP},
	{SDL_SCANCODE_RIGHT, OPENC2E_KEY_RIGHT},
	{SDL_SCANCODE_DOWN, OPENC2E_KEY_DOWN},
	{SDL_SCANCODE_PRINTSCREEN, OPENC2E_KEY_PRINTSCREEN},
	{SDL_SCANCODE_INSERT, OPENC2E_KEY_INSERT},
	{SDL_SCANCODE_DELETE, OPENC2E_KEY_DELETE},
	{SDL_SCANCODE_0, OPENC2E_KEY_0},
	{SDL_SCANCODE_1, OPENC2E_KEY_1},
	{SDL_SCANCODE_2, OPENC2E_KEY_2},
	{SDL_SCANCODE_3, OPENC2E_KEY_3},
	{SDL_SCANCODE_4, OPENC2E_KEY_4},
	{SDL_SCANCODE_5, OPENC2E_KEY_5},
	{SDL_SCANCODE_6, OPENC2E_KEY_6},
	{SDL_SCANCODE_7, OPENC2E_KEY_7},
	{SDL_SCANCODE_8, OPENC2E_KEY_8},
	{SDL_SCANCODE_9, OPENC2E_KEY_9},
	{SDL_SCANCODE_A, OPENC2E_KEY_A},
	{SDL_SCANCODE_B, OPENC2E_KEY_B},
	{SDL_SCANCODE_C, OPENC2E_KEY_C},
	{SDL_SCANCODE_D, OPENC2E_KEY_D},
	{SDL_SCANCODE_E, OPENC2E_KEY_E},
	{SDL_SCANCODE_F, OPENC2E_KEY_F},
	{SDL_SCANCODE_G, OPENC2E_KEY_G},
	{SDL_SCANCODE_H, OPENC2E_KEY_H},
	{SDL_SCANCODE_I, OPENC2E_KEY_I},
	{SDL_SCANCODE_J, OPENC2E_KEY_J},
	{SDL_SCANCODE_K, OPENC2E_KEY_K},
	{SDL_SCANCODE_L, OPENC2E_KEY_L},
	{SDL_SCANCODE_M, OPENC2E_KEY_M},
	{SDL_SCANCODE_N, OPENC2E_KEY_N},
	{SDL_SCANCODE_O, OPENC2E_KEY_O},
	{SDL_SCANCODE_P, OPENC2E_KEY_P},
	{SDL_SCANCODE_Q, OPENC2E_KEY_Q},
	{SDL_SCANCODE_R, OPENC2E_KEY_R},
	{SDL_SCANCODE_S, OPENC2E_KEY_S},
	{SDL_SCANCODE_T, OPENC2E_KEY_T},
	{SDL_SCANCODE_U, OPENC2E_KEY_U},
	{SDL_SCANCODE_V, OPENC2E_KEY_V},
	{SDL_SCANCODE_W, OPENC2E_KEY_W},
	{SDL_SCANCODE_X, OPENC2E_KEY_X},
	{SDL_SCANCODE_Y, OPENC2E_KEY_Y},
	{SDL_SCANCODE_Z, OPENC2E_KEY_Z},
	{SDL_SCANCODE_F1, OPENC2E_KEY_F1},
	{SDL_SCANCODE_F2, OPENC2E_KEY_F2},
	{SDL_SCANCODE_F3, OPENC2E_KEY_F3},
	{SDL_SCANCODE_F4, OPENC2E_KEY_F4},
	{SDL_SCANCODE_F5, OPENC2E_KEY_F5},
	{SDL_SCANCODE_F6, OPENC2E_KEY_F6},
	{SDL_SCANCODE_F7, OPENC2E_KEY_F7},
	{SDL_SCANCODE_F8, OPENC2E_KEY_F8},
	{SDL_SCANCODE_F9, OPENC2E_KEY_F9},
	{SDL_SCANCODE_F10, OPENC2E_KEY_F10},
	{SDL_SCANCODE_F11, OPENC2E_KEY_F11},
	{SDL_SCANCODE_F12, OPENC2E_KEY_F12},
	{SDL_SCANCODE_NUMLOCKCLEAR, OPENC2E_KEY_NUMLOCK},
}};

int SDLBackend::translateScancode(int key) {
	for (auto keytran : keytrans) {
		if (keytran.sdl == key)
			return keytran.openc2e;
	}

	return -1;
}

// TODO: this is possibly not a great idea, we should maybe maintain our own state table
bool SDLBackend::keyDown(Openc2eKeycode key) {
	const bool* keystate = SDL_GetKeyboardState(nullptr);

	for (auto keytran : keytrans) {
		if (keytran.openc2e == key)
			if (keystate[keytran.sdl])
				return true;
	}

	return false;
}

static constexpr int OPENC2E_MAX_FPS = 60;
static constexpr int OPENC2E_MIN_FPS = 20;

static bool should_quit = false;

bool sdl_event_watcher(void* userdata, SDL_Event* event) {
	(void)userdata;
	if (event->type == SDL_EVENT_QUIT) {
		should_quit = true;
	}
	return true;
}

void SDLBackend::setWindowTitle(const std::string& title) {
	if (window)
		SDL_SetWindowTitle(window, title.c_str());
}

bool SDLBackend::isFullscreen() {
	return window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
}

void SDLBackend::setFullscreen(bool on) {
	if (window)
		SDL_SetWindowFullscreen(window, on); // no mode set: the desktop's own size
}

void SDLBackend::run(std::function<bool()> update_callback) {
	SDL_AddEventWatch(sdl_event_watcher, nullptr);

	uint32_t last_frame_end = 0;

	while (true) {
		// TODO: we have to calculate renderer sizes when the backend is initialized,
		// otherwise side panels get in weird locations. related to issue with panels
		// when resizing in general?

		// (the window may have changed size since the last frame)
		mainrendertarget->knownw = mainrendertarget->knownh = -1;
		SDL_SetRenderTarget(renderer, nullptr); // (the scale is kept per target)
		SDL_SetRenderScale(renderer, SDL_GetWindowDisplayScale(window), SDL_GetWindowDisplayScale(window));

		bool focused = SDL_GetWindowFlags(window) & (SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS);
		// (developer aid: OPENC2E_FULL_RATE draws at the focused window's rate,
		// for measuring with a window nobody is looking at)
		static const bool fullRate = devenv("OPENC2E_FULL_RATE") != nullptr;
		focused = focused || fullRate;
		Uint32 desired_ticks_per_frame = 1000 / (focused ? OPENC2E_MAX_FPS : OPENC2E_MIN_FPS);
		Uint32 frame_end = SDL_GetTicks();
		if (frame_end - last_frame_end < desired_ticks_per_frame) {
			SDL_Delay(desired_ticks_per_frame - (frame_end - last_frame_end));
		}
		last_frame_end = SDL_GetTicks();

		// Closing a text box stops text input for the window, and typing to
		// the creatures needs it too: naming a new world left the hand unable
		// to speak until the world was opened again (reported 2026-11-22)
		if (!SDL_TextInputActive(window))
			SDL_StartTextInput(window);

		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();

		bool result = update_callback();
		if (should_quit || !result) {
			break;
		}

		ImGui::Render();
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

		// developer aid (the trailer, owner 2026-11-21): OPENC2E_RECORD=FILE.mp4
		// records what is drawn through ffmpeg at 30 frames a second of real
		// time (a frame drawn late is repeated, one drawn early skipped);
		// OPENC2E_RECORD_SECONDS=N stops after N seconds (the file is then
		// finished while the game runs on)
		if (const char* out = devenv("OPENC2E_RECORD")) {
			static FILE* rec = nullptr;
			static bool started = false, stopped = false;
			static Uint64 startMs = 0;
			static long frames = 0;
			static int rw = 0, rh = 0;
			static const double stopAfter = devenv("OPENC2E_RECORD_SECONDS") ? atof(devenv("OPENC2E_RECORD_SECONDS")) : 0.0;
			if (!stopped) {
				SDL_SetRenderTarget(renderer, nullptr);
				if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
					if (SDL_Surface* rgb = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGB24)) {
						if (!started) {
							started = true;
							rw = rgb->w;
							rh = rgb->h;
							std::string cmd = fmt::format("ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgb24 -s {}x{} -r 30 -i - "
														  "-c:v libx264 -preset veryfast -crf 14 -pix_fmt yuv420p \"{}\"",
								rw, rh, out);
#ifdef _WIN32
							rec = _popen(cmd.c_str(), "wb");
#else
							rec = popen(cmd.c_str(), "w");
#endif
							startMs = SDL_GetTicks();
							fmt::print("* Recording to {} ({}x{}, 30 fps)\n", out, rw, rh);
						}
						double t = (SDL_GetTicks() - startMs) / 1000.0;
						long want = (long)(t * 30.0) + 1;
						while (rec && frames < want && rgb->w == rw && rgb->h == rh) {
							for (int y = 0; y < rh; y++)
								fwrite((const uint8_t*)rgb->pixels + (size_t)y * rgb->pitch, 1, (size_t)rw * 3, rec);
							frames++;
						}
						if (rec && stopAfter > 0 && t >= stopAfter) {
#ifdef _WIN32
							_pclose(rec);
#else
							pclose(rec);
#endif
							rec = nullptr;
							stopped = true;
							fmt::print("* Recording finished: {} ({} frames)\n", out, frames);
						}
						SDL_DestroySurface(rgb);
					}
					SDL_DestroySurface(shot);
				}
			}
		}

		if (!pending_screenshot.empty()) {
			SDL_SetRenderTarget(renderer, nullptr);
			if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
				SDL_SaveBMP(shot, pending_screenshot.c_str());
				SDL_DestroySurface(shot);
				fmt::print("* Screenshot saved: {}\n", pending_screenshot);
			}
			pending_screenshot.clear();
		}

		SDL_RenderPresent(renderer);
	}
}