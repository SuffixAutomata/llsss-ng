#include "indexed_executor.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace rlife::llsss {
namespace {

// Reification performs hundreds of dependent level passes. Entering a new
// OpenMP parallel region for every level leaves the workers spinning at a
// succession of short barriers. Keep one ordinary C++ worker team for the
// lifetime of the process and dispatch the same coarse indexed callbacks
// through it instead. Calls are intentionally serialized because solver
// phases are dependent.
class IndexedTaskPool {
public:
  IndexedTaskPool() = default;
  IndexedTaskPool(const IndexedTaskPool&) = delete;
  IndexedTaskPool& operator=(const IndexedTaskPool&) = delete;

  ~IndexedTaskPool() { stop_workers(); }

  void execute(std::size_t task_count, int requested_workers, void* context, IndexedTaskFunction function) {
    if(requested_workers <= 0)
      throw std::invalid_argument("indexed executor needs at least one worker");
    if(task_count == 0)
      return;

    std::unique_lock dispatch(dispatch_mutex_);
    configure(static_cast<std::size_t>(requested_workers));
    {
      std::lock_guard lock(state_mutex_);
      task_count_ = task_count;
      context_ = context;
      function_ = function;
      next_task_.store(0, std::memory_order_relaxed);
      failed_.store(false, std::memory_order_relaxed);
      failure_ = nullptr;
      remaining_workers_ = workers_.size();
      ++generation_;
    }
    work_cv_.notify_all();

    run_tasks(0);
    {
      std::unique_lock lock(state_mutex_);
      done_cv_.wait(lock, [&] { return remaining_workers_ == 0; });
    }
    if(failure_)
      std::rethrow_exception(failure_);
  }

private:
  void configure(std::size_t requested_workers) {
    if(configured_workers_ == requested_workers)
      return;
    stop_workers();
    {
      std::lock_guard lock(state_mutex_);
      stopping_ = false;
      configured_workers_ = requested_workers;
    }
    workers_.reserve(requested_workers - 1U);
    for(std::size_t worker = 1; worker < requested_workers; ++worker)
      workers_.emplace_back([this, worker] { worker_loop(worker); });
  }

  void stop_workers() noexcept {
    {
      std::lock_guard lock(state_mutex_);
      stopping_ = true;
      ++generation_;
    }
    work_cv_.notify_all();
    for(auto& worker : workers_) {
      if(worker.joinable())
        worker.join();
    }
    workers_.clear();
    configured_workers_ = 0;
  }

  void worker_loop(std::size_t worker) noexcept {
    std::uint64_t seen_generation = 0;
    for(;;) {
      {
        std::unique_lock lock(state_mutex_);
        work_cv_.wait(lock, [&] { return stopping_ || generation_ != seen_generation; });
        if(stopping_)
          return;
        seen_generation = generation_;
      }
      run_tasks(worker);
      {
        std::lock_guard lock(state_mutex_);
        if(remaining_workers_ != 0)
          --remaining_workers_;
        if(remaining_workers_ == 0)
          done_cv_.notify_one();
      }
    }
  }

  void run_tasks(std::size_t worker) noexcept {
    for(;;) {
      const auto task = next_task_.fetch_add(1, std::memory_order_relaxed);
      if(task >= task_count_)
        return;
      if(failed_.load(std::memory_order_relaxed))
        continue;
      try {
        function_(context_, task, worker);
      } catch(...) {
        bool expected = false;
        if(failed_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
          std::lock_guard lock(failure_mutex_);
          failure_ = std::current_exception();
        }
      }
    }
  }

  std::mutex dispatch_mutex_;
  std::mutex state_mutex_;
  std::mutex failure_mutex_;
  std::condition_variable work_cv_;
  std::condition_variable done_cv_;
  std::vector<std::thread> workers_;
  std::atomic<std::size_t> next_task_ = 0;
  std::atomic<bool> failed_ = false;
  std::size_t configured_workers_ = 0;
  std::size_t task_count_ = 0;
  std::size_t remaining_workers_ = 0;
  std::uint64_t generation_ = 0;
  void* context_ = nullptr;
  IndexedTaskFunction function_ = nullptr;
  std::exception_ptr failure_;
  bool stopping_ = false;
};

} // namespace

void execute_indexed_tasks(std::size_t task_count, int requested_workers, void* context, IndexedTaskFunction function) {
  static IndexedTaskPool pool;
  pool.execute(task_count, requested_workers, context, function);
}

} // namespace rlife::llsss
