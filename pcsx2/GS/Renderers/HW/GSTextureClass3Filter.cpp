#include "GS/Renderers/HW/GSTextureClass3Filter.h"

#include <chrono>
#include <cmath>

namespace GE1Class3Filter
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kScale = 4.0;
constexpr int kLobes = 3;
constexpr double kGaussSigma = 2.0;
constexpr double kGaussSpan = 3.0;
constexpr double kUnsharpPercent = 50.0;
constexpr double kUnsharpThreshold = 3.0;

double Sinc(double x)
{
	if (x == 0.0)
		return 1.0;
	const double pix = kPi * x;
	return std::sin(pix) / pix;
}

double LanczosKernel(double d)
{
	d = d < 0.0 ? -d : d;
	if (d >= kLobes)
		return 0.0;
	return Sinc(d) * Sinc(d / kLobes);
}

double ClampPlane(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Separable Lanczos 4x of one double plane, mirroring the fitted numpy
// op order (row pass into tmp, then column pass; per-pixel renormalized
// clamped 6-tap weights).
void Lanczos4xPlane(const double* in, int w, int h, double* out)
{
	const int W = static_cast<int>(w * kScale);
	const int H = static_cast<int>(h * kScale);
	std::vector<double> tmp(static_cast<size_t>(h) * W);
	for (int j = 0; j < W; j++)
	{
		const double x = (j + 0.5) / kScale - 0.5;
		int idx[2 * kLobes];
		double k[2 * kLobes];
		const int i0 = static_cast<int>(std::floor(x)) - (kLobes - 1);
		double sum = 0.0;
		for (int t = 0; t < 2 * kLobes; t++)
		{
			const int s = i0 + t;
			idx[t] = s < 0 ? 0 : (s > w - 1 ? w - 1 : s);
			k[t] = LanczosKernel(x - s);
			sum += k[t];
		}
		for (int t = 0; t < 2 * kLobes; t++)
			k[t] /= sum;
		for (int y = 0; y < h; y++)
		{
			double acc = 0.0;
			for (int t = 0; t < 2 * kLobes; t++)
				acc += in[static_cast<size_t>(y) * w + idx[t]] * k[t];
			tmp[static_cast<size_t>(y) * W + j] = acc;
		}
	}
	for (int i = 0; i < H; i++)
	{
		const double y = (i + 0.5) / kScale - 0.5;
		int idx[2 * kLobes];
		double k[2 * kLobes];
		const int j0 = static_cast<int>(std::floor(y)) - (kLobes - 1);
		double sum = 0.0;
		for (int t = 0; t < 2 * kLobes; t++)
		{
			const int s = j0 + t;
			idx[t] = s < 0 ? 0 : (s > h - 1 ? h - 1 : s);
			k[t] = LanczosKernel(y - s);
			sum += k[t];
		}
		for (int t = 0; t < 2 * kLobes; t++)
			k[t] /= sum;
		for (int j = 0; j < W; j++)
		{
			double acc = 0.0;
			for (int t = 0; t < 2 * kLobes; t++)
				acc += tmp[static_cast<size_t>(idx[t]) * W + j] * k[t];
			// rint (banker's, like numpy) matches the fitted chain: the
			// recipe rounds each Lanczos plane before unpremultiplying.
			out[static_cast<size_t>(i) * W + j] = std::rint(acc);
		}
	}
}

double HalfUp(double v)
{
	return std::floor(v + 0.5);
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

	// Split planes as doubles.
	std::vector<double> plane[4];
	for (int c = 0; c < 4; c++)
	{
		plane[c].resize(n);
		for (size_t i = 0; i < n; i++)
			plane[c][i] = stock_rgba[i * 4 + c];
	}

	// Premultiply RGB by alpha (half-up), mirroring Pillow's RGBA path.
	std::vector<double> pre[3];
	for (int c = 0; c < 3; c++)
	{
		pre[c].resize(n);
		for (size_t i = 0; i < n; i++)
			pre[c][i] = HalfUp(plane[c][i] * plane[3][i] / 255.0);
	}

	// Lanczos each band (premultiplied RGB + alpha), clip, half-up.
	std::vector<double> up[4];
	for (int c = 0; c < 3; c++)
	{
		up[c].resize(n4);
		Lanczos4xPlane(pre[c].data(), w, h, up[c].data());
	}
	up[3].resize(n4);
	Lanczos4xPlane(plane[3].data(), w, h, up[3].data());
	std::vector<double> rgb(n4 * 3), alpha(n4);
	for (size_t i = 0; i < n4; i++)
	{
		const double a = HalfUp(ClampPlane(up[3][i], 0.0, 255.0));
		alpha[i] = a;
		for (int c = 0; c < 3; c++)
		{
			const double p = ClampPlane(up[c][i], 0.0, 255.0);
			rgb[i * 3 + c] = (a == 0.0) ? 0.0 : HalfUp(p * 255.0 / a);
		}
	}

	// Gaussian blur of the rounded RGB (sigma 2, span 3, edge-clamped).
	const int gr = static_cast<int>(kGaussSigma * kGaussSpan + 0.5);
	std::vector<double> gk(static_cast<size_t>(2 * gr + 1));
	{
		double sum = 0.0;
		for (int t = -gr; t <= gr; t++)
		{
			const double v = std::exp(-0.5 * (t / kGaussSigma) * (t / kGaussSigma));
			gk[t + gr] = v;
			sum += v;
		}
		for (double& v : gk)
			v /= sum;
	}
	std::vector<double> blur(n4 * 3, 0.0), tmp(n4 * 3, 0.0);
	for (int c = 0; c < 3; c++)
	{
		for (int y = 0; y < H; y++)
		{
			for (int j = 0; j < W; j++)
			{
				double acc = 0.0;
				for (int t = -gr; t <= gr; t++)
				{
					int jj = j + t;
					jj = jj < 0 ? 0 : (jj > W - 1 ? W - 1 : jj);
					acc += rgb[(static_cast<size_t>(y) * W + jj) * 3 + c] * gk[t + gr];
				}
				tmp[(static_cast<size_t>(y) * W + j) * 3 + c] = acc;
			}
		}
		for (int i = 0; i < H; i++)
		{
			for (int j = 0; j < W; j++)
			{
				double acc = 0.0;
				for (int t = -gr; t <= gr; t++)
				{
					int ii = i + t;
					ii = ii < 0 ? 0 : (ii > H - 1 ? H - 1 : ii);
					acc += tmp[(static_cast<size_t>(ii) * W + j) * 3 + c] * gk[t + gr];
				}
				blur[(static_cast<size_t>(i) * W + j) * 3 + c] = acc;
			}
		}
	}

	// Per-channel unsharp, half-up quantize; alpha raw or clamped.
	out_rgba.resize(n4 * 4);
	for (size_t i = 0; i < n4; i++)
	{
		for (int c = 0; c < 3; c++)
		{
			const double v = rgb[i * 3 + c];
			const double d = v - blur[i * 3 + c];
			const double s = (d >= kUnsharpThreshold || d <= -kUnsharpThreshold) ?
			                 v + d * kUnsharpPercent / 100.0 :
			                 v;
			out_rgba[i * 4 + c] = static_cast<std::uint8_t>(ClampPlane(HalfUp(s), 0.0, 255.0));
		}
		double a = alpha[i];
		if (alpha_clamp >= 0 && a > alpha_clamp)
			a = alpha_clamp;
		out_rgba[i * 4 + 3] = static_cast<std::uint8_t>(ClampPlane(a, 0.0, 255.0));
	}
	out_w = W;
	out_h = H;
	if (ms)
	{
		*ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}
	return true;
}

} // namespace GE1Class3Filter
