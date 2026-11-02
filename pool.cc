// Many searches at once: the library's thread pool, ForEach() and
// SolveTables() (see solver.h). The solver's search caches are thread_local,
// so each pool thread searches with caches of its own, and the threads live
// as long as the library so the caches keep the size they grew to.
#include <string.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "solver.h"

namespace {

// Threads that live until their number changes. Run() hands every thread the
// same job and returns when all have finished it.
class Pool {
 public:
  explicit Pool(int n) {
    for (int i = 0; i < n; ++i) threads_.emplace_back([this] { Loop(); });
  }
  // Only called when the thread count changes, never at process exit: by then
  // the system has killed the threads, and joining them can hang (MinGW's
  // winpthreads waits forever).
  ~Pool() {
    Run(ReleaseCaches);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    start_.notify_all();
    for (auto& t : threads_) t.join();
  }
  int size() const { return (int)threads_.size(); }

  void Run(const std::function<void()>& job) {
    std::unique_lock<std::mutex> lock(mutex_);
    job_ = &job;
    running_ = (int)threads_.size();
    ++generation_;
    start_.notify_all();
    done_.wait(lock, [this] { return running_ == 0; });
    job_ = nullptr;
  }

 private:
  void Loop() {
    int seen = 0;
    for (;;) {
      const std::function<void()>* job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        start_.wait(lock, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
        job = job_;
      }
      (*job)();
      std::lock_guard<std::mutex> lock(mutex_);
      if (--running_ == 0) done_.notify_one();
    }
  }

  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable start_, done_;
  const std::function<void()>* job_ = nullptr;
  int generation_ = 0, running_ = 0;
  bool stop_ = false;
};

std::mutex run_mutex;     // one ForEach on the pool at a time
std::mutex config_mutex;  // guards max_threads and pool
Pool* pool = nullptr;     // never deleted at exit, see ~Pool
int max_threads = 0;      // 0: one per core

// The pool for `threads` threads, made or remade as needed. Call with
// run_mutex held.
Pool& GetPool(int threads) {
  std::lock_guard<std::mutex> lock(config_mutex);
  if (!pool || pool->size() != threads) {
    delete pool;
    pool = new Pool(threads);
  }
  return *pool;
}

// DDS's estimate of how hard a deal is to solve (Scheduler::Fanout): for each
// hand, its runs of touching ranks (KT982 is K, T98, 2) times one more than
// its voids, summed over the hands. Only the order it gives is used.
int Fanout(const Deal& deal) {
  static const char RANKS[] = "23456789TJQKA";
  int total = 0;
  for (const auto& hand : deal.hands) {
    int runs = 0, voids = 0;
    int prev = -2;
    bool any = false;
    for (size_t i = 0; i <= hand.size(); ++i) {
      char c = i < hand.size() ? hand[i] : ' ';
      if (c == ' ') {
        if (!any) ++voids;
        any = false;
        prev = -2;
        continue;
      }
      const char* r = strchr(RANKS, c);
      if (!r || !*r) continue;
      int rank = (int)(r - RANKS);
      if (rank != prev - 1) ++runs;
      prev = rank;
      any = true;
    }
    total += runs * (1 + voids);
  }
  return total;
}

}  // namespace

int Cores() {
  int n = (int)std::thread::hardware_concurrency();
  return n > 0 ? n : 1;
}

void SetThreads(int threads) {
  std::lock_guard<std::mutex> lock(config_mutex);
  max_threads = threads <= 0 ? 0 : std::min(threads, Cores());
}

int Threads() {
  std::lock_guard<std::mutex> lock(config_mutex);
  return max_threads > 0 ? max_threads : Cores();
}

void ForEach(int n, const std::function<void(int)>& work) {
  if (n <= 0) return;
  int threads = Threads();
  if (threads == 1 || n == 1) {
    for (int i = 0; i < n; ++i) work(i);
    return;
  }
  std::lock_guard<std::mutex> run(run_mutex);
  std::atomic<int> next{0};
  GetPool(threads).Run([&] {
    for (int i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) work(i);
  });
}

void SolveTables(
    const std::vector<Deal>& deals, const std::vector<int>& trumps,
    const std::function<void(int deal, int trump, int lead_seat, int ns_tricks)>& seat_done) {
  std::vector<int> order(deals.size()), fanout(deals.size());
  for (size_t i = 0; i < deals.size(); ++i) {
    order[i] = (int)i;
    fanout[i] = Fanout(deals[i]);
  }
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return fanout[a] > fanout[b]; });
  struct Item {
    int deal, trump;
  };
  std::vector<Item> items;
  for (int d : order)
    for (int trump : trumps) items.push_back({d, trump});
  ForEach((int)items.size(), [&](int k) {
    int d = items[k].deal;
    Solve(
        deals[d], {items[k].trump}, {WEST, EAST, NORTH, SOUTH}, [](int) {},
        [&](int trump, int lead_seat, int ns_tricks) { seat_done(d, trump, lead_seat, ns_tricks); },
        [](int) {});
  });
}

void ReleaseAllCaches() {
  ReleaseCaches();
  std::lock_guard<std::mutex> run(run_mutex);
  std::lock_guard<std::mutex> lock(config_mutex);
  if (pool) pool->Run(ReleaseCaches);
}
