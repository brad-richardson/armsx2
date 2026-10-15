#include "GS/Renderers/HW/GSTextureClass3Filter.h"

#include <chrono>
#include <cmath>
#include <cstring>

namespace GE1Class3Filter
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr int kUnsharpPercent = 50;
constexpr int kUnsharpThreshold = 3;
constexpr int kBoxPasses = 3;

// Pillow Resample.c port (LANCZOS): per-output-pixel bounds, double coeffs
// normalized per pixel, INT32 fixed-point (2^22, half-up), accumulate from
// 2^21, clip to uint8. H-pass writes uint8, V-pass reads it — the clip
// between passes (including negative-lobe clipping) is part of the recipe.
constexpr int kResamplePrecision = 22;

double PillowSinc(double x)
{
	if (x == 0.0)
		return 1.0;
	x *= kPi;
	return std::sin(x) / x;
}

double PillowLanczos(double x)
{
	if (-3.0 <= x && x < 3.0)
		return PillowSinc(x) * PillowSinc(x / 3);
	return 0.0;
}

struct ResampleCoeffs
{
	int ksize = 0;
	std::vector<int> xmin; // per output pixel
	std::vector<int> count; // per output pixel
	std::vector<std::int32_t> k; // per output pixel x ksize, fixed-point
};

void PrecomputeCoeffs(int inSize, int outSize, ResampleCoeffs& c)
{
	const double scale = static_cast<double>(inSize) / outSize;
	const double filterscale = scale < 1.0 ? 1.0 : scale;
	const double support = 3.0 * filterscale;
	const double inv = 1.0 / filterscale;
	c.ksize = static_cast<int>(std::ceil(support)) * 2 + 1;
	c.xmin.assign(outSize, 0);
	c.count.assign(outSize, 0);
	c.k.assign(static_cast<size_t>(outSize) * c.ksize, 0);
	for (int xx = 0; xx < outSize; xx++)
	{
		const double center = (xx + 0.5) * scale;
		double ww = 0.0;
		int xmin = static_cast<int>(center - support + 0.5);
		if (xmin < 0)
			xmin = 0;
		int xmax = static_cast<int>(center + support + 0.5);
		if (xmax > inSize)
			xmax = inSize;
		xmax -= xmin;
		std::vector<double> w(static_cast<size_t>(c.ksize), 0.0);
		for (int x = 0; x < xmax; x++)
		{
			const double wt = PillowLanczos((x + xmin - center + 0.5) * inv);
			w[x] = wt;
			ww += wt;
		}
		if (ww != 0.0)
		{
			for (int x = 0; x < xmax; x++)
			{
				const double n = w[x] / ww * (1 << kResamplePrecision);
				c.k[static_cast<size_t>(xx) * c.ksize + x] =
					static_cast<std::int32_t>(n < 0 ? -0.5 + n : 0.5 + n);
			}
		}
		c.xmin[xx] = xmin;
		c.count[xx] = xmax;
	}
}

std::int32_t ClipResample(std::int64_t ss)
{
	const std::int64_t v = ss >> kResamplePrecision;
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return static_cast<std::int32_t>(v);
}

void ResampleHPlane(const std::uint8_t* in, std::uint8_t* out, int w, int h, int W,
	const ResampleCoeffs& c)
{
	for (int yy = 0; yy < h; yy++)
	{
		const std::uint8_t* lineIn = in + static_cast<size_t>(yy) * w;
		std::uint8_t* lineOut = out + static_cast<size_t>(yy) * W;
		for (int xx = 0; xx < W; xx++)
		{
			const int xmin = c.xmin[xx];
			const int xmax = c.count[xx];
			const std::int32_t* k = &c.k[static_cast<size_t>(xx) * c.ksize];
			std::int64_t ss = static_cast<std::int64_t>(1) << (kResamplePrecision - 1);
			for (int x = 0; x < xmax; x++)
				ss += static_cast<std::int64_t>(lineIn[x + xmin]) * k[x];
			lineOut[xx] = static_cast<std::uint8_t>(ClipResample(ss));
		}
	}
}

void ResampleVPlane(const std::uint8_t* in, std::uint8_t* out, int W, int h, int H,
	const ResampleCoeffs& c)
{
	for (int xx = 0; xx < W; xx++)
	{
		for (int yy = 0; yy < H; yy++)
		{
			const int ymin = c.xmin[yy];
			const int ymax = c.count[yy];
			const std::int32_t* k = &c.k[static_cast<size_t>(yy) * c.ksize];
			std::int64_t ss = static_cast<std::int64_t>(1) << (kResamplePrecision - 1);
			for (int y = 0; y < ymax; y++)
				ss += static_cast<std::int64_t>(in[static_cast<size_t>(y + ymin) * W + xx]) * k[y];
			out[static_cast<size_t>(yy) * W + xx] = static_cast<std::uint8_t>(ClipResample(ss));
		}
	}
}

