// GE1 on-device font filter: C++ port of the raster-edge recipe.
//
// Lane REM3, Part 10. Pure C++17 (stdint/vector/string/cmath only) so it
// compiles both inside pcsx2/GS and in a standalone parity harness.
// The reference oracle is tools/ssx3/remaster_font.py; every numeric
// choice below mirrors it op-for-op, including float32/float64 staging
// (numpy promotion rules), Pillow BILINEAR/BOX conventions (center map,
// clamp, round-half-up), np.rint (banker's) and C truncation casts.
// Compile with -ffp-contract=off (matches GSTextureUpscaler.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace GE1FontFilter
{

// SFN glyph metrics parsed at runtime from the user's disc (never baked:
// other disc versions carry different tables). Field subset used by the
// recipe: codepoint (dedup identity; the oracle keys on chr(code)), x, y,
// width, height. Mirrors remaster_font.parse_sfn validation.
struct SfnRow
{
	int codepoint, x, y, w, h;
};

// Parses an EA Sports FNTS table; false on header/length mismatch.
bool ParseSfn(const std::uint8_t* data, size_t len, std::vector<SfnRow>& out);
struct WordRegion
{
	int label_len, x0, y0, x1, y1, fill_class;
};

enum
{
	ATLAS_FE = 2,
	ATLAS_HUD = 72,
	ATLAS_MENU = 70,
	ATLAS_HUDWORDS = 80,
};

struct Stats
{
	int cells = 0;
	int regions = 0;
	int tiny_kept_stock = 0;
	int empty_kept_stock = 0;
	double ms = 0.0;
};

// Builds the 4x pack canvas from stock RGBA (w*h*4 bytes, RGBA order).
// Atlases 2/72 take runtime-parsed SFN rows (file order, dups skipped);
// 70/80 ignore rows and use the baked word regions. Returns false when
// a word-region segmentation check fails.
bool BuildFontAtlas(int atlas, const std::uint8_t* stock_rgba, int w, int h,
	const SfnRow* rows, int nrows,
	std::vector<std::uint8_t>& out_rgba, int& out_w, int& out_h, Stats* stats = nullptr);

// Hash filenames of the four stock atlases (mirrors remaster_font.NAMES).
const char* DumpFilename(int atlas);

// PNG writer replicating the oracle's Pillow 12.3 ZipEncode (filter order
// None->Up->Sub->Paeth, wrapped-byte SAD, deflate 6/15/9/Z_FILTERED, one
// row per call, 64 KiB IDAT splits, no ancillary chunks). Byte-equal to
// the oracle's bytes when linked against the same zlib (v6c: zlib-ng
// 1.3.1). Returns false on zlib/file failure.
bool WriteFontPNG(const std::string& path, const std::uint8_t* rgba, int w, int h);

} // namespace GE1FontFilter
