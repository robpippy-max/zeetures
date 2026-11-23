#include "ImageUtils.h"
#include <vector>
#include <algorithm>
#include <cmath>

#include <cstring>
#include "blkImage.h"
#include "bmpImage.h"
#include "c16Image.h"
#include "charsetdta.h"
#include "common/Ascii.h"
#include "common/Exception.h"
#include "common/io/SpanReader.h"
#include "common/mappedfile.h"
#include "s16Image.h"
#include "sprImage.h"
#include "pngImage.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace ImageUtils {

MultiImage ReadImage(const fs::path& path) {
	if (!fs::exists(path)) {
		throw_exception("File {:?} doesn't exist", path.string());
	}

	auto ext = to_ascii_lowercase(path.extension());

	mappedfile m(path);
	SpanReader in(m);
	if (ext == ".spr") {
		return ReadSprFile(in);
	}
	if (ext == ".s16" || ext == ".photo") {
		return ReadS16File(in);
	}
	if (ext == ".c16") {
		return ReadC16File(in);
	}
	if (ext == ".blk") {
		return {ReadBlkFile(in)};
	}
	if (ext == ".bmp") {
		return {ReadBmpFile(in)};
	}
	if (ext == ".png") {
		return ReadPngFile(in);
	}

	auto filename = path.filename();
	filename = to_ascii_lowercase(filename);

	if (filename == "charset.dta" || filename == "eurocharset.dta") {
		return ReadCharsetDtaFile(in);
	}

	throw_exception("Don't know how to read image {:?}", path.string());
}

bool IsBackground(const MultiImage& images) {
	if (images.size() == 464 || images.size() == 928) {
		for (const auto& image : images) {
			if (image.width != 144 || image.height != 150) {
				return false;
			}
		}
		return true;
	}
	return false;
}

Image StitchBackground(const MultiImage& images) {
	if (!IsBackground(images)) {
		throw Exception("Stitching non-background images not implemented");
	}

	const int sprwidth = 144;
	const int sprheight = 150;
	const int widthinsprites = 58;
	const unsigned int heightinsprites = [&] {
		if (images.size() == 464)
			return 8;
		if (images.size() == 928)
			return 16;
		throw Exception("Number of frames should be 464 or 928");
	}();
	const int bytes_per_pixel = [&] {
		if (images[0].format == if_index8)
			return 1;
		if (images[0].format == if_rgb555 || images[0].format == if_rgb565)
			return 2;
		throw Exception("Format should be 8-bit, RGB555, or RGB565");
	}();
	const int totalwidth = sprwidth * widthinsprites;
	const int totalheight = sprheight * heightinsprites;

	Image newimage;
	newimage.width = totalwidth;
	newimage.height = totalheight;
	newimage.format = images[0].format;
	newimage.palette = images[0].palette;
	newimage.data = shared_array<uint8_t>(totalwidth * totalheight * bytes_per_pixel);

	for (unsigned int i = 0; i < heightinsprites; i++) {
		for (int j = 0; j < widthinsprites; j++) {
			const unsigned int whereweare = j * heightinsprites + i;
			uint8_t* sprite = (uint8_t*)images[whereweare].data.data();
			for (int blocky = 0; blocky < sprheight; blocky++) {
				uint8_t* start = &newimage.data[(i * sprheight + blocky) * totalwidth * bytes_per_pixel + j * sprwidth * bytes_per_pixel];
				std::copy(&sprite[blocky * sprwidth * bytes_per_pixel], &sprite[blocky * sprwidth * bytes_per_pixel + sprwidth * bytes_per_pixel], start);
			}
		}
	}

	return newimage;
}

Image Stipple(const Image& oldimage) {
	Image newimage = oldimage;
	newimage.data = shared_array<uint8_t>(oldimage.data.size());
	std::memcpy(newimage.data.data(), oldimage.data.data(), oldimage.data.size());
	int bpp = oldimage.format == if_index8 ? 1 : oldimage.format == if_rgba32 ? 4 : (oldimage.format == if_bgr24 || oldimage.format == if_rgb24) ? 3 : 2;
	for (int y = 0; y < oldimage.height; y++)
		for (int x = (y + 1) % 2; x < oldimage.width; x += 2)
			std::memset(newimage.data.data() + ((size_t)y * oldimage.width + x) * bpp, 0, bpp); // 0 is transparent in each format
	return newimage;
}

