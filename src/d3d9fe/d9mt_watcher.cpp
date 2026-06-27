// d9mt: Metal backend — completion watcher (liveness backbone).
//
// One background thread per process waits on submitted MTLCommandBuffers in
// FIFO order and runs registered callbacks when they retire. This implements
// the "completion handler" role of BACKEND-SURFACE §5.1 on top of winemetal,
// which only exposes MTLCommandBuffer_waitUntilCompleted/_status (no generic
// callback export). All three deadlock-critical signals (presenter frame,
// submission fence, staging fence) and resource track-release run through
// here.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <queue>
#include <thread>
#include <utility>

#include "d9mt_backend.h"

#include "../../vendor/dxvk/src/util/thread.h"
#include "../../vendor/dxvk/src/util/util_env.h"
#include "../../vendor/dxvk/src/util/util_time.h"
#include "../../vendor/dxvk/src/util/log/log.h"

namespace dxvk::d9mt {

  namespace {

    // GPU watchdog timeout (ns). A submitted command buffer that doesn't reach
    // .completed within this window is treated as a hung GPU -> device-lost.
    // Override with D9MT_GPU_TIMEOUT_MS (0/invalid -> default 5000 ms).
    int64_t gpuTimeoutNs() {
      static const int64_t ns = [] {
        long ms = 5000;
        if (const char* v = std::getenv("D9MT_GPU_TIMEOUT_MS")) {
          char* end = nullptr;
          long parsed = std::strtol(v, &end, 10);
          if (end != v && parsed > 0)
            ms = parsed;
        }
        return int64_t(ms) * 1000000;
      }();
      return ns;
    }

    struct WatchEntry {
      obj_handle_t          cmdbuf = 0;
      std::function<void()> callback;
    };

    class CompletionWatcher {

    public:

      CompletionWatcher()
      : m_thread([this] { run(); }) {
      }

      // Never destroyed (allocated with new, intentionally leaked): joining
      // a thread from a static destructor during PE process teardown under
      // Wine is a known hang source. waitIdle() is the orderly drain.
      ~CompletionWatcher() = delete;

      void watch(obj_handle_t cmdbuf, std::function<void()>&& callback) {
        if (cmdbuf)
          NSObject_retain(cmdbuf);

        std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_queue.push(WatchEntry { cmdbuf, std::move(callback) });
        m_workCond.notify_one();
      }

      void waitIdle() {
        std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_idleCond.wait(lock, [this] {
          return m_queue.empty() && !m_busy;
        });
      }

      // Signal the worker to exit its loop and return. The thread is detached /
      // never joined (joining during Wine PE teardown is a known hang); making
      // run() return just lets the thread terminate on its own. Called from
      // DxvkInstance::~DxvkInstance BEFORE wsi::quit() hands off to the macOS
      // app-termination handshake, so the idle watcher — otherwise parked in a
      // Wine syscall — no longer interlocks with -[NSApplication
      // _shouldTerminate] and wedges process exit.
      void stop() {
        {
          std::unique_lock<dxvk::mutex> lock(m_mutex);
          m_stop = true;
        }
        m_workCond.notify_all();
        m_idleCond.notify_all();
      }

    private:

      dxvk::mutex              m_mutex;
      dxvk::condition_variable m_workCond;
      dxvk::condition_variable m_idleCond;

      std::queue<WatchEntry>   m_queue;
      bool                     m_busy = false;
      bool                     m_stop = false;

      dxvk::thread             m_thread;

