// GE1 on-device font filter: C++ port of tools/ssx3/remaster_font.py.
//
// Numbers-only port (see GSTextureFontFilter.h). Float discipline mirrors
// numpy: float32 stages use float, stages numpy promotes to float64 use
// double, left-to-right evaluation order everywhere, rint = banker's
// (FE_TONEAREST), narrowing casts truncate like astype(uint8).
// Compile with -ffp-contract=off.
#include "GSTextureFontFilter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

#include <zlib.h>

namespace GE1FontFilter
{
namespace
{

#include "GSTextureFontTables.inc"

inline float PiOver2F()
{
	return 1.5707963267948966f;
}

// ---- Pillow-mimic resizes (uint8, two-pass with uint8 intermediate) ----

// BILINEAR 4x: center map, clamp edges, dyadic weights, round-half-up.
void Bilinear4x(const std::uint8_t* in, int w, int h, std::uint8_t* out)
{
	// Horizontal pass into uint8 temp (4w x h).
	std::vector<std::uint8_t> tmp(static_cast<size_t>(4 * w) * h);
	for (int y = 0; y < h; y++)
	{
		for (int ox = 0; ox < 4 * w; ox++)
		{
			const double pos = (ox + 0.5) / 4.0 - 0.5;
			long long x0 = static_cast<long long>(std::floor(pos));
			const double fx = pos - static_cast<double>(x0);
			const long long x0c = x0 < 0 ? 0 : (x0 > w - 1 ? w - 1 : x0);
			long long x1 = x0 + 1;
			const long long x1c = x1 < 0 ? 0 : (x1 > w - 1 ? w - 1 : x1);
			const double v = (1.0 - fx) * in[y * w + x0c] + fx * in[y * w + x1c];
			tmp[static_cast<size_t>(y) * 4 * w + ox] = static_cast<std::uint8_t>(std::floor(v + 0.5));
		}
	}
	// Vertical pass.
	for (int y = 0; y < 4 * h; y++)
	{
		const double pos = (y + 0.5) / 4.0 - 0.5;
		long long y0 = static_cast<long long>(std::floor(pos));
		const double fy = pos - static_cast<double>(y0);
		const long long y0c = y0 < 0 ? 0 : (y0 > h - 1 ? h - 1 : y0);
		long long y1 = y0 + 1;
		const long long y1c = y1 < 0 ? 0 : (y1 > h - 1 ? h - 1 : y1);
		for (int x = 0; x < 4 * w; x++)
		{
			const double v = (1.0 - fy) * tmp[static_cast<size_t>(y0c) * 4 * w + x] +
			                 fy * tmp[static_cast<size_t>(y1c) * 4 * w + x];
			out[static_cast<size_t>(y) * 4 * w + x] = static_cast<std::uint8_t>(std::floor(v + 0.5));
		}
	}
}

// BOX 4x upscale: replicate (every output window lies in one input pixel).
void Box4xUp(const std::uint8_t* in, int w, int h, std::uint8_t* out)
{
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			const std::uint8_t v = in[y * w + x];
			for (int dy = 0; dy < 4; dy++)
			{
				std::uint8_t* row = out + (static_cast<size_t>(4 * y + dy) * 4 * w) + 4 * x;
				row[0] = row[1] = row[2] = row[3] = v;
			}
		}
	}
}

// BOX 2x downscale, two passes, round-half-up (matches Pillow BOX).
void BoxDown2x(const std::uint8_t* in, int w, int h, std::uint8_t* out)
{
	const int ow = w / 2, oh = h / 2;
	std::vector<std::uint8_t> tmp(static_cast<size_t>(ow) * h);
	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < ow; x++)
		{
			const double v = (in[y * w + 2 * x] + in[y * w + 2 * x + 1]) / 2.0;
			tmp[static_cast<size_t>(y) * ow + x] = static_cast<std::uint8_t>(std::floor(v + 0.5));
		}
	}
	for (int y = 0; y < oh; y++)
	{
		for (int x = 0; x < ow; x++)
		{
			const double v = (tmp[static_cast<size_t>(2 * y) * ow + x] + tmp[static_cast<size_t>(2 * y + 1) * ow + x]) / 2.0;
			out[static_cast<size_t>(y) * ow + x] = static_cast<std::uint8_t>(std::floor(v + 0.5));
		}
	}
}