Image Mute(const Image& src, float grey, float lift) {
	const bool sixteen = src.format == if_rgb565 || src.format == if_rgb555;
	const bool rgba = src.format == if_rgba32;
	if (!sixteen && !rgba)
		return src;
	Image out = src;
	const size_t n = (size_t)src.width * src.height;
	const int bpp = rgba ? 4 : 2;
	out.data = shared_array<uint8_t>(n * bpp);
	std::memcpy(out.data.data(), src.data.data(), n * bpp);
	auto mute = [&](float& r, float& g, float& b) {
		float l = 0.299f * r + 0.587f * g + 0.114f * b;
		r += (l - r) * grey, g += (l - g) * grey, b += (l - b) * grey;
		r += (255 - r) * lift, g += (255 - g) * lift, b += (255 - b) * lift;
	};
	for (size_t i = 0; i < n; i++) {
		uint8_t* p = out.data.data() + i * bpp;
		if (rgba) {
			if (p[3] == 0)
				continue;
			float r = p[0], g = p[1], b = p[2];
			mute(r, g, b);
			p[0] = (uint8_t)std::clamp(r, 0.f, 255.f), p[1] = (uint8_t)std::clamp(g, 0.f, 255.f), p[2] = (uint8_t)std::clamp(b, 0.f, 255.f);
			continue;
		}
		uint16_t v;
		std::memcpy(&v, p, 2);
		if (v == 0)
			continue; // transparent
		float r, g, b;
		if (src.format == if_rgb565)
			r = ((v >> 11) & 31) * 255 / 31.f, g = ((v >> 5) & 63) * 255 / 63.f;
		else
			r = ((v >> 10) & 31) * 255 / 31.f, g = ((v >> 5) & 31) * 255 / 31.f;
		b = (v & 31) * 255 / 31.f;
		mute(r, g, b);
		unsigned int R = (unsigned int)std::clamp(r, 0.f, 255.f), G = (unsigned int)std::clamp(g, 0.f, 255.f), B = (unsigned int)std::clamp(b, 0.f, 255.f);
		v = src.format == if_rgb565 ? (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3)) : (uint16_t)(((R >> 3) << 10) | ((G >> 3) << 5) | (B >> 3));
		if (v == 0)
			v = 1 << 5; // opaque black must not turn transparent
		std::memcpy(p, &v, 2);
	}
	return out;
}

