// GE1 on-device generated replacements (see GSTextureOnDeviceUpscale.h).
#include "GS/Renderers/HW/GSTextureOnDeviceUpscale.h"

#include "GS/Renderers/HW/GSTextureFontFilter.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SHA256.h"
#include "pcsx2/Config.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <cstdlib>
#include <cstring>

extern bool FMVstarted;

namespace GE1OnDeviceUpscale
{
namespace
{

// One registry row per full replacement identity (ODP1 §3). Suffix packs
// the TextureName bits (PSM:6 TW:4 TH:4 TA0:8 AEM:1 TA1:8); region and
// miplevel are always base/full for these rows.
struct RegistryRow
{
	std::uint64_t tex0hash, cluthash;
	std::uint32_t suffix;
	int atlas;
	std::uint32_t expect_w, expect_h;
	const char* sfn_file; // atlases 2/72, else nullptr (baked words)
};

constexpr RegistryRow kRegistry[] = {
	{0x3be92b30fd7ab98aULL, 0x372720106ade32f4ULL, 0x00002254, 2, 512, 256, "DATA/FONTS/FEFONT.SFN"},
	{0xb7e032dcd63ad8dbULL, 0x372720106ade32f4ULL, 0x00001e14, 72, 256, 128, "DATA/FONTS/HUDFONT.SFN"},
	{0x51446d7052987c01ULL, 0xe5708d0189ccd302ULL, 0x0000221b, 70, 256, 256, nullptr},
	{0xfb36e14323437580ULL, 0x2f302f4bc33356f5ULL, 0x0000221b, 80, 256, 256, nullptr},
};
constexpr char kRecipe[] = "raster-edge:v1";

struct SfnStore
{
	bool present = false;
	std::string digest16 = "missing";
	std::vector<GE1FontFilter::SfnRow> rows;
};

std::mutex s_lock;
std::once_flag s_init_once;
bool s_initialized = false;
bool s_trace = false; // TEMPORARY P10 bring-up: GE1_ODU_TRACE=1. Removed before landing.
bool s_enabled = false;
unsigned s_generation = 0;
unsigned s_applied_seq = 0;
SfnStore s_sfn[2]; // [0] atlas 2, [1] atlas 72
std::string s_cache_root_override;
Stats s_stats;

int SfnIndex(int atlas)
{
	return atlas == 2 ? 0 : (atlas == 72 ? 1 : -1);
}

// Boot-log diagnostics: first occurrence per key (rare paths only; the
// hot miss path stays quiet).
std::set<std::string> s_logged;
void LogOnceLocked(const std::string& key, const std::string& msg)
{
	if (s_logged.insert(key).second)
		Console.WriteLn("ODU: %s", msg.c_str());
}

std::string Hex8(std::uint64_t v)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%08llx", static_cast<unsigned long long>(v & 0xffffffffULL));
	return std::string(buf);
}

std::string Hex16(std::uint64_t v)
{
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
	return std::string(buf);
}

std::string CacheRootLocked()
{
	if (!s_cache_root_override.empty())
		return s_cache_root_override;
	// EmuFolders::Cache is only populated through the settings UI path,
	// which GE1 never runs; fall back to the platform data root, else run
	// uncached (generate every time — correct, just slower).
	std::string base = EmuFolders::Cache;
	if (base.empty())
	{
		const char* d = std::getenv("GE1_GS_DATA_DIR");
		if (d && *d)
			base = d;
	}
	if (base.empty())
		base = EmuFolders::DataRoot;
	if (base.empty())
		return std::string();
	return Path::Combine(Path::Combine(base, "ondevice-upscale"), "v1");
}

bool ReadFile(const std::string& path, std::vector<std::uint8_t>& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (len < 0)
	{
		std::fclose(f);
		return false;
	}
	out.resize(static_cast<std::size_t>(len));
	const std::size_t got = std::fread(out.data(), 1, out.size(), f);
	std::fclose(f);
	return got == out.size();
}

bool WriteFileAtomic(const std::string& path, const std::uint8_t* data, std::size_t len)
{
	const std::string tmp = path + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const std::size_t wrote = std::fwrite(data, 1, len, f);
	std::fclose(f);
	if (wrote != len)
		return false;
	return FileSystem::RenamePath(tmp.c_str(), path.c_str(), nullptr);
}

// Deletes oldest PNGs (with manifests) while the recipe dir exceeds budget.
void TrimCacheLocked(const std::string& dir)
{
	constexpr std::int64_t kBudget = 256LL * 1024 * 1024;
	FileSystem::FindResultsArray files;
	if (!FileSystem::FindFiles(dir.c_str(), "*.png", 0, &files, nullptr))
		return;
	std::int64_t total = 0;
	for (const auto& f : files)
		total += f.Size;
	if (total <= kBudget)
		return;
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
		return a.ModificationTime < b.ModificationTime;
	});
	for (const auto& f : files)
	{
		if (total <= kBudget)
			break;
		const std::string png = Path::Combine(dir, f.FileName);
		std::string stem = f.FileName;
		if (stem.size() > 4)
			stem.resize(stem.size() - 4);
		FileSystem::DeleteFilePath(png.c_str(), nullptr);
		FileSystem::DeleteFilePath(Path::Combine(dir, stem + ".manifest").c_str(), nullptr);
		total -= f.Size;
	}
}