// Full Pillow resize of one uint8 band (H uint8 intermediate, like Pillow).
void PillowResizeBand(const std::uint8_t* in, int w, int h, std::uint8_t* out, int W, int H)
{
	ResampleCoeffs ch, cv;
	PrecomputeCoeffs(w, W, ch);
	PrecomputeCoeffs(h, H, cv);
	std::vector<std::uint8_t> mid(static_cast<size_t>(W) * h);
	ResampleHPlane(in, mid.data(), w, h, W, ch);
	ResampleVPlane(mid.data(), out, W, h, H, cv);
}

// Pillow BoxBlur.c (ImagingLineBoxBlur8) verbatim port for one strided
// single-band line: fixed-point weights ww/fw, round-half-up SAVE, clamped
// edges. out must not alias in within a pass (ping-pong buffers).
void BoxBlurLine(const std::uint8_t* in, std::uint8_t* out, int len, int stride,
	int radius, int edgeA, int edgeB, std::uint32_t ww, std::uint32_t fw)
{
	std::uint64_t acc = static_cast<std::uint64_t>(in[0]) * static_cast<std::uint64_t>(radius + 1);
	for (int x = 0; x < edgeA - 1; x++)
		acc += in[static_cast<size_t>(x) * stride];
	acc += static_cast<std::uint64_t>(in[static_cast<size_t>(len - 1) * stride]) *
		static_cast<std::uint64_t>(radius - edgeA + 1);
	if (edgeA <= edgeB)
	{
		for (int x = 0; x < edgeA; x++)
		{
			acc += in[static_cast<size_t>(x + radius) * stride] - in[0];
			const std::uint64_t bulk = acc * ww +
				(static_cast<std::uint64_t>(in[0]) + in[static_cast<size_t>(x + radius + 1) * stride]) * fw;
			out[static_cast<size_t>(x) * stride] = static_cast<std::uint8_t>((bulk + (1u << 23)) >> 24);
		}
		for (int x = edgeA; x < edgeB; x++)
		{
			acc += in[static_cast<size_t>(x + radius) * stride] - in[static_cast<size_t>(x - radius - 1) * stride];
			const std::uint64_t bulk = acc * ww +
				(static_cast<std::uint64_t>(in[static_cast<size_t>(x - radius - 1) * stride]) +
					in[static_cast<size_t>(x + radius + 1) * stride]) * fw;
			out[static_cast<size_t>(x) * stride] = static_cast<std::uint8_t>((bulk + (1u << 23)) >> 24);
		}
		for (int x = edgeB; x < len; x++)
		{
			acc += in[static_cast<size_t>(len - 1) * stride] - in[static_cast<size_t>(x - radius - 1) * stride];
			const std::uint64_t bulk = acc * ww +
				(static_cast<std::uint64_t>(in[static_cast<size_t>(x - radius - 1) * stride]) +
					in[static_cast<size_t>(len - 1) * stride]) * fw;
			out[static_cast<size_t>(x) * stride] = static_cast<std::uint8_t>((bulk + (1u << 23)) >> 24);
		}
	}
	else
	{
		for (int x = 0; x < edgeB; x++)
		{
			acc += in[static_cast<size_t>(x + radius) * stride] - in[0];
			const std::uint64_t bulk = acc * ww +
				(static_cast<std::uint64_t>(in[0]) + in[static_cast<size_t>(x + radius + 1) * stride]) * fw;
			out[static_cast<size_t>(x) * stride] = static_cast<std::uint8_t>((bulk + (1u << 23)) >> 24);
		}
		for (int x = edgeB; x < edgeA; x++)
		{
			acc += in[static_cast<size_t>(len - 1) * stride] - in[0];
			const std::uint64_t bulk = acc * ww +
				(static_cast<std::uint64_t>(in[0]) + in[static_cast<size_t>(len - 1) * stride]) * fw;
			out[static_cast<size_t>(x) * stride] = static_cast<std::uint8_t>((bulk + (1u << 23)) >> 24);
		}
		for (int x = edgeA; x < len; x++)
		{
			acc += in[static_cast<size_t>(len - 1) * stride] - in[static_cast<size_t>(x - radius - 1) * stride];
			const std::uint64_t bulk = acc * ww +
				(static_cast<std::uint64_t>(in[static_cast<size_t>(x - radius - 1) * stride]) +
					in[static_cast<size_t>(len - 1) * stride]) * fw;
			out[static_cast<size_t>(x) * stride] = static_cast<std::uint8_t>((bulk + (1u << 23)) >> 24);
		}
	}
}

