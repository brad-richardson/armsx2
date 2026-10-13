// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

// PCF2: GS/GSBackgroundWriter.h. A newer job replaces a queued one under the same key, one job runs
// at a time, Drain and the destructor wait for every job, and a durable write is seen whole or not
// at all by a concurrent reader.

#include "GS/GSBackgroundWriter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
	std::string TempDir(const char* name)
	{
		const std::filesystem::path dir =
			std::filesystem::temp_directory_path() / (std::string("pcf2-") + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		return dir.string();
	}

	std::vector<unsigned char> ReadAll(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		return std::vector<unsigned char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	// Holds the worker inside a job until Release().
	struct Gate
	{
		std::mutex m;
		std::condition_variable cv;
		bool entered = false, released = false;
		void Enter()
		{
			std::unique_lock<std::mutex> lock(m);
			entered = true;
			cv.notify_all();
			cv.wait(lock, [this] { return released; });
		}
		void WaitEntered()
		{
			std::unique_lock<std::mutex> lock(m);
			cv.wait(lock, [this] { return entered; });
		}
		void Release()
		{
			std::lock_guard<std::mutex> lock(m);
			released = true;
			cv.notify_all();
		}
	};
} // namespace

TEST(GSBackgroundWriter, NewerJobReplacesQueuedOneUnderSameKey)
{
	GSBackgroundWriter writer;
	Gate gate;
	std::vector<int> ran;
	std::mutex ran_mutex;
	auto job = [&](int id) { return [&, id] { std::lock_guard<std::mutex> l(ran_mutex); ran.push_back(id); }; };

	EXPECT_FALSE(writer.Submit("cache", [&] { gate.Enter(); job(0)(); }));
	gate.WaitEntered(); // job 0 is in flight, not queued
	EXPECT_FALSE(writer.Submit("cache", job(1)));
	EXPECT_TRUE(writer.Submit("cache", job(2)));
	EXPECT_TRUE(writer.Submit("cache", job(3)));
	EXPECT_FALSE(writer.Submit("selectors", job(10)));
	gate.Release();
	writer.Drain();
	EXPECT_EQ(ran, (std::vector<int>{0, 3, 10}));
}

TEST(GSBackgroundWriter, OneJobInFlight)
{
	GSBackgroundWriter writer;
	std::atomic<int> running{0}, max_running{0}, done{0};
	for (int i = 0; i < 64; i++)
	{
		writer.Submit("k" + std::to_string(i), [&] {
			const int now = ++running;
			int seen = max_running.load();
			while (now > seen && !max_running.compare_exchange_weak(seen, now)) {}
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			--running;
			++done;
		});
	}
	writer.Drain();
	EXPECT_EQ(done.load(), 64);
	EXPECT_EQ(max_running.load(), 1);
}

TEST(GSBackgroundWriter, DestructorDrainsQueuedWrites)
{
	const std::string dir = TempDir("dtor");
	const std::string path = dir + "/cache.bin";
	{
		GSBackgroundWriter writer;
		writer.Submit("a", [] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); });
		writer.Submit(path, [path] {
			const std::vector<unsigned char> data(1 << 20, 0x5a);
			ASSERT_TRUE(GSBackgroundWriter::WriteFileDurable(path, data.data(), data.size()));
		});
	}
	EXPECT_EQ(ReadAll(path), std::vector<unsigned char>(1 << 20, 0x5a));
	std::filesystem::remove_all(dir);
}

TEST(GSBackgroundWriter, DurableWriteRenamesOverOldFileAndLeavesNoTemp)
{
	const std::string dir = TempDir("rename");
	const std::string path = dir + "/cache.bin";
	const std::vector<unsigned char> a(1000, 1), b(3000, 2);
	ASSERT_TRUE(GSBackgroundWriter::WriteFileDurable(path, a.data(), a.size()));
	EXPECT_EQ(ReadAll(path), a);
	ASSERT_TRUE(GSBackgroundWriter::WriteFileDurable(path, b.data(), b.size()));
	EXPECT_EQ(ReadAll(path), b);
	size_t files = 0;
	for (const auto& entry : std::filesystem::directory_iterator(dir))
	{
		files++;
		EXPECT_EQ(entry.path().filename().string(), "cache.bin");
	}
	EXPECT_EQ(files, 1u);
	EXPECT_FALSE(GSBackgroundWriter::WriteFileDurable(dir + "/missing-dir/cache.bin", a.data(), a.size()));
	std::filesystem::remove_all(dir);
}

TEST(GSBackgroundWriter, ConcurrentReaderNeverSeesPartialFile)
{
	const std::string dir = TempDir("reader");
	const std::string path = dir + "/cache.bin";
	// Version v is (1 MiB + v * 4 KiB) bytes, all equal to v: a reader can tell a whole file from a
	// torn or mixed one.
	auto version = [](unsigned v) { return std::vector<unsigned char>((1u << 20) + v * 4096u, static_cast<unsigned char>(v)); };
	{
		const std::vector<unsigned char> v0 = version(0);
		ASSERT_TRUE(GSBackgroundWriter::WriteFileDurable(path, v0.data(), v0.size()));
	}

	std::atomic<bool> stop{false};
	std::atomic<int> reads{0}, bad{0};
	std::thread reader([&] {
		while (!stop.load())
		{
			const std::vector<unsigned char> got = ReadAll(path);
			const unsigned v = got.empty() ? 0xffffu : got[0];
			bool whole = !got.empty() && got.size() == (1u << 20) + v * 4096u;
			for (size_t i = 0; whole && i < got.size(); i += 4093)
				whole = got[i] == v;
			whole = whole && got.back() == v;
			if (!whole)
				bad++;
			reads++;
		}
	});

	GSBackgroundWriter writer;
	for (unsigned v = 1; v <= 60; v++)
	{
		writer.Submit(path, [path, data = version(v)] {
			ASSERT_TRUE(GSBackgroundWriter::WriteFileDurable(path, data.data(), data.size()));
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	writer.Drain();
	while (reads.load() < 20)
		std::this_thread::yield();
	stop = true;
	reader.join();
	EXPECT_EQ(bad.load(), 0);
	EXPECT_GE(reads.load(), 20);
	EXPECT_EQ(ReadAll(path), version(60)); // the last submitted snapshot always lands
	std::filesystem::remove_all(dir);
}
