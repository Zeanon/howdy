#ifndef OPTIONAL_TASK_H_
#define OPTIONAL_TASK_H_

#include <cassert>
#include <chrono>
#include <future>
#include <thread>

// A task executed only if activated.
template <typename T> class optional_task {
  std::thread thread;
  std::packaged_task<T()> task;
  std::future<T> future;
  bool spawned{false};
  bool is_active{false};

public:
  explicit optional_task(std::function<T()> func);

  void activate();

  template <typename R, typename P>
  auto wait(std::chrono::duration<R, P> dur) -> std::future_status;

  auto get() -> T;

  void stop(bool force);

  bool active() const;
  bool spawned_thread() const;

  ~optional_task();
};

template <typename T>
optional_task<T>::optional_task(std::function<T()> func)
    : task(std::packaged_task<T()>(std::move(func))),
      future(task.get_future()) {}

// Create a new thread and launch the task on it.
template <typename T> void optional_task<T>::activate() {
  assert(!spawned);

  thread = std::thread(std::move(task));
  spawned = true;
  is_active = true;
}

// Wait for `dur` time and return a `future` status.
template <typename T>
template <typename R, typename P>
auto optional_task<T>::wait(std::chrono::duration<R, P> dur)
    -> std::future_status {
  return future.wait_for(dur);
}

// Get the value.
// WARNING: The function should be run only if the task has successfully been
// stopped.
template <typename T> auto optional_task<T>::get() -> T {
  assert(!is_active && spawned);
  return future.get();
}

template <typename T> bool optional_task<T>::active() const {
  return is_active;
}

template <typename T> bool optional_task<T>::spawned_thread() const {
  return spawned;
}

// Stop the thread:
// - if `force` is `false`, by joining the thread.
// - if `force` is `true`, by cancelling the thread using `pthread_cancel`.
//
// WARNING: pthread_cancel() must only be used for tasks which are explicitly
// written to be cancellation-safe.
template <typename T> void optional_task<T>::stop(bool force) {
  if (!spawned || !thread.joinable()) {
    is_active = false;
    return;
  }

  if (force) {
    auto native_hd = thread.native_handle();
    pthread_cancel(native_hd);
  }

  thread.join();
  is_active = false;
}

template <typename T> optional_task<T>::~optional_task() {
  if (is_active && spawned) {
    stop(false);
  }
}

#endif // OPTIONAL_TASK_H_