// Gwosdek boxes-for-Gauss box radius, replicating BoxBlur.c float math.
// radius 2, 3 passes -> 1.375 (int radius 1, ww 4473924, fw 1677722).
float BoxRadiusForGauss(float radius, int passes)
{
	const float sigma2 = radius * radius / passes;
	const float L = static_cast<float>(std::sqrt(12.0 * sigma2 + 1.0));
	const float l = std::floor((L - 1.0f) / 2.0f);
	float a = (2 * l + 1) * (l * (l + 1) - 3 * sigma2);
	a /= 6 * (sigma2 - (l + 1) * (l + 1));
	return l + a;
}

// Pillow GaussianBlur(radius=2): 3 horizontal box passes, 3 vertical
// (strided; transpose-exact), uint8 between passes. One band.
void PillowBlur2(const std::uint8_t* in, std::uint8_t* out, int w, int h)
{
	const float floatRadius = BoxRadiusForGauss(2.0f, kBoxPasses);
	const int radius = static_cast<int>(floatRadius);
	const std::uint32_t ww = static_cast<std::uint32_t>(16777216.0f / (floatRadius * 2 + 1));
	const std::uint32_t fw = (16777216u - static_cast<std::uint32_t>(radius * 2 + 1) * ww) / 2;
	const size_t n = static_cast<size_t>(w) * h;
	std::vector<std::uint8_t> ping(n), pong(n);
	std::memcpy(ping.data(), in, n);
	const int edgeAH = radius + 1 < w ? radius + 1 : w;
	const int edgeBH = w - radius - 1 > 0 ? w - radius - 1 : 0;
	for (int p = 0; p < kBoxPasses; p++)
	{
		for (int y = 0; y < h; y++)
			BoxBlurLine(ping.data() + static_cast<size_t>(y) * w, pong.data() + static_cast<size_t>(y) * w,
				w, 1, radius, edgeAH, edgeBH, ww, fw);
		ping.swap(pong);
	}
	const int edgeAV = radius + 1 < h ? radius + 1 : h;
	const int edgeBV = h - radius - 1 > 0 ? h - radius - 1 : 0;
	for (int p = 0; p < kBoxPasses; p++)
	{
		for (int x = 0; x < w; x++)
			BoxBlurLine(ping.data() + x, pong.data() + x, h, w, radius, edgeAV, edgeBV, ww, fw);
		ping.swap(pong);
	}
	std::memcpy(out, ping.data(), n);
}

// Pillow RGBA resize premultiplication (Convert.c): Image.resize on RGBA
// converts to RGBa first and back after. Premult rounds half-up
// ((v*a+127)/255); unpremult truncates (255*p/a, clipped), passing alpha
// 0/255 through. Probed off Pillow 12.3 (REM3 P13).
void PremultBand(const std::uint8_t* rgb, const std::uint8_t* alpha, std::uint8_t* out, size_t n)
{
	for (size_t i = 0; i < n; i++)
		out[i] = static_cast<std::uint8_t>(
			(static_cast<unsigned>(rgb[i]) * alpha[i] + 127) / 255);
}

void UnpremultBand(const std::uint8_t* premult, const std::uint8_t* alpha, std::uint8_t* out, size_t n)
{
	for (size_t i = 0; i < n; i++)
	{
		const unsigned a = alpha[i];
		if (a == 255 || a == 0)
			out[i] = premult[i];
		else
		{
			const unsigned v = (255u * premult[i]) / a;
			out[i] = static_cast<std::uint8_t>(v > 255 ? 255 : v);
		}
	}
}

} // namespace

