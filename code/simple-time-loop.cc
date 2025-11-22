#include <chrono>
#include <coroutine>
#include <deque>
#include <exception>
#include <iostream>
#include <queue>
#include <span>
#include <thread>

using namespace std::chrono_literals;

template <class A>
concept Awaiter = requires(A a, std::coroutine_handle<> h) {
  { a.await_ready() };
  { a.await_suspend(h) };
  { a.await_resume() };
};

template <class A>
concept Awaitable = Awaiter<A> || requires(A a) {
  { a.operator co_await() } -> Awaiter;
};

struct PreviousAwaiter {
  // await_ready(): Always return false to ensure suspension
  // - This allows await_suspend() to be called to resume the caller
  // - Must be noexcept because it's used in final_suspend()
  bool await_ready() noexcept { return false; }

  // await_suspend(): Called when the callee coroutine completes
  // - Returns the caller's coroutine handle to resume it (symmetric transfer)
  // - This is the "return" mechanism - going back UP the call chain
  // - Must be noexcept because it's used in final_suspend()
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> coroutine) noexcept {
    if (previous && !previous.done()) {
      std::cout
          << "- [PreviousAwaiter] Climbing up: resuming previous coroutine."
          << std::endl;
      // Return the caller's handle - this resumes the caller coroutine
      // The 'previous' was set by TaskAwaiter when the caller did co_await
      return previous;
    } else {
      std::cout << "- No previous coroutine to resume." << std::endl;
      // No caller to return to (we're at the top level)
      return std::noop_coroutine();
    }
  }

  // await_resume(): Called when resuming, but does nothing for PreviousAwaiter
  // - The caller resumes at its suspension point, not here
  // - Must be noexcept because it's used in final_suspend()
  void await_resume() noexcept {}

  // Constructor: Stores the caller's coroutine handle
  PreviousAwaiter(std::coroutine_handle<> prev) : previous(prev) {}

  // previous: The coroutine handle of the caller (who is waiting for us)
  // - Set by TaskAwaiter::await_suspend() when caller does co_await
  // - Used to resume the caller when this coroutine completes
  std::coroutine_handle<> previous{std::noop_coroutine()};
};

template <typename T> struct Promise {

  // initial_suspend(): Always suspend at the start
  auto initial_suspend() { return std::suspend_always{}; }

  // final_suspend(): Use PreviousAwaiter to automatically resume caller
  auto final_suspend() noexcept { return PreviousAwaiter{previous}; }

  // unhandled_exception(): Rethrow any unhandled exceptions
  void unhandled_exception() { exception = std::current_exception(); }

  void return_value(T val) { value = val; }

  // get_return_object(): Creates the Task object for this coroutine
  std::coroutine_handle<Promise> get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  // result retrieval
  std::optional<T> result() {
    if (exception) {
      std::rethrow_exception(exception);
    }
    return value;
  }

  std::coroutine_handle<> previous{std::noop_coroutine()};
  std::exception_ptr exception{nullptr};
  std::optional<T> value{std::nullopt};
  // Disable copying and moving
  Promise &operator=(Promise &&) = delete;
};

// void return type
template <> struct Promise<void> {

  // initial_suspend(): Always suspend at the start
  auto initial_suspend() { return std::suspend_always{}; }

  // final_suspend(): Use PreviousAwaiter to automatically resume caller
  auto final_suspend() noexcept { return PreviousAwaiter{previous}; }

  // unhandled_exception(): Rethrow any unhandled exceptions
  void unhandled_exception() { exception = std::current_exception(); }

  void return_void() {
    if (exception) {
      std::rethrow_exception(exception);
    }
  }

  // result retrieval
  void result() {}

  // get_return_object(): Creates the Task object for this coroutine
  std::coroutine_handle<Promise> get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  std::coroutine_handle<> previous{std::noop_coroutine()};
  std::exception_ptr exception{nullptr};

  // Disable copying and moving
  Promise &operator=(Promise &&) = delete;
};

template <typename T = void> struct Task {
  using promise_type = Promise<T>;

  Task(std::coroutine_handle<promise_type> handle) : coroutine(handle) {}

  ~Task() {
    if (coroutine) {
      coroutine.destroy();
    }
  }

  struct Awaiter {
    bool await_ready() noexcept { return false; }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> caller) noexcept {
      // Set the caller as the previous coroutine in the callee's promise
      coroutine.promise().previous = caller;
      return coroutine;
    }

    T await_resume() {
      if constexpr (std::is_void_v<T>) {
        coroutine.promise().result();
      } else {
        return *(coroutine.promise().result());
      }
    }

    std::coroutine_handle<promise_type> coroutine;
  };

  std::coroutine_handle<> coroutine;

  // Disable copying to prevent double-destruction
  Task &operator=(const Task &&) = delete;
};


struct Loop {

  Loop() = default;

  struct TimerEntry {
    std::chrono::steady_clock::time_point expire_time;
    std::coroutine_handle<> handle;

    bool operator>(const TimerEntry &other) const {
      return expire_time > other.expire_time;
    }
  };

  std::queue<std::coroutine_handle<>> ready_tasks;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>> timers;

  void add_task(std::coroutine_handle<> handle) {
    ready_tasks.push(handle);
  }

  void add_timer(std::chrono::steady_clock::time_point time,
                 std::coroutine_handle<> handle) {
    timers.push(TimerEntry{time, handle});
  }
};

Loop& get_global_loop() {
  static Loop global_loop;
  return global_loop;
}