      void run() {
        env::setThreadName("d9mt-watcher");

        std::unique_lock<dxvk::mutex> lock(m_mutex);

        while (true) {
          m_workCond.wait(lock, [this] {
            return !m_queue.empty() || m_stop;
          });

          if (m_stop) {
            m_idleCond.notify_all();
            return;
          }

          WatchEntry entry = std::move(m_queue.front());
          m_queue.pop();
          m_busy = true;

          lock.unlock();

          if (entry.cmdbuf) {
            if (isDeviceLost()) {
              // Device already lost (an earlier frame timed out): fail open —
              // don't wait on the dead GPU, retire immediately so nothing
              // blocks and the process stays responsive / cleanly quittable.
            } else {
              // Bounded completion wait (was an infinite
              // MTLCommandBuffer_waitUntilCompleted). winemetal exposes no
              // timeout-block for command buffers, but Metal updates
              // MTLCommandBuffer.status asynchronously as the GPU progresses,
              // so poll it with a hard deadline. A frame that never completes
              // (GPU/driver hang — e.g. the Apple Metal shader-compiler lockup)
              // thus becomes a recoverable device-loss instead of an unkillable
              // freeze: the registered callbacks (submission/staging/present
              // fences) still get to run below, unblocking the spin-waiters in
              // DxvkDevice::waitForSubmission/waitForResource. Sleep (not
              // yield-spin) between polls so the watcher stays ~0 CPU; tight
              // first, backing off for the rare slow/hung case.
              const int64_t timeoutNs = gpuTimeoutNs();
              const auto    t0        = dxvk::high_resolution_clock::now();
              bool          timedOut  = false;

              for (uint32_t i = 0; ; i++) {
                WMTCommandBufferStatus st = MTLCommandBuffer_status(entry.cmdbuf);
                if (st == WMTCommandBufferStatusCompleted
                 || st == WMTCommandBufferStatusError)
                  break;

                int64_t dtNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  dxvk::high_resolution_clock::now() - t0).count();
                if (dtNs >= timeoutNs) {
                  timedOut = true;
                  break;
                }

                std::this_thread::sleep_for(
                  std::chrono::microseconds(i < 512 ? 100 : 1000));
              }

              if (timedOut) {
                Logger::err("d9mt: command buffer wait TIMED OUT — GPU appears "
                  "hung; entering device-lost recovery (frames fail open, "
                  "process stays responsive). Tune via D9MT_GPU_TIMEOUT_MS.");
                setDeviceLost();
              } else if (MTLCommandBuffer_status(entry.cmdbuf)
                       == WMTCommandBufferStatusError) {
                Logger::err("d9mt: command buffer completed with error");
                logNSError("d9mt: MTLCommandBuffer error",
                  MTLCommandBuffer_error(entry.cmdbuf));
              }
            }
          }

          if (entry.callback)
            entry.callback();

          if (entry.cmdbuf)
            NSObject_release(entry.cmdbuf);

          lock.lock();
          m_busy = false;

          if (m_queue.empty())
            m_idleCond.notify_all();
        }
      }

    };

    CompletionWatcher* g_watcher = nullptr;

    CompletionWatcher& watcher() {
      // intentionally leaked, see ~CompletionWatcher comment
      static CompletionWatcher* s_watcher = new CompletionWatcher();
      g_watcher = s_watcher;
      return *s_watcher;
    }

  } // anonymous namespace


  // Sticky device-lost flag. Set by the watcher when a command buffer blows the
  // GPU watchdog timeout (or by any backend path that detects an unrecoverable
  // Metal error). Once set, the watcher fails open and the presenter reports
  // VK_ERROR_DEVICE_LOST — turning an unkillable freeze into a responsive,
  // cleanly-quittable device-loss. Not cleared in-process (a hung Apple Metal
  // device needs a restart); Phase B aims to stop hitting it.
  std::atomic<bool> g_deviceLost = { false };

  void setDeviceLost() { g_deviceLost.store(true, std::memory_order_release); }
  bool isDeviceLost()  { return g_deviceLost.load(std::memory_order_acquire); }


  void watchCommandBuffer(obj_handle_t cmdbuf, std::function<void()> callback) {
    watcher().watch(cmdbuf, std::move(callback));
  }


  void watcherWaitIdle() {
    watcher().waitIdle();
  }

  // Stop the watcher thread so run() returns and the thread terminates (no
  // join). Called at backend teardown (DxvkInstance::~DxvkInstance) before
  // wsi::quit(), so the thread is gone before the macOS termination handshake.
  // No-op if the watcher was never created.
  void watcherStop() {
    if (g_watcher)
      g_watcher->stop();
  }

}