bool BuildLanczosAtlas(const std::uint8_t* stock_rgba, int w, int h, int alpha_clamp,
	std::vector<std::uint8_t>& out_rgba, int& out_w, int& out_h, double* ms)
{
	if (!stock_rgba || w <= 0 || h <= 0)
		return false;
	const auto t0 = std::chrono::steady_clock::now();
	const int W = w * 4, H = h * 4;
	const size_t n = static_cast<size_t>(w) * h;
	const size_t n4 = static_cast<size_t>(W) * H;

	// v5f art recipe (build_options.lanczos): premultiply the stock (round
	// half-up), Pillow-resize the RGBa bands, unpremultiply back to straight
	// RGB (truncate, clip: the resize call's RGBa->RGBA convert), UnsharpMask
	// the straight RGB (convert('RGB') drops the band), and attach the
	// resized alpha clamped to the row peak.
	std::vector<std::uint8_t> up[4], bands(n4 * 3), alpha(n4), blur(n4 * 3);
	{
		std::vector<std::uint8_t> band(n), pre(n);
		for (int c = 0; c < 3; c++)
		{
			for (size_t i = 0; i < n; i++)
				band[i] = stock_rgba[i * 4 + c];
			for (size_t i = 0; i < n; i++)
				pre[i] = stock_rgba[i * 4 + 3];
			// premultiply by the stock alpha (alpha band resizes straight)
			std::vector<std::uint8_t> prem(n);
			PremultBand(band.data(), pre.data(), prem.data(), n);
			up[c].resize(n4);
			PillowResizeBand(prem.data(), w, h, up[c].data(), W, H);
		}
		{
			std::vector<std::uint8_t> ab(n);
			for (size_t i = 0; i < n; i++)
				ab[i] = stock_rgba[i * 4 + 3];
			up[3].resize(n4);
			PillowResizeBand(ab.data(), w, h, up[3].data(), W, H);
		}
		std::vector<std::uint8_t> un(n4);
		for (size_t i = 0; i < n4; i++)
			alpha[i] = up[3][i];
		for (int c = 0; c < 3; c++)
		{
			UnpremultBand(up[c].data(), up[3].data(), un.data(), n4);
			for (size_t i = 0; i < n4; i++)
				bands[i * 3 + c] = un[i];
		}
	}
	// Pillow GaussianBlur(radius=2) of the resized RGB, one band at a time.
	{
		std::vector<std::uint8_t> band(n4), bout(n4);
		for (int c = 0; c < 3; c++)
		{
			for (size_t i = 0; i < n4; i++)
				band[i] = bands[i * 3 + c];
			PillowBlur2(band.data(), bout.data(), W, H);
			for (size_t i = 0; i < n4; i++)
				blur[i * 3 + c] = bout[i];
		}
	}

	// Per-channel Pillow UnsharpMask(radius=2, percent=50, threshold=3):
	// diff = orig - blurred (uint8 both); sharpen iff |diff| > 3;
	// out = clip(orig + diff*50/100) with C truncation toward zero.
	out_rgba.resize(n4 * 4);
	for (size_t i = 0; i < n4; i++)
	{
		for (int c = 0; c < 3; c++)
		{
			const int v = static_cast<int>(bands[i * 3 + c]);
			const int d = v - static_cast<int>(blur[i * 3 + c]);
			int u = v;
			if (d > kUnsharpThreshold || d < -kUnsharpThreshold)
			{
				u = v + d * kUnsharpPercent / 100;
				u = u < 0 ? 0 : (u > 255 ? 255 : u);
			}
			out_rgba[i * 4 + c] = static_cast<std::uint8_t>(u);
		}
		int a = static_cast<int>(alpha[i]);
		if (alpha_clamp >= 0 && a > alpha_clamp)
			a = alpha_clamp;
		out_rgba[i * 4 + 3] = static_cast<std::uint8_t>(a);
	}
	out_w = W;
	out_h = H;
	if (ms)
	{
		*ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
	return true;
}

// One mip step of the RMT9 DDS chain: the previous RGBA level through
// Pillow's RGBA resize (premult round half-up, Resample.c fixed-point,
// unpremult truncate+clip). Alpha resizes straight.
bool BuildMipLevel(const std::uint8_t* prev_rgba, int w, int h,
	std::vector<std::uint8_t>& out_rgba, int& out_w, int& out_h, double* ms)
{
	if (!prev_rgba || w <= 0 || h <= 0 || (w == 1 && h == 1))
		return false;
	const auto t0 = std::chrono::steady_clock::now();
	out_w = w == 1 ? 1 : w / 2;
	out_h = h == 1 ? 1 : h / 2;
	const size_t n = static_cast<size_t>(w) * h;
	const size_t nm = static_cast<size_t>(out_w) * out_h;

	// RMT9 chain() resizes the RGBA level: premultiply (round half-up),
	// Pillow-resize, unpremultiply (truncate, clip). Alpha resizes straight.
	std::vector<std::uint8_t> band(n), prem(n), bout(nm), alpha(n), alphaOut(nm);
	for (size_t i = 0; i < n; i++)
		alpha[i] = prev_rgba[i * 4 + 3];
	PillowResizeBand(alpha.data(), w, h, alphaOut.data(), out_w, out_h);
	out_rgba.resize(nm * 4);
	for (int c = 0; c < 3; c++)
	{
		for (size_t i = 0; i < n; i++)
			band[i] = prev_rgba[i * 4 + c];
		PremultBand(band.data(), alpha.data(), prem.data(), n);
		PillowResizeBand(prem.data(), w, h, bout.data(), out_w, out_h);
		std::vector<std::uint8_t> un(nm);
		UnpremultBand(bout.data(), alphaOut.data(), un.data(), nm);
		for (size_t i = 0; i < nm; i++)
			out_rgba[i * 4 + c] = un[i];
	}
	for (size_t i = 0; i < nm; i++)
		out_rgba[i * 4 + 3] = alphaOut[i];
	if (ms)
		*ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return true;
}
} // namespace GE1Class3Filter
