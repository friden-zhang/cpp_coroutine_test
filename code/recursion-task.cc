#include <coroutine>
#include <iostream>
#include <optional>


struct PreviousAwaiter {
  
  bool await_ready() noexcept { return false; }

  
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) noexcept {
    if (previous && !previous.done()) {
      std::cout << "- [PreviousAwaiter] Climbing up: resuming previous coroutine." << std::endl;
      return previous;
    } else {
      std::cout << "- No previous coroutine to resume." << std::endl;
      return std::noop_coroutine();
    }
  }

  void await_resume() noexcept {}

  PreviousAwaiter(std::coroutine_handle<> prev) : previous(prev) {}

  std::coroutine_handle<> previous{std::noop_coroutine()};
};

// Forward declaration
struct Promise;


struct CalleeAwaiter {
  std::coroutine_handle<Promise> callee;  // The coroutine being called (deeper level)
  std::coroutine_handle<> caller;          // The coroutine doing the calling (current level)

  bool await_ready() { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> awaiting_coroutine);

  int await_resume();
};

struct Promise {

  auto initial_suspend() { return std::suspend_always{}; }
  
  auto final_suspend() noexcept { return PreviousAwaiter{previous}; }
  
  void unhandled_exception() { throw; }

  auto yield_value(int value) {
    std::cout << "- Yielded value: " << value << std::endl;
    _value = value;               // Store the yielded value
    return std::suspend_always{}; // Suspend after yielding
  }

  void return_value(int value) {
    std::cout << "- Returned value: " << value << std::endl;
    _value = value; // Store the final return value
  }

  std::coroutine_handle<Promise> get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  std::optional<int> get_value() { return _value; }

  std::optional<int> _value{};
  
  std::coroutine_handle<> previous{std::noop_coroutine()};
};

struct Task {
  using promise_type = Promise;

  std::coroutine_handle<promise_type> coroutine;

  Task(std::coroutine_handle<promise_type> handle) : coroutine(handle) {}

  Task& operator=(Task&& other) = delete;

  ~Task() {
    if (coroutine) {
      coroutine.destroy();
    }
  }

  std::optional<int> value() { return coroutine.promise().get_value(); }

  CalleeAwaiter operator co_await() { return CalleeAwaiter{coroutine, nullptr}; }
};


std::coroutine_handle<>
CalleeAwaiter::await_suspend(std::coroutine_handle<> awaiting_coroutine) {
  caller = awaiting_coroutine;
  
  // CRITICAL: Set up the upward return link
  // When callee completes, PreviousAwaiter will use this to climb back up
  callee.promise().previous = caller;
  
  std::cout << "- [CalleeAwaiter] Going deeper: suspending caller and starting callee."
            << std::endl;
  
  // Return callee handle to resume it (symmetric transfer)
  // This transfers control DOWN into the deeper level
  return callee;
}

int CalleeAwaiter::await_resume() {
  std::cout << "- [CalleeAwaiter] Climbing back: resuming caller with result from callee."
            << std::endl;
  auto val = callee.promise().get_value();
  return val.value_or(0); // Return the result from deeper level
}

// ==============================================================================
// factorial(): Example of a recursive coroutine
// ==============================================================================
// This function demonstrates how coroutines can call themselves recursively
// using co_await. Each recursive call creates a new coroutine that is properly
// linked via the 'previous' chain.
//
// Execution flow for factorial(5):
//
// DESCENT PHASE (CalleeAwaiter going down):
// 1. factorial(5) starts, calls co_await factorial(4)
//    - CalleeAwaiter suspends factorial(5), transfers DOWN to factorial(4)
//    - Sets factorial(4).previous = factorial(5)
// 2. factorial(4) calls co_await factorial(3)
//    - CalleeAwaiter suspends factorial(4), transfers DOWN to factorial(3)
//    - Sets factorial(3).previous = factorial(4)
// 3. ... continues going DOWN until factorial(1)
//
// BASE CASE (the deepest point):
// 4. factorial(1) returns 1 (no more recursion)
//    - PreviousAwaiter starts UPWARD traversal
//
// ASCENT PHASE (PreviousAwaiter climbing back up):
// 5. factorial(1) completes → PreviousAwaiter resumes factorial(2)
//    - CalleeAwaiter::await_resume() returns 1
// 6. factorial(2) gets result 1, returns 2*1=2
//    - PreviousAwaiter resumes factorial(3)
// 7. ... continues climbing UP the chain
// 8. factorial(5) gets result 24, returns 5*24=120
Task factorial(int n) {
  std::cout << "Calculating factorial(" << n << ")" << std::endl;
  
  // Base case: factorial(1) = 1
  if (n <= 1) {
    co_return 1;  // This triggers final_suspend() → PreviousAwaiter
  }

  // Recursive case: factorial(n) = n * factorial(n-1)
  Task sub_task = factorial(n - 1);  // Create the recursive coroutine
  int sub_result = co_await sub_task; // Go down via CalleeAwaiter
  //     ^^^^^^^^^ This value comes from CalleeAwaiter::await_resume()
  //               after PreviousAwaiter brings us back up
  
  int result = n * sub_result;
  std::cout << "factorial(" << n << ") = " << result << std::endl;
  co_return result;  // This triggers final_suspend() → PreviousAwaiter
}

// ==============================================================================
// main(): Demonstrates recursive coroutine execution
// ==============================================================================
int main() {
  std::cout << "=== Recursive Coroutine Example ===" << std::endl;

  // Create the top-level coroutine (factorial(5))
  // - This creates a suspended coroutine (initial_suspend returns suspend_always)
  // - The coroutine body has not started executing yet
  Task task = factorial(5);

  std::cout << "\nStarting coroutine..." << std::endl;
  
  // Resume the coroutine once
  // - This starts factorial(5), which will recursively create and execute
  //   factorial(4), factorial(3), factorial(2), factorial(1)
  // - CalleeAwaiter handles the DESCENT (going down to deeper levels)
  // - PreviousAwaiter handles the ASCENT (climbing back up with results)
  // - The chain of awaiters handles all the calls and returns automatically
  //   via symmetric transfer
  // - When this returns, the entire computation is complete
  task.coroutine.resume();

  // Retrieve the final result
  std::cout << "\nFinal result: " << *task.value() << std::endl;

  return 0;
}