std::string CacheKey(const RegistryRow& row, const std::string& sfn_digest)
{
	return "f" + std::to_string(row.atlas) + "-" + Hex16(row.tex0hash) + "-" + Hex16(row.cluthash) + "-" +
	       Hex8(row.suffix) + "-" + sfn_digest + "-raster-edge-v1";
}

} // namespace

void EnsureInitialized()
{
	std::call_once(s_init_once, InitializeFromEnvironment);
}

void InitializeFromEnvironment()
{
	std::unique_lock<std::mutex> lock(s_lock);
	if (s_initialized)
		return;
	s_initialized = true;
	const char* e = std::getenv("GE1_ONDEVICE_UPSCALE");
	s_enabled = (e && std::strcmp(e, "1") == 0);
	const char* t = std::getenv("GE1_ODU_TRACE");
	s_trace = (t && std::strcmp(t, "1") == 0);
	if (s_enabled)
		s_generation = 1;
	const char* sfn_dir = std::getenv("GE1_ONDEVICE_SFN_DIR");
	int loaded = 0;
	if (sfn_dir && *sfn_dir)
	{
		for (const RegistryRow& row : kRegistry)
		{
			if (!row.sfn_file)
				continue;
			const char* base = std::strrchr(row.sfn_file, '/');
			const std::string path = Path::Combine(sfn_dir, base ? base + 1 : row.sfn_file);
			std::vector<std::uint8_t> bytes;
			if (ReadFile(path, bytes))
			{
				lock.unlock();
				const bool ok = SetSfnData(row.atlas, bytes.data(), bytes.size());
				lock.lock();
				loaded += ok ? 1 : 0;
			}
		}
	}
	Console.WriteLn("ODU: on-device upscale %s (sfn %d/2, cache %s).", s_enabled ? "ON" : "off", loaded,
		CacheRootLocked().c_str());
}

void ApplyOnDeviceUpscale(bool enabled, unsigned sequence)
{
	std::unique_lock<std::mutex> lock(s_lock);
	if (sequence <= s_applied_seq)
		return; // stale command
	s_applied_seq = sequence;
	s_enabled = enabled;
	s_generation++;
	Console.WriteLn("ODU: applied %s (seq %u, gen %u).", enabled ? "ON" : "off", sequence, s_generation);
}

bool IsEnabled()
{
	// The lookup gate asks before any miss runs: initialize here so the
	// open-time environment is honored on the very first lookup.
	EnsureInitialized();
	std::unique_lock<std::mutex> lock(s_lock);
	return s_enabled;
}