inline double BilinearAt(const float* a, int w, int h, double xd, double yd)
{
	// numpy op order in _bilinear_grid: clip(floor) indices, fx/fy clipped,
	// per-corner products a*(1-fx)*(1-fy) etc. evaluated before the sum,
	// result ((p0+p1)+p2)+p3.
	const long long x0 = std::min<long long>(std::max<long long>(static_cast<long long>(std::floor(xd)), 0), w - 1);
	const long long y0 = std::min<long long>(std::max<long long>(static_cast<long long>(std::floor(yd)), 0), h - 1);
	const long long x1 = x0 + 1 > w - 1 ? w - 1 : x0 + 1;
	const long long y1 = y0 + 1 > h - 1 ? h - 1 : y0 + 1;
	double fx = xd - static_cast<double>(x0);
	double fy = yd - static_cast<double>(y0);
	fx = fx < 0.0 ? 0.0 : (fx > 1.0 ? 1.0 : fx);
	fy = fy < 0.0 ? 0.0 : (fy > 1.0 ? 1.0 : fy);
	const double p0 = static_cast<double>(a[y0 * w + x0]) * (1.0 - fx) * (1.0 - fy);
	const double p1 = static_cast<double>(a[y0 * w + x1]) * fx * (1.0 - fy);
	const double p2 = static_cast<double>(a[y1 * w + x0]) * (1.0 - fx) * fy;
	const double p3 = static_cast<double>(a[y1 * w + x1]) * fx * fy;
	return ((p0 + p1) + p2) + p3;
}

// EDGE x8 upsample of a float cell (port of upsample_edge). Returns 8x
// uint8 (truncated like astype(uint8)).
void UpsampleEdge8(const float* a, int w, int h, std::uint8_t* out)
{
	const int ow = w * 8, oh = h * 8;
	const size_t n = static_cast<size_t>(w) * h;
	std::vector<float> gx(n, 0.0f), gy(n, 0.0f);
	for (int y = 0; y < h; y++)
	{
		for (int x = 1; x < w - 1; x++)
			gx[static_cast<size_t>(y) * w + x] = (a[static_cast<size_t>(y) * w + x + 1] - a[static_cast<size_t>(y) * w + x - 1]) / 2.0f;
	}
	for (int y = 1; y < h - 1; y++)
	{
		for (int x = 0; x < w; x++)
			gy[static_cast<size_t>(y) * w + x] = (a[static_cast<size_t>(y + 1) * w + x] - a[static_cast<size_t>(y - 1) * w + x]) / 2.0f;
	}
	// 3x3 box blur of gx*gx, gy*gy, gx*gy; edge-replicated pad,
	// left-to-right float adds.
	std::vector<float> jxx(n), jyy(n), jxyv(n);
	std::vector<float> sqx(n), sqy(n), prod(n);
	for (size_t i = 0; i < n; i++)
	{
		sqx[i] = gx[i] * gx[i];
		sqy[i] = gy[i] * gy[i];
		prod[i] = gx[i] * gy[i];
	}
	const float* const blur_src[3] = {sqx.data(), sqy.data(), prod.data()};
	float* blur_dst[3] = {jxx.data(), jyy.data(), jxyv.data()};
	for (int k = 0; k < 3; k++)
	{
		const float* s = blur_src[k];
		float* d = blur_dst[k];
		for (int y = 0; y < h; y++)
		{
			for (int x = 0; x < w; x++)
			{
				const int xm = x > 0 ? x - 1 : 0, xp = x < w - 1 ? x + 1 : w - 1;
				const int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : h - 1;
				float v = s[ym * w + xm];
				v = v + s[ym * w + x];
				v = v + s[ym * w + xp];
				v = v + s[y * w + xm];
				v = v + s[y * w + x];
				v = v + s[y * w + xp];
				v = v + s[yp * w + xm];
				v = v + s[yp * w + x];
				v = v + s[yp * w + xp];
				d[static_cast<size_t>(y) * w + x] = v / 9.0f;
			}
		}
	}
	for (int oy = 0; oy < oh; oy++)
	{
		for (int ox = 0; ox < ow; ox++)
		{
			// Output coords in input px (float32 grid, then promoted).
			const double xd = static_cast<double>((static_cast<float>(ox) + 0.5f) / 8.0f - 0.5f);
			const double yd = static_cast<double>((static_cast<float>(oy) + 0.5f) / 8.0f - 0.5f);
			const double txx = BilinearAt(jxx.data(), w, h, xd, yd);
			const double tyy = BilinearAt(jyy.data(), w, h, xd, yd);
			const double txy = BilinearAt(jxyv.data(), w, h, xd, yd);
			const double tr = txx - tyy;
			const double disc = std::sqrt(tr * tr + 4.0 * txy * txy);
			const double den = (txx + tyy) > 1e-6 ? (txx + tyy) : 1e-6;
			const double coh = disc / den;
			const bool use = coh > 0.25;
			const double phi = 0.5 * std::atan2(2.0 * txy, tr) + 1.5707963267948966;
			const double ex = std::cos(phi);
			const double ey = std::sin(phi);
			const double base = BilinearAt(a, w, h, xd, yd);
			const double t0 = BilinearAt(a, w, h, xd - 1.5 * ex, yd - 1.5 * ey);
			const double t1 = BilinearAt(a, w, h, xd - 0.5 * ex, yd - 0.5 * ey);
			const double t2 = BilinearAt(a, w, h, xd + 0.5 * ex, yd + 0.5 * ey);
			const double t3 = BilinearAt(a, w, h, xd + 1.5 * ex, yd + 1.5 * ey);
			const double along = ((-t0 + 9.0 * t1) + 9.0 * t2 - t3) / 16.0;
			double v = use ? along : base;
			v = v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
			out[static_cast<size_t>(oy) * ow + ox] = static_cast<std::uint8_t>(v);
		}
	}
}

