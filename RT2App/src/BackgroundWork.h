#pragma once

#ifndef BACKGROUND_WORK_H
#define BACKGROUND_WORK_H

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <mutex>
#include <memory>

// BackgroundWork — minimal thread-safe wrapper for running CPU-intensive
// work on a background thread with status reporting and completion polling.
//
// Usage:
//   auto work = std::make_unique<BackgroundWork>();
//   work->Run("Loading scene...", [](BackgroundWork& self) {
//       self.SetStatus("Parsing file...");
//       // ... heavy CPU work ...
//       self.SetStatus("Decoding textures...");
//       // ... more work ...
//       return true; // success
//   });
//   // Main thread polls each frame:
//   while (work->IsBusy()) {
//       ImGui::Text("%s", work->GetStatus());
//   }
//   bool success = work->GetResult();
//
// The work function runs on a dedicated std::thread. SetStatus is
// thread-safe (mutex-protected). GetResult is only valid after IsBusy()
// returns false. The thread is joined on destruction, so the work MUST
// eventually complete (no infinite loops).
//
// Only ONE BackgroundWork should be active at a time — the UI modal is
// modal and blocks other load operations. This is enforced by the host
// (WalnutApp) checking m_BackgroundWork before starting new work.
//
class BackgroundWork
{
public:
	BackgroundWork() = default;
	~BackgroundWork() { Join(); }

	BackgroundWork(const BackgroundWork&) = delete;
	BackgroundWork& operator=(const BackgroundWork&) = delete;
	BackgroundWork(BackgroundWork&&) = delete;
	BackgroundWork& operator=(BackgroundWork&&) = delete;

	// Launch a work function on a background thread. The function receives
	// a reference to this BackgroundWork so it can call SetStatus. Returns
	// immediately. Must not be called if already running.
	using WorkFn = std::function<bool(BackgroundWork&)>;

	void Run(const std::string& initialStatus, WorkFn fn)
	{
		m_Result.store(false);
		m_Done.store(false);
		m_Started.store(true);
		SetStatus(initialStatus);
		m_Thread = std::thread([this, fn = std::move(fn)]() {
			bool ok = false;
			try
			{
				ok = fn(*this);
			}
			catch (...)
			{
				ok = false;
				SetStatus("Exception in background work");
			}
			m_Result.store(ok);
			m_Done.store(true);
		});
	}

	// True while the worker thread is running.
	bool IsBusy() const { return m_Started.load() && !m_Done.load(); }
	bool IsDone() const { return m_Done.load(); }
	bool GetResult() const { return m_Result.load(); }

	// Thread-safe status string (updated by the worker, read by the UI).
	void SetStatus(const std::string& s)
	{
		std::lock_guard<std::mutex> lock(m_StatusMutex);
		m_Status = s;
	}
	std::string GetStatus() const
	{
		std::lock_guard<std::mutex> lock(m_StatusMutex);
		return m_Status;
	}

	// Join the thread if done (safe to call from the main thread's poll
	// loop). Returns true if the thread was joined (work complete).
	bool JoinIfDone()
	{
		if (!m_Started.load() || !m_Done.load()) return false;
		Join();
		return true;
	}

	// Force-join (blocks until the worker finishes). Called by destructor.
	void Join()
	{
		if (m_Thread.joinable())
			m_Thread.join();
		m_Started.store(false);
	}

private:
	std::thread m_Thread;
	std::atomic<bool> m_Started{ false };
	std::atomic<bool> m_Done{ false };
	std::atomic<bool> m_Result{ false };
	mutable std::mutex m_StatusMutex;
	std::string m_Status;
};

#endif // BACKGROUND_WORK_H