unsigned Generation()
{
	std::unique_lock<std::mutex> lock(s_lock);
	return s_generation;
}

bool SetSfnData(int atlas, const std::uint8_t* data, std::size_t len)
{
	const int idx = SfnIndex(atlas);
	if (idx < 0 || !data || len == 0)
		return false;
	std::vector<GE1FontFilter::SfnRow> rows;
	if (!GE1FontFilter::ParseSfn(data, len, rows) || rows.empty())
		return false;
	std::unique_lock<std::mutex> lock(s_lock);
	s_sfn[idx].present = true;
	s_sfn[idx].digest16 = SHA256Digest::HashHex(data, len).substr(0, 16);
	s_sfn[idx].rows = std::move(rows);
	return true;
}

bool HaveSfnData(int atlas)
{
	const int idx = SfnIndex(atlas);
	if (idx < 0)
		return false;
	std::unique_lock<std::mutex> lock(s_lock);
	return s_sfn[idx].present;
}

bool LookupGenerated(const GSTextureCache::HashCacheKey& key, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem, bool mipmap,
	GSTextureReplacements::ReplacementTexture& out, std::pair<std::uint8_t, std::uint8_t>* alpha_minmax)
{
	EnsureInitialized();

	std::unique_lock<std::mutex> lock(s_lock);
	s_stats.lookups++;
	if (!s_enabled)
	{
		s_stats.skipped_disabled++;
		return false;
	}
	if (FMVstarted)
	{
		s_stats.skipped_fmv++;
		return false;
	}
	if (mipmap || key.region_width != 0 || key.region_height != 0)
	{
		if (mipmap)
			s_stats.skipped_mipmap++;
		else
			s_stats.skipped_region++;
		return false;
	}
	// Same guard as the generic upscaler: the CPU CLUT copy must be live.
	if (GSLocalMemory::m_psm[TEX0.PSM].pal > 0 && mem.m_clut.GetGPUTexture())
	{
		s_stats.skipped_gpu_clut++;
		return false;
	}
	// Suffix bits use the key's masked TEXA (zero for paletted formats),
	// exactly like CreateTextureName — never the raw guest TEXA.
	const std::uint32_t suffix = static_cast<std::uint32_t>(TEX0.PSM) |
		(static_cast<std::uint32_t>(TEX0.TW) << 6) | (static_cast<std::uint32_t>(TEX0.TH) << 10) |
		(static_cast<std::uint32_t>(key.TEXA.TA0) << 15) | (static_cast<std::uint32_t>(key.TEXA.AEM) << 23) |
		(static_cast<std::uint32_t>(key.TEXA.TA1) << 24);
	const RegistryRow* row = nullptr;
	for (const RegistryRow& r : kRegistry)
	{
		if (r.tex0hash == key.TEX0Hash && r.cluthash == key.CLUTHash && r.suffix == suffix)
		{
			row = &r;
			break;
		}
	}
	if (s_trace)
	{
		Console.WriteLn("ODU: lookup tex0=%s clut=%s suffix=%08x region=%ux%u mipmap=%d match=%s.",
			Hex16(key.TEX0Hash).c_str(), Hex16(key.CLUTHash).c_str(), suffix,
			static_cast<unsigned>(key.region_width), static_cast<unsigned>(key.region_height),
			mipmap ? 1 : 0, row ? "yes" : "no");
	}
	if (!row)
		return false;
	s_stats.registry_hits++;
	{
		char msg[160];
		std::snprintf(msg, sizeof(msg), "registry hit atlas %d (tex0=%s clut=%s suffix=%s).",
			row->atlas, Hex16(row->tex0hash).c_str(), Hex16(row->cluthash).c_str(),
			Hex8(row->suffix).c_str());
		LogOnceLocked("hit-" + std::to_string(row->atlas), msg);
	}
	const SfnStore* sfn = nullptr;
	std::string sfn_digest = "words-v1";
	if (row->sfn_file)
	{
		const int idx = SfnIndex(row->atlas);
		if (!s_sfn[idx].present)
		{
			s_stats.skipped_no_sfn++;
			return false;
		}
		sfn = &s_sfn[idx];
		sfn_digest = sfn->digest16;
	}
	const std::string root = CacheRootLocked();
	const bool cache_on = !root.empty();
	const std::string dir = cache_on ? Path::Combine(Path::Combine(root, sfn_digest), kRecipe) : std::string();
	const std::string ckey = CacheKey(*row, sfn_digest);
	const std::string png_path = cache_on ? Path::Combine(dir, ckey + ".png") : std::string();
	const std::string man_path = cache_on ? Path::Combine(dir, ckey + ".manifest") : std::string();
	lock.unlock();

	// Warm path: validated disk cache first (no recomputation, no reboot).
	if (cache_on && FileSystem::FileExists(png_path.c_str()) && FileSystem::FileExists(man_path.c_str()))
	{
		GSTextureReplacements::ReplacementTextureLoader loader =
			GSTextureReplacements::GetLoader(png_path);
		GSTextureReplacements::ReplacementTexture cached;
		if (loader && loader(png_path, &cached, true) && cached.format == GSTexture::Format::Color &&
			cached.width == row->expect_w * 4 && cached.height == row->expect_h * 4 && !cached.data.empty())
		{
			// Alpha bounds from the actual pixels (same policy as packs).
			unsigned lo = 255, hi = 0;
			const std::uint8_t* px = cached.data.data();
			for (std::uint32_t yy = 0; yy < cached.height; yy++)
			{
				for (std::uint32_t xx = 0; xx < cached.width; xx++)
				{
					const unsigned a = px[static_cast<std::size_t>(yy) * cached.pitch + static_cast<std::size_t>(xx) * 4 + 3];
					lo = a < lo ? a : lo;
					hi = a > hi ? a : hi;
				}
			}
			*alpha_minmax = {static_cast<std::uint8_t>(lo), static_cast<std::uint8_t>(hi)};
			out = std::move(cached);
			lock.lock();
			s_stats.cache_hits++;
			lock.unlock();
			Console.WriteLn("ODU: cache hit atlas %d (%ux%u).", row->atlas, out.width, out.height);
			return true;
		}
		lock.lock();
		s_stats.cache_corrupt++;
		lock.unlock();
	}

	// Cold path: decode the guest level with the shared primitive, filter.
	bool decoded = false;
	std::vector<std::uint8_t> tight;
	std::uint32_t dw = 0, dh = 0;
	const bool ok = GSTextureReplacements::WithDecodedSourceLevel(
		TEX0, TEXA, region, mem,
		[&](const std::uint8_t* pixels, std::uint32_t w, std::uint32_t h, std::uint32_t pitch) {
			if (w != row->expect_w || h != row->expect_h)
				return false;
			tight.resize(static_cast<std::size_t>(w) * h * 4);
			for (std::uint32_t yy = 0; yy < h; yy++)
			{
				std::memcpy(tight.data() + static_cast<std::size_t>(yy) * w * 4,
					pixels + static_cast<std::size_t>(yy) * pitch, static_cast<std::size_t>(w) * 4);
			}
			dw = w;
			dh = h;
			decoded = true;
			return true;
		});
	if (!ok || !decoded)
	{
		lock.lock();
		s_stats.skipped_decode++;
		lock.unlock();
		return false;
	}
	if (dw != row->expect_w || dh != row->expect_h)
	{
		lock.lock();
		s_stats.skipped_geometry++;
		lock.unlock();
		return false;
	}

	std::vector<std::uint8_t> out_rgba;
	int ow = 0, oh = 0;
	GE1FontFilter::Stats fstat;
	const GE1FontFilter::SfnRow* sfn_rows = sfn ? sfn->rows.data() : nullptr;
	const int sfn_n = sfn ? static_cast<int>(sfn->rows.size()) : 0;
	if (!GE1FontFilter::BuildFontAtlas(row->atlas, tight.data(), static_cast<int>(dw), static_cast<int>(dh),
			sfn_rows, sfn_n, out_rgba, ow, oh, &fstat))
	{
		lock.lock();
		s_stats.skipped_geometry++;
		lock.unlock();
		return false;
	}

	// Persist privately (temp + rename, manifest last); loss is normal.
	// Written with the platform zlib: decode-equal to the oracle's PNGs
	// everywhere, byte-equal where the compressor matches (Mac harness
	// proves byte-equality against zlib-ng 1.3.1).
	if (cache_on)
	{
		Error err;
		FileSystem::CreateDirectoryPath(dir.c_str(), true, &err);
		const std::string tmp_png = png_path + ".tmp";
		if (GE1FontFilter::WriteFontPNG(tmp_png, out_rgba.data(), ow, oh) &&
			FileSystem::RenamePath(tmp_png.c_str(), png_path.c_str(), nullptr))
		{
			char man[256];
			std::snprintf(man, sizeof(man),
				"ondevice-upscale-manifest-v1\nrecipe=%s sharp=1.0 thresh=64 mode=smoothstep\n"
				"tex0=%s clut=%s suffix=%s\nsfn=%s\ndims=%ux%u\n",
				kRecipe, Hex16(row->tex0hash).c_str(), Hex16(row->cluthash).c_str(),
				Hex8(row->suffix).c_str(), sfn_digest.c_str(), static_cast<unsigned>(ow),
				static_cast<unsigned>(oh));
			const std::string tmp_man = man_path + ".tmp";
			WriteFileAtomic(tmp_man, reinterpret_cast<const std::uint8_t*>(man), std::strlen(man));
			FileSystem::RenamePath(tmp_man.c_str(), man_path.c_str(), nullptr);
			lock.lock();
			TrimCacheLocked(dir);
			lock.unlock();
		}
	}

	unsigned lo = 255, hi = 0;
	for (std::size_t i = 3; i < out_rgba.size(); i += 4)
	{
		lo = out_rgba[i] < lo ? out_rgba[i] : lo;
		hi = out_rgba[i] > hi ? out_rgba[i] : hi;
	}
	*alpha_minmax = {static_cast<std::uint8_t>(lo), static_cast<std::uint8_t>(hi)};
	out.width = static_cast<std::uint32_t>(ow);
	out.height = static_cast<std::uint32_t>(oh);
	// Color (RGBA8) like the pack PNG loader produces, not UNorm8 (A8).
	out.format = GSTexture::Format::Color;
	out.alpha_minmax = *alpha_minmax;
	out.pitch = static_cast<std::uint32_t>(ow) * 4;
	out.data.assign(out_rgba.begin(), out_rgba.end());
	lock.lock();
	s_stats.generated++;
	lock.unlock();
	Console.WriteLn("ODU: generated atlas %d (%dx%d, cells=%d regions=%d %.1f ms).", row->atlas, ow, oh,
		fstat.cells, fstat.regions, fstat.ms);
	return true;
}

Stats GetStats()
{
	std::unique_lock<std::mutex> lock(s_lock);
	return s_stats;
}

void SetCacheRootForTests(const std::string& dir)
{
	std::unique_lock<std::mutex> lock(s_lock);
	s_cache_root_override = dir;
}

void ResetForTests()
{
	std::unique_lock<std::mutex> lock(s_lock);
	s_initialized = false;
	s_enabled = false;
	s_generation = 0;
	s_applied_seq = 0;
	s_sfn[0] = SfnStore();
	s_sfn[1] = SfnStore();
	s_cache_root_override.clear();
	s_stats = Stats();
}

} // namespace GE1OnDeviceUpscale