// Tone map + BOX to 4x (port of raster_ship_alpha, smoothstep default).
// in8: 8x uint8 EDGE output. Returns 4x uint8.
void ShipAlpha4x(const std::uint8_t* in8, int w8, int h8,
	float r_sharp, int thresh, std::uint8_t* out4)
{
	// numpy staging: g is float32, but g - th promotes to float64
	// (thresh/128.0 is a python float), so t and a are float64.
	const double th = static_cast<double>(thresh) / 128.0;
	const double w01 = static_cast<double>(r_sharp) * 0.125 > 0.02 ? static_cast<double>(r_sharp) * 0.125 : 0.02;
	std::vector<std::uint8_t> mapped(static_cast<size_t>(w8) * h8);
	for (int y = 0; y < h8; y++)
	{
		for (int x = 0; x < w8; x++)
		{
			float g = in8[static_cast<size_t>(y) * w8 + x] / 128.0f;
			g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
			double t = (static_cast<double>(g) - th + w01 / 2.0) / w01;
			t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
			const double a = (t * t) * (3.0 - 2.0 * t);
			mapped[static_cast<size_t>(y) * w8 + x] = static_cast<std::uint8_t>(a * 255.0);
		}
	}
	BoxDown2x(mapped.data(), w8, h8, out4);
}

// Fill predicates mirror fill_mask comparisons exactly (uint8 RGBA in).
inline bool FillPixel(int fill_class, int r, int g, int b, int a)
{
	if (a < 64)
		return false;
	switch (fill_class)
	{
		case 0: return r > 200 && g >= 40 && g < 120 && b < 60; // orange70
		case 1: return r > 140 && g > 180 && b < 40; // green70
		case 2: return r > 240 && g > 240 && b > 240; // white
		case 3: return r > 90 && g > 200 && b < 20; // green80
		case 4: return r > 180 && g > 80 && g < 150 && b < 40; // orange80
		default: return false;
	}
}

int RunsCount(const std::vector<char>& col_active)
{
	int n = 0;
	bool open = false;
	for (size_t x = 0; x <= col_active.size(); x++)
	{
		const bool active = x < col_active.size() ? col_active[x] != 0 : false;
		if (active && !open)
		{
			open = true;
		}
		else if (!active && open)
		{
			n++;
			open = false;
		}
	}
	return n;
}

} // namespace

