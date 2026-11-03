// The console's threads (libctru's threadCreate and the rest, as tools/test/native/stub/3ds.h declares them for
// MW3DS_ANDROID) on the system's: the streaming thread reads the next exterior cells while the game plays
// (mwworld/scene.cpp), on another of the watch's cores, where the PC build of the tests reads them on the spot
#include <3ds.h>
#include <condition_variable>
#include <sys/resource.h>
#include <unistd.h>

namespace
{
struct AndroidThread
{
	std::thread thread;
	ThreadFunc entry;
	void* arg;
	int nice;
	std::mutex lock;
	std::condition_variable doneSignal;
	bool done = false;
};

thread_local AndroidThread* t_current = nullptr;        // (the main thread's: null, as libctru's)

void run(AndroidThread* t)
{
	t_current = t;
	// Below the game's thread, as the console's priority under the main thread's: a watch has few cores
	setpriority(PRIO_PROCESS, gettid(), t->nice);
	t->entry(t->arg);
	std::lock_guard<std::mutex> hold(t->lock);
	t->done = true;
	t->doneSignal.notify_all();
}
}

Thread threadCreate(ThreadFunc entry, void* arg, size_t, int prio, int, bool detached)
{
	if (detached)
		return nullptr;                 // (never asked for: a detached thread would free itself)
	AndroidThread* t = new AndroidThread();
	t->entry = entry;
	t->arg = arg;
	t->nice = prio > 0x30 ? 5 : 0;      // (0x30 the main thread's: one lower is the streaming thread's)
	t->thread = std::thread(run, t);
	return t;
}

Thread threadGetCurrent()
{
	return t_current;
}

// 0 once the thread has finished, within timeoutNs (U64_MAX: however long); otherwise a timeout, not an error, as libctru's
Result threadJoin(Thread th, u64 timeoutNs)
{
	AndroidThread* t = (AndroidThread*)th;
	if (!t)
		return 0;
	std::unique_lock<std::mutex> hold(t->lock);
	if (timeoutNs == U64_MAX)
		t->doneSignal.wait(hold, [t] { return t->done; });
	else if (!t->doneSignal.wait_for(hold, std::chrono::nanoseconds((long long)timeoutNs), [t] { return t->done; }))
		return 0x09401BFE;              // (the kernel's timeout, which R_FAILED doesn't call a failure)
	return 0;
}

void threadFree(Thread th)
{
	AndroidThread* t = (AndroidThread*)th;
	if (!t)
		return;
	if (t->thread.joinable())
		t->thread.join();
	delete t;
}
