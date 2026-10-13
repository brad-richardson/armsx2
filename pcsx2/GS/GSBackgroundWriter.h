// SPDX-FileCopyrightText: 2026 ARMSX2 Contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// PCF2: cache file writes off the GS thread.
//
// The caller takes an in-memory snapshot on its own thread and hands a job to one worker thread.
// Jobs are keyed (one key per file): a job submitted while an older one with the same key is still
// queued replaces it, so at most one write per file is in flight and one more is queued. Drain()
// waits for every queued and in-flight job; the destructor drains and joins, so a process that
// shuts down through it never leaves a half-written cache.
//
// WriteFileDurable() is the write a job normally runs: temporary file, fsync, rename over the old
// one. A reader of `path` sees the old file or the new one, never part of either.
//
// Header-only and std-only, so it builds into every GS host and a standalone test.

#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

class GSBackgroundWriter
{
public:
	using Job = std::function<void()>;

	GSBackgroundWriter() = default;
	GSBackgroundWriter(const GSBackgroundWriter&) = delete;
	GSBackgroundWriter& operator=(const GSBackgroundWriter&) = delete;

	~GSBackgroundWriter()
	{
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_idle.wait(lock, [this] { return m_queue.empty() && !m_busy; });
			m_stop = true;
		}
		m_wake.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	/// Queues `job` under `key`. Returns true if it replaced a queued, not yet started job.
	bool Submit(const std::string& key, Job job)
	{
		bool replaced = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& queued : m_queue)
			{
				if (queued.first == key)
				{
					queued.second = std::move(job);
					replaced = true;
					break;
				}
			}
			if (!replaced)
				m_queue.emplace_back(key, std::move(job));
			if (!m_thread.joinable())
				m_thread = std::thread([this] { Run(); });
		}
		m_wake.notify_one();
		return replaced;
	}

	/// Blocks until every job submitted before the call has finished.
	void Drain()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_idle.wait(lock, [this] { return m_queue.empty() && !m_busy; });
	}

	/// Writes `size` bytes to `path` through `path.tmp<pid>`, fsyncs it, then renames it over `path`.
	static bool WriteFileDurable(const std::string& path, const void* data, size_t size)
	{
#ifdef _WIN32
		const std::string tmp = path + ".tmp" + std::to_string(_getpid());
#else
		const std::string tmp = path + ".tmp" + std::to_string(getpid());
#endif
		std::FILE* f = std::fopen(tmp.c_str(), "wb");
		if (!f)
			return false;
		bool ok = (size == 0 || std::fwrite(data, 1, size, f) == size) && std::fflush(f) == 0;
#ifdef _WIN32
		ok = ok && _commit(_fileno(f)) == 0;
#else
		ok = ok && fsync(fileno(f)) == 0;
#endif
		ok = (std::fclose(f) == 0) && ok;
#ifdef _WIN32
		// rename() does not replace on Windows.
		if (ok)
			std::remove(path.c_str());
#endif
		if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0)
		{
			std::remove(tmp.c_str());
			return false;
		}
		return true;
	}

private:
	void Run()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		for (;;)
		{
			m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
			if (m_queue.empty())
				return; // m_stop, nothing left
			Job job = std::move(m_queue.front().second);
			m_queue.erase(m_queue.begin());
			m_busy = true;
			lock.unlock();
			job();
			lock.lock();
			m_busy = false;
			if (m_queue.empty())
				m_idle.notify_all();
		}
	}

	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::condition_variable m_idle;
	std::vector<std::pair<std::string, Job>> m_queue;
	bool m_busy = false;
	bool m_stop = false;
	std::thread m_thread;
};