static std::uint16_t ReadU16LE(const std::uint8_t* p)
{
	return static_cast<std::uint16_t>(p[0] | (static_cast<unsigned>(p[1]) << 8));
}

static std::uint32_t ReadU32LE(const std::uint8_t* p)
{
	return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
	       (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool ParseSfn(const std::uint8_t* data, size_t len, std::vector<SfnRow>& out)
{
	// Mirrors remaster_font.parse_sfn: FNTS magic, total size, count, table end.
	if (len < 0x80 || std::memcmp(data, "FNTS", 4) != 0)
		return false;
	if (ReadU32LE(data + 4) != len)
		return false;
	const unsigned count = ReadU16LE(data + 10);
	const std::uint32_t end = ReadU32LE(data + 0x18);
	if (0x80u + count * 12u != end || end > len)
		return false;
	out.clear();
	out.reserve(count);
	for (unsigned n = 0; n < count; n++)
	{
		const std::uint8_t* r = data + 0x80 + n * 12;
		SfnRow row;
		row.codepoint = ReadU16LE(r);
		row.w = r[2];
		row.h = r[3];
		row.x = ReadU16LE(r + 4);
		row.y = ReadU16LE(r + 6);
		// Bytes 8..11 (advance, x/y_offset, flags) unused by the recipe.
		out.push_back(row);
	}
	return true;
}

const char* DumpFilename(int atlas)
{
	switch (atlas)
	{
		case ATLAS_FE: return "3be92b30fd7ab98a-372720106ade32f4-00002254.png";
		case ATLAS_HUD: return "b7e032dcd63ad8db-372720106ade32f4-00001e14.png";
		case ATLAS_MENU: return "51446d7052987c01-e5708d0189ccd302-0000221b.png";
		case ATLAS_HUDWORDS: return "fb36e14323437580-2f302f4bc33356f5-0000221b.png";
		default: return nullptr;
	}
}

bool BuildFontAtlas(int atlas, const std::uint8_t* stock_rgba, int w, int h,
	const SfnRow* rows, int nrows,
	std::vector<std::uint8_t>& out_rgba, int& out_w, int& out_h, Stats* stats)
{
	const auto t0 = std::chrono::steady_clock::now();
	Stats st;
	const int w4 = w * 4, h4 = h * 4;
	const size_t n4 = static_cast<size_t>(w4) * h4;
	std::vector<std::uint8_t> rgb4(n4 * 3), a4u8(n4);
	// RGB bilinear 4x per channel.
	for (int c = 0; c < 3; c++)
	{
		std::vector<std::uint8_t> plane(static_cast<size_t>(w) * h), plane4(n4);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				plane[static_cast<size_t>(y) * w + x] = stock_rgba[(static_cast<size_t>(y) * w + x) * 4 + c];
		Bilinear4x(plane.data(), w, h, plane4.data());
		for (size_t i = 0; i < n4; i++)
			rgb4[i * 3 + c] = plane4[i];
	}
	// Alpha box 4x (replicate).
	{
		std::vector<std::uint8_t> plane(static_cast<size_t>(w) * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				plane[static_cast<size_t>(y) * w + x] = stock_rgba[(static_cast<size_t>(y) * w + x) * 4 + 3];
		Box4xUp(plane.data(), w, h, a4u8.data());
	}
	std::vector<float> a4f(n4);
	for (size_t i = 0; i < n4; i++)
		a4f[i] = static_cast<float>(a4u8[i]);

	if (atlas == ATLAS_MENU || atlas == ATLAS_HUDWORDS)
	{
		const WordRegion* words = atlas == ATLAS_MENU ? kWords70 : kWords80;
		const int nwords = atlas == ATLAS_MENU ? 5 : 3;
		for (int wi = 0; wi < nwords; wi++)
		{
			const int x0 = words[wi].x0, y0 = words[wi].y0;
			const int x1 = words[wi].x1, y1 = words[wi].y1;
			const int rw = x1 - x0, rh = y1 - y0;
			// Segment validation (mirrors _word_tiles).
			std::vector<char> col_active(rw, 0);
			for (int x = 0; x < rw; x++)
			{
				for (int y = 0; y < rh; y++)
				{
					const std::uint8_t* px = stock_rgba + (static_cast<size_t>(y0 + y) * w + x0 + x) * 4;
					if (FillPixel(words[wi].fill_class, px[0], px[1], px[2], px[3]))
					{
						col_active[x] = 1;
						break;
					}
				}
			}
			if (RunsCount(col_active) != words[wi].label_len)
				return false;
			// Binary ink tile, EDGE, ship.
			std::vector<std::uint8_t> ink(static_cast<size_t>(rw) * rh);
			for (int y = 0; y < rh; y++)
				for (int x = 0; x < rw; x++)
					ink[static_cast<size_t>(y) * rw + x] =
						stock_rgba[(static_cast<size_t>(y0 + y) * w + x0 + x) * 4 + 3] >= 64 ? 255 : 0;
			std::vector<float> inkf(static_cast<size_t>(rw) * rh);
			for (size_t i = 0; i < inkf.size(); i++)
				inkf[i] = static_cast<float>(ink[i]);
			std::vector<std::uint8_t> edge(static_cast<size_t>(rw) * 8 * rh * 8);
			UpsampleEdge8(inkf.data(), rw, rh, edge.data());
			std::vector<std::uint8_t> tile4(static_cast<size_t>(rw) * 4 * rh * 4);
			ShipAlpha4x(edge.data(), rw * 8, rh * 8, 1.0f, 64, tile4.data());
			st.regions++;
			long long sum = 0;
			for (std::uint8_t v : tile4)
				sum += v;
			if (sum == 0)
			{
				st.empty_kept_stock++;
				continue;
			}
			for (int y = 0; y < rh * 4; y++)
			{
				for (int x = 0; x < rw * 4; x++)
				{
					const float cand = std::rint(static_cast<float>(tile4[static_cast<size_t>(y) * rw * 4 + x]) * 128.0f / 255.0f);
					const float cur = a4f[(static_cast<size_t>(y0 * 4 + y)) * w4 + x0 * 4 + x];
					a4f[(static_cast<size_t>(y0 * 4 + y)) * w4 + x0 * 4 + x] = cand < cur ? cand : cur;
				}
			}
		}
	}
	else
	{
		std::vector<std::uint8_t> base(static_cast<size_t>(w) * h);
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				base[static_cast<size_t>(y) * w + x] = stock_rgba[(static_cast<size_t>(y) * w + x) * 4 + 3];
		std::set<std::tuple<int, int, int, int, int>> seen;
		for (int ci = 0; ci < nrows; ci++)
		{
			const int cp = rows[ci].codepoint, x = rows[ci].x, y = rows[ci].y;
			const int cw = rows[ci].w, ch = rows[ci].h;
			auto key = std::make_tuple(cp, x, y, cw, ch);
			if (seen.count(key))
				continue;
			seen.insert(key);
			st.cells++;
			if (cw * ch < 4)
			{
				st.tiny_kept_stock++;
				continue;
			}
			std::vector<float> cellf(static_cast<size_t>(cw) * ch);
			for (int yy = 0; yy < ch; yy++)
				for (int xx = 0; xx < cw; xx++)
					cellf[static_cast<size_t>(yy) * cw + xx] = static_cast<float>(base[static_cast<size_t>(y + yy) * w + x + xx]);
			std::vector<std::uint8_t> edge(static_cast<size_t>(cw) * 8 * ch * 8);
			UpsampleEdge8(cellf.data(), cw, ch, edge.data());
			std::vector<std::uint8_t> tile4(static_cast<size_t>(cw) * 4 * ch * 4);
			ShipAlpha4x(edge.data(), cw * 8, ch * 8, 1.0f, 64, tile4.data());
			long long sum = 0;
			for (std::uint8_t v : tile4)
				sum += v;
			if (sum == 0)
			{
				st.empty_kept_stock++;
				continue;
			}
			for (int yy = 0; yy < ch * 4; yy++)
			{
				for (int xx = 0; xx < cw * 4; xx++)
				{
					const float cand = std::rint(static_cast<float>(tile4[static_cast<size_t>(yy) * cw * 4 + xx]) * 128.0f / 255.0f);
					const float cur = a4f[(static_cast<size_t>(y * 4 + yy)) * w4 + x * 4 + xx];
					a4f[(static_cast<size_t>(y * 4 + yy)) * w4 + x * 4 + xx] = cand < cur ? cand : cur;
				}
			}
		}
	}

	out_w = w4;
	out_h = h4;
	out_rgba.resize(n4 * 4);
	for (size_t i = 0; i < n4; i++)
	{
		out_rgba[i * 4 + 0] = rgb4[i * 3 + 0];
		out_rgba[i * 4 + 1] = rgb4[i * 3 + 1];
		out_rgba[i * 4 + 2] = rgb4[i * 3 + 2];
		out_rgba[i * 4 + 3] = static_cast<std::uint8_t>(a4f[i]);
	}
	const auto t1 = std::chrono::steady_clock::now();
	st.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
	if (stats)
		*stats = st;
	return true;
}

namespace
{

// Pillow 12.3 ZipEncode replication (optimize off): per row, filter order
// None -> Up -> Sub -> Paeth with strict less-than on the wrapped-byte
// SAD, never Average; deflate level 6, window 15, mem 9, Z_FILTERED, one
// row per call. Byte-equal to the oracle's Pillow output when linked
// against the same zlib (v6c used zlib-ng 1.3.1).
long FilterSad(const std::uint8_t* b, size_t n)
{
	long s = 0;
	for (size_t i = 0; i < n; i++)
	{
		const unsigned v = b[i];
		s += v < 128 ? v : 256 - v;
	}
	return s;
}

void PutU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
	out.push_back(static_cast<std::uint8_t>(v >> 24));
	out.push_back(static_cast<std::uint8_t>(v >> 16));
	out.push_back(static_cast<std::uint8_t>(v >> 8));
	out.push_back(static_cast<std::uint8_t>(v));
}

void EmitChunk(std::vector<std::uint8_t>& out, const char* type, const std::uint8_t* data, size_t len)
{
	PutU32(out, static_cast<std::uint32_t>(len));
	const size_t hdr = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data, data + len);
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, out.data() + hdr, static_cast<uInt>(4 + len));
	PutU32(out, static_cast<std::uint32_t>(crc));
}

} // namespace