Image Scale(const Image& src, float k) {
	const int w = std::max(1, (int)std::lround(src.width * k));
	const int h = std::max(1, (int)std::lround(src.height * k));
	const int bpp = src.format == if_index8 ? 1 : src.format == if_rgba32 ? 4 : (src.format == if_bgr24 || src.format == if_rgb24) ? 3 : 2;
	Image out = src;
	out.width = w;
	out.height = h;
	out.data = shared_array<uint8_t>((size_t)w * h * bpp);
	std::memset(out.data.data(), 0, (size_t)w * h * bpp);
	const uint8_t* in = src.data.data();
	const bool sixteen = src.format == if_rgb565 || src.format == if_rgb555;
	const bool rgba = src.format == if_rgba32;
	const int N = 4; // samples per side of each new pixel
	// the source column and row of each sample, worked out once (a world's
	// load spends a third of its time here)
	std::vector<int> sampleX((size_t)w * N), sampleY((size_t)h * N);
	for (int ox = 0; ox < w; ox++)
		for (int sx = 0; sx < N; sx++)
			sampleX[(size_t)ox * N + sx] = std::clamp((int)((ox + (sx + 0.5f) / N) / k), 0, src.width - 1);
	for (int oy = 0; oy < h; oy++)
		for (int sy = 0; sy < N; sy++)
			sampleY[(size_t)oy * N + sy] = std::clamp((int)((oy + (sy + 0.5f) / N) / k), 0, src.height - 1);
	for (int oy = 0; oy < h; oy++) {
		for (int ox = 0; ox < w; ox++) {
			uint8_t* dst = out.data.data() + ((size_t)oy * w + ox) * bpp;
			if (!sixteen && !rgba) { // nearest
				int px = std::clamp((int)((ox + 0.5f) / k), 0, src.width - 1);
				int py = std::clamp((int)((oy + 0.5f) / k), 0, src.height - 1);
				std::memcpy(dst, in + ((size_t)py * src.width + px) * bpp, bpp);
				continue;
			}
			int count = 0;
			unsigned int sr = 0, sg = 0, sb = 0;
			for (int sy = 0; sy < N; sy++) {
				const uint8_t* row = in + (size_t)sampleY[(size_t)oy * N + sy] * src.width * bpp;
				for (int sx = 0; sx < N; sx++) {
					const uint8_t* p = row + (size_t)sampleX[(size_t)ox * N + sx] * bpp;
					if (sixteen) {
						uint16_t v;
						std::memcpy(&v, p, 2);
						if (v == 0)
							continue; // transparent
						if (src.format == if_rgb565) {
							sr += ((v >> 11) & 31) * 255 / 31;
							sg += ((v >> 5) & 63) * 255 / 63;
						} else {
							sr += ((v >> 10) & 31) * 255 / 31;
							sg += ((v >> 5) & 31) * 255 / 31;
						}
						sb += (v & 31) * 255 / 31;
					} else {
						if (p[3] < 128)
							continue;
						sr += p[0];
						sg += p[1];
						sb += p[2];
					}
					count++;
				}
			}
			if (count * 2 < N * N)
				continue; // mostly transparent: transparent
			unsigned int r = sr / count, g = sg / count, b = sb / count;
			if (sixteen) {
				uint16_t v = src.format == if_rgb565 ? (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
													 : (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
				if (v == 0)
					v = 1 << 5; // opaque black must not turn transparent
				std::memcpy(dst, &v, 2);
			} else {
				dst[0] = (uint8_t)r;
				dst[1] = (uint8_t)g;
				dst[2] = (uint8_t)b;
				dst[3] = 255;
			}
		}
	}
	return out;
}

Image Turn(const Image& src, int cx, int cy, float angle, int& radius) {
	const int dx = std::max(cx, src.width - 1 - cx), dy = std::max(cy, src.height - 1 - cy);
	const int R = (int)std::ceil(std::hypot((float)dx, (float)dy)) + 1;
	radius = R;
	const int side = 2 * R + 1;
	const int bpp = src.format == if_index8 ? 1 : src.format == if_rgba32 ? 4 : (src.format == if_bgr24 || src.format == if_rgb24) ? 3 : 2;
	Image out = src;
	out.width = out.height = side;
	out.data = shared_array<uint8_t>((size_t)side * side * bpp);
	std::memset(out.data.data(), 0, (size_t)side * side * bpp);
	const uint8_t* in = src.data.data();
	const bool sixteen = src.format == if_rgb565 || src.format == if_rgb555;
	const bool rgba = src.format == if_rgba32;
	const int N = sixteen || rgba ? 4 : 1; // samples per side of each new pixel (others: nearest)
	const float c = std::cos(angle), s = std::sin(angle);
	for (int oy = 0; oy < side; oy++) {
		for (int ox = 0; ox < side; ox++) {
			uint8_t* dst = out.data.data() + ((size_t)oy * side + ox) * bpp;
			int count = 0;
			unsigned int sr = 0, sg = 0, sb = 0;
			for (int sy = 0; sy < N; sy++) {
				for (int sx = 0; sx < N; sx++) {
					// the sample, from the pivot pixel's middle, turned back
					const float u = ox + (sx + 0.5f) / N - R - 0.5f, v = oy + (sy + 0.5f) / N - R - 0.5f;
					const int px = (int)std::floor(c * u + s * v + cx + 0.5f), py = (int)std::floor(-s * u + c * v + cy + 0.5f);
					if (px < 0 || py < 0 || px >= src.width || py >= src.height)
						continue;
					const uint8_t* q = in + ((size_t)py * src.width + px) * bpp;
					if (sixteen) {
						uint16_t w;
						std::memcpy(&w, q, 2);
						if (w == 0)
							continue; // transparent
						if (src.format == if_rgb565) {
							sr += ((w >> 11) & 31) * 255 / 31;
							sg += ((w >> 5) & 63) * 255 / 63;
						} else {
							sr += ((w >> 10) & 31) * 255 / 31;
							sg += ((w >> 5) & 31) * 255 / 31;
						}
						sb += (w & 31) * 255 / 31;
					} else if (rgba) {
						if (q[3] < 128)
							continue;
						sr += q[0];
						sg += q[1];
						sb += q[2];
					} else {
						std::memcpy(dst, q, bpp);
					}
					count++;
				}
			}
			if ((!sixteen && !rgba) || count * 2 < N * N)
				continue;
			const unsigned int r = sr / count, g = sg / count, b = sb / count;
			if (sixteen) {
				uint16_t w = src.format == if_rgb565 ? (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
													 : (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
				if (w == 0)
					w = 1 << 5; // opaque black must not turn transparent
				std::memcpy(dst, &w, 2);
			} else {
				dst[0] = (uint8_t)r;
				dst[1] = (uint8_t)g;
				dst[2] = (uint8_t)b;
				dst[3] = 255;
			}
		}
	}
	return out;
}

Image Tint(const Image& oldimage, unsigned char r, unsigned char g, unsigned char b,
	unsigned char rotation, unsigned char swap) {
	if (!(oldimage.format == if_rgb565 || oldimage.format == if_rgb555)) {
		throw Exception("Tried to tint a sprite in an unsupported format");
	}

	if (128 == r && 128 == g && 128 == b && 128 == rotation && 128 == swap)
		return oldimage; // duh

	/*
   * CDN:
   * if rotation >= 128
   * absRot = rotation-128
   * else
   * absRot = 128 - rotation
   * endif
   * invRot = 127-absRot
   */
	int absRot;
	if (rotation >= 128)
		absRot = (int)rotation - 128;
	else
		absRot = 128 - (int)rotation;
	int invRot = 127 - absRot;

	/*
   * CDN:
   * if swap >= 128
   * absSwap = swap - 128
   * else
   * absSwap = 128 - swap
   * endif
   * invSwap = 127-absSwap
   */
	int absSwap;
	if (swap >= 128)
		absSwap = (int)swap - 128;
	else
		absSwap = 128 - (int)swap;
	int invSwap = 127 - absSwap;

	/*
   * CDN:
   * redTint = red-128
   * greenTint = green-128
   * blueTint = blue-128
   */

	int redTint = (int)r - 128;
	int greenTint = (int)g - 128;
	int blueTint = (int)b - 128;

	Image newimage;
	newimage.width = oldimage.width;
	newimage.height = oldimage.height;
	newimage.format = oldimage.format;
	newimage.data = shared_array<uint8_t>(oldimage.data.size());

	for (int j = 0; j < oldimage.height; j++) {
		for (int k = 0; k < oldimage.width; k++) {
			unsigned short v = ((unsigned short*)oldimage.data.data())[(j * oldimage.width) + k];
			if (v == 0) {
				((unsigned short*)newimage.data.data())[(j * newimage.width) + k] = 0;
				continue;
			};

			/*
			 * CDN:
			 * tempRed = RedValue + redTint;
			 * tempGreen = GreenValue + greenTint;
			 * tempBlue = BlueValue + blueTint;
			 */
			// TODO: should this work differently for 565 vs 555 color?
			int red = (((uint32_t)(v)&0xf800) >> 8) + redTint;
			if (red < 0)
				red = 0;
			else if (red > 255)
				red = 255;
			int green = (((uint32_t)(v)&0x07e0) >> 3) + greenTint;
			if (green < 0)
				green = 0;
			else if (green > 255)
				green = 255;
			int blue = (((uint32_t)(v)&0x001f) << 3) + blueTint;
			if (blue < 0)
				blue = 0;
			else if (blue > 255)
				blue = 255;

			/*
			 * CDN:
			 * if (rotation < 128)
			 * rotRed = ((absRot * tempBlue) + (invRot * tempRed)) / 256
			 * rotGreen = ((absRot * tempRed) + (invRot * tempGreen)) / 256
			 * rotBlue = ((absRot * tempGreen) + (invRot * tempBlue)) / 256
			 * endif
			 */

			int rotRed, rotGreen, rotBlue;
			rotRed = ((blue * absRot) + (red * invRot)) / 128;
			rotGreen = ((red * absRot) + (green * invRot)) / 128;
			rotBlue = ((green * absRot) + (blue * invRot)) / 128;


			/*
			 * CDN:
			 * swappedRed = ((absSwap * rotBlue) + (invSwap * rotRed))/256
			 * swappedBlue = ((absSwap * rotRed) + (invSwap * rotBlue))/256
			 *
			 * fuzzie notes that this doesn't seem to be a no-op for swap=128..
			 */
			int swappedRed = ((absSwap * rotBlue) + (invSwap * rotRed)) / 128;
			int swappedBlue = ((absSwap * rotRed) + (invSwap * rotBlue)) / 128;

			/*
			 * SetColour(definedcolour to (swappedRed,rotGreen,swappedBlue))
			 */
			swappedRed = (swappedRed << 8) & 0xf800;
			rotGreen = (rotGreen << 3) & 0x7e0;
			swappedBlue = (swappedBlue >> 3) & 0x1f;
			v = (swappedRed | rotGreen | swappedBlue);
			/*
			 * if definedcolour ==0 SetColour(definedcolour to (1,1,1))
			 */
			if (v == 0)
				v = (1 << 11 | 1 << 5 | 1);
			((unsigned short*)newimage.data.data())[(j * newimage.width) + k] = v;
		}
	}

	return newimage;
}

Color GetPixelColor(const Image& image, unsigned int x, unsigned int y) {
	if (image.format == if_index8) {
		uint8_t palette_index = image.data[y * image.width + x];
		return image.palette[palette_index];

	} else if (image.format == if_rgb555) {
		uint16_t pixel = ((uint16_t*)image.data.data())[y * image.width + x];
		uint8_t r = (pixel & 0x7C00) >> 10;
		uint8_t g = (pixel & 0x03E0) >> 5;
		uint8_t b = (pixel & 0x001F);
		r = r * 255 / 31;
		g = g * 255 / 31;
		b = b * 255 / 31;
		return Color{r, g, b, 255};

	} else if (image.format == if_rgb565) {
		uint16_t pixel = ((uint16_t*)image.data.data())[y * image.width + x];
		uint8_t r = (pixel & 0xF800) >> 11;
		uint8_t g = (pixel & 0x07E0) >> 5;
		uint8_t b = (pixel & 0x001F);
		r = r * 255 / 31;
		g = g * 255 / 63;
		b = b * 255 / 31;
		return Color{r, g, b, 255};

	} else if (image.format == if_rgba32) {
		const uint8_t* p = &image.data[(y * image.width + x) * 4];
		return Color{p[0], p[1], p[2], p[3]};

	} else if (image.format == if_bgr24) {
		uint8_t b = image.data[(y * image.width + x) * 3];
		uint8_t g = image.data[(y * image.width + x) * 3 + 1];
		uint8_t r = image.data[(y * image.width + x) * 3 + 2];
		return Color{r, g, b, 255};

	} else {
		throw Exception("GetPixelColor unimplemented for format " + std::to_string(image.format));
	}
}

Image ToRGB24(const Image& oldimage) {
	if (oldimage.format == if_rgb24)
		return oldimage;

	Image newimage = oldimage;
	newimage.data = shared_array<uint8_t>(newimage.width * newimage.height * 3);
	newimage.format = if_rgb24;

	for (int j = 0; j < oldimage.width * oldimage.height; ++j) {
		uint8_t r, g, b;
		if (oldimage.format == if_rgb565) {
			uint16_t pixel = *(((uint16_t*)oldimage.data.data()) + j);
			r = ((pixel & 0xF800) >> 11) * 255 / 31;
			g = ((pixel & 0x07E0) >> 5) * 255 / 63;
			b = ((pixel & 0x001F)) * 255 / 31;

		} else if (oldimage.format == if_rgb555) {
			uint16_t pixel = *(((uint16_t*)oldimage.data.data()) + j);
			r = ((pixel & 0x7C00) >> 10) * 255 / 31;
			g = ((pixel & 0x03E0) >> 5) * 255 / 31;
			b = ((pixel & 0x001F)) * 255 / 31;

		} else if (oldimage.format == if_bgr24) {
			r = oldimage.data[j * 3 + 2];
			g = oldimage.data[j * 3 + 1];
			b = oldimage.data[j * 3];

		} else if (oldimage.format == if_index8) {
			r = oldimage.palette[oldimage.data[j]].r;
			g = oldimage.palette[oldimage.data[j]].g;
			b = oldimage.palette[oldimage.data[j]].b;

		} else {
			throw Exception("Expected format RGB565, RGB555, BGR24, or INDEX8");
		}
		newimage.data[j * 3] = r;
		newimage.data[j * 3 + 1] = g;
		newimage.data[j * 3 + 2] = b;
	}
	return newimage;
}

} // namespace ImageUtils