bool WriteFontPNG(const std::string& path, const std::uint8_t* rgba, int w, int h)
{
	const size_t stride = static_cast<size_t>(w) * 4;
	// Candidate buffers hold filter byte at [0], filtered bytes after.
	std::vector<std::uint8_t> none_buf(stride + 1, 0), up_buf(stride + 1, 2),
		sub_buf(stride + 1, 1), pae_buf(stride + 1, 4), prev(stride, 0);
	z_stream zs;
	std::memset(&zs, 0, sizeof(zs));
	if (deflateInit2_(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 9,
			Z_FILTERED, zlibVersion(), static_cast<int>(sizeof(zs))) != Z_OK)
		return false;
	std::vector<std::uint8_t> zdata;
	zdata.reserve(static_cast<size_t>(h) * (stride + 1) / 2);
	// Pillow feeds one 64 KiB output buffer shared across rows: when it
	// fills, the encoder returns the chunk and resumes (partially
	// consumed rows continue on the next call). Replicate that rhythm.
	std::vector<std::uint8_t> zout(65536);
	zs.next_out = zout.data();
	zs.avail_out = static_cast<uInt>(zout.size());
	auto drain = [&]() {
		zdata.insert(zdata.end(), zout.data(), zs.next_out);
		zs.next_out = zout.data();
		zs.avail_out = static_cast<uInt>(zout.size());
	};
	// One loop iteration below is one Pillow encode() call: an optional
	// resume followed by rows sharing the call's 64 KiB buffer, then (once
	// rows are done) finish bytes into the same call's remainder. The
	// buffer is drained only at call boundaries, never between a resume
	// and its following rows: zlib-ng's bytes depend on that rhythm.
	bool ok = true;
	int y = 0;
	bool finished = false;
	while (!finished && ok)
	{
		if (zs.next_in != nullptr && zs.avail_in > 0)
		{
			// Resume a row split by a full chunk (top of Pillow call).
			if (deflate(&zs, Z_NO_FLUSH) != Z_OK)
			{
				ok = false;
				break;
			}
		}
		while (y < h && zs.avail_out > 0 && ok)
		{
		const std::uint8_t* raw = rgba + static_cast<size_t>(y) * stride;
		std::memcpy(none_buf.data() + 1, raw, stride);
		long bestsum = FilterSad(none_buf.data() + 1, stride);
		const std::uint8_t* best = none_buf.data();
		if (bestsum > 0)
		{
			for (size_t i = 0; i < stride; i++)
				up_buf[i + 1] = static_cast<std::uint8_t>(raw[i] - prev[i]);
			const long s = FilterSad(up_buf.data() + 1, stride);
			if (s < bestsum)
			{
				bestsum = s;
				best = up_buf.data();
			}
		}
		if (bestsum > 0)
		{
			for (size_t i = 0; i < stride; i++)
				sub_buf[i + 1] = static_cast<std::uint8_t>(i < 4 ? raw[i] : raw[i] - raw[i - 4]);
			const long s = FilterSad(sub_buf.data() + 1, stride);
			if (s < bestsum)
			{
				bestsum = s;
				best = sub_buf.data();
			}
		}
		if (bestsum > 0)
		{
			for (size_t i = 0; i < stride; i++)
			{
				unsigned v;
				if (i < 4)
				{
					v = raw[i] - prev[i];
				}
				else
				{
					const int a = raw[i - 4], b = prev[i], c = prev[i - 4];
					const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
					const int pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
					v = static_cast<unsigned>(raw[i]) - static_cast<unsigned>(pr);
				}
				pae_buf[i + 1] = static_cast<std::uint8_t>(v);
			}
			const long s = FilterSad(pae_buf.data() + 1, stride);
			if (s < bestsum)
			{
				bestsum = s;
				best = pae_buf.data();
			}
		}
		zs.next_in = const_cast<Bytef*>(best);
		zs.avail_in = static_cast<uInt>(stride + 1);
		if (deflate(&zs, Z_NO_FLUSH) != Z_OK)
		{
			ok = false;
			break;
		}
		std::memcpy(prev.data(), raw, stride);
		y++;
		}
		if (ok && y >= h)
		{
			// End of rows: flush into this call's remaining buffer.
			int err = Z_OK;
			while (err == Z_OK && zs.avail_out > 0)
			{
				err = deflate(&zs, Z_FINISH);
				if (err != Z_OK && err != Z_STREAM_END)
				{
					ok = false;
					break;
				}
			}
			if (err == Z_STREAM_END)
				finished = true;
		}
		drain(); // encode() return: full chunk, or the final partial one
	}
	deflateEnd(&zs);
	if (!ok || zdata.empty())
		return false;
	static const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
	std::vector<std::uint8_t> out;
	out.insert(out.end(), sig, sig + 8);
	std::uint8_t ihdr[13] = {0};
	ihdr[0] = static_cast<std::uint8_t>(w >> 24);
	ihdr[1] = static_cast<std::uint8_t>(w >> 16);
	ihdr[2] = static_cast<std::uint8_t>(w >> 8);
	ihdr[3] = static_cast<std::uint8_t>(w);
	ihdr[4] = static_cast<std::uint8_t>(h >> 24);
	ihdr[5] = static_cast<std::uint8_t>(h >> 16);
	ihdr[6] = static_cast<std::uint8_t>(h >> 8);
	ihdr[7] = static_cast<std::uint8_t>(h);
	ihdr[8] = 8;
	ihdr[9] = 6;
	EmitChunk(out, "IHDR", ihdr, sizeof(ihdr));
	for (size_t o = 0; o < zdata.size(); o += 65536)
	{
		const size_t n = std::min<size_t>(65536, zdata.size() - o);
		EmitChunk(out, "IDAT", zdata.data() + o, n);
	}
	EmitChunk(out, "IEND", nullptr, 0);
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f)
		return false;
	const size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
	std::fclose(f);
	return wrote == out.size();
}

} // namespace GE1FontFilter

