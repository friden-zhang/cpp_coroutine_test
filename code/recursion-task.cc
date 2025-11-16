#include <coroutine>
#include <iostream>
#include <optional>

// ==============================================================================
// PreviousAwaiter: Handles returning from callee to caller coroutine
// ==============================================================================
// This awaiter is used in final_suspend() to automatically resume the caller
// coroutine when the callee coroutine completes. It implements the "return"
// part of the coroutine call mechanism.
//
// Execution flow:
//   factorial(5) calls factorial(4)
//        ↓ (TaskAwaiter suspends caller, starts callee)
//   factorial(4) executes and completes
//        ↓ (PreviousAwaiter resumes caller automatically)
//   factorial(5) continues execution
struct PreviousAwaiter {
  // await_ready(): Always return false to ensure suspension
  // - This allows await_suspend() to be called to resume the caller
  // - Must be noexcept because it's used in final_suspend()
  bool await_ready() noexcept { return false; }

  // await_suspend(): Called when the callee coroutine completes
  // - Returns the caller's coroutine handle to resume it (symmetric transfer)
  // - This is the "return" mechanism - going back UP the call chain
  // - Must be noexcept because it's used in final_suspend()
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) noexcept {
    if (previous && !previous.done()) {
      std::cout << "- [PreviousAwaiter] Climbing up: resuming previous coroutine." << std::endl;
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

// Forward declaration
struct Promise;

// ==============================================================================
// CalleeAwaiter: Handles calling down into a callee coroutine (the "call")
// ==============================================================================
// This awaiter implements the downward traversal in recursive coroutine calls.
// When you write `co_await factorial(n-1)`, this awaiter:
// 1. Suspends the caller (current coroutine)
// 2. Sets up the return path (callee.previous = caller)
// 3. Transfers control DOWN to the callee (deeper into recursion)
//
// Think of it as "going down the rabbit hole" - each co_await goes one level
// deeper until hitting the base case.
//
// Relationship with PreviousAwaiter:
// - CalleeAwaiter: Downward traversal (caller → callee, going deeper)
//   Example: factorial(5) → factorial(4) → factorial(3) → ... → factorial(1)
// - PreviousAwaiter: Upward traversal (callee → caller, coming back)
//   Example: factorial(1) → factorial(2) → factorial(3) → ... → factorial(5)
//
// Together they form the complete recursion cycle:
//   factorial(5) ─[CalleeAwaiter]→ factorial(4) ─[CalleeAwaiter]→ ... → factorial(1)
//        ↑                                                                      ↓
//        └─────────────────[PreviousAwaiter]─────────────────────────────────┘
struct CalleeAwaiter {
  std::coroutine_handle<Promise> callee;  // The coroutine being called (deeper level)
  std::coroutine_handle<> caller;          // The coroutine doing the calling (current level)

  // await_ready(): Always suspend to perform the downward call
  // - We need to suspend to transfer control to the deeper coroutine
  bool await_ready() { return false; }

  // await_suspend(): The downward "call" mechanism
  // - Suspends the current coroutine (caller)
  // - Sets up the upward return path (callee.previous = caller)
  // - Transfers control DOWN to the callee (symmetric transfer)
  // - This is the "descent" phase of recursion
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> awaiting_coroutine);

  // await_resume(): Called when climbing back up (after PreviousAwaiter returns)
  // - This is where we retrieve the result from the deeper level
  // - The return value becomes the result of the co_await expression
  // - Example: int result = co_await factorial(4);
  //            ^^^^^^ this value comes from await_resume()
  // - This happens during the "ascent" phase after hitting the base case
  int await_resume();
};

// ==============================================================================
// Promise: The promise type for recursive coroutines
// ==============================================================================
// This promise supports recursive coroutine calls by maintaining a 'previous'
// link to the caller coroutine. This creates a call chain that allows proper
// return flow when coroutines complete.
struct Promise {

  // initial_suspend(): Start in suspended state (lazy execution)
  // - The coroutine won't run until explicitly resumed
  auto initial_suspend() { return std::suspend_always{}; }
  
  // final_suspend(): Use PreviousAwaiter to automatically resume caller
  // - When this coroutine completes, PreviousAwaiter will resume 'previous'
  // - This is how the return flow works in recursive calls
  // - Must be noexcept (C++ coroutine requirement)
  auto final_suspend() noexcept { return PreviousAwaiter{previous}; }
  
  // unhandled_exception(): Rethrow any unhandled exceptions
  void unhandled_exception() { throw; }

  // yield_value(): Called when co_yield is used
  // - Stores the yielded value and suspends
  auto yield_value(int value) {
    std::cout << "- Yielded value: " << value << std::endl;
    _value = value;               // Store the yielded value
    return std::suspend_always{}; // Suspend after yielding
  }

  // return_value(): Called when co_return is used
  // - Stores the final return value
  // - After this, final_suspend() is called automatically
  void return_value(int value) {
    std::cout << "- Returned value: " << value << std::endl;
    _value = value; // Store the final return value
  }

  // get_return_object(): Creates the Task object for this coroutine
  std::coroutine_handle<Promise> get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  // get_value(): Retrieves the stored value (from yield or return)
  std::optional<int> get_value() { return _value; }

  // _value: Storage for yielded or returned values
  std::optional<int> _value{};
  
  // previous: Handle to the caller coroutine (who is waiting for us)
  // - Set by TaskAwaiter when someone does co_await on our Task
  // - Used by PreviousAwaiter to resume the caller when we complete
  // - This creates the "call stack" for recursive coroutines
  std::coroutine_handle<> previous{std::noop_coroutine()};
};

// ==============================================================================
// Task: The coroutine return type with recursive call support
// ==============================================================================
// Task wraps a coroutine handle and provides co_await support, enabling
// recursive coroutine calls. The key feature is TaskAwaiter, which implements
// the "call" mechanism.
struct Task {
  using promise_type = Promise;

  std::coroutine_handle<promise_type> coroutine;

  // Constructor: Takes ownership of the coroutine handle
  Task(std::coroutine_handle<promise_type> handle) : coroutine(handle) {}

  // Disable copying to prevent double-destruction
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  // Move constructor: Transfer ownership
  Task(Task &&other) : coroutine(other.coroutine) { other.coroutine = nullptr; }

  // Move assignment: Transfer ownership with cleanup
  Task &operator=(Task &&other) {
    if (this != &other) {
      if (coroutine) {
        coroutine.destroy();
      }
      coroutine = other.coroutine;
      other.coroutine = nullptr;
    }
    return *this;
  }

  // Destructor: Clean up the coroutine
  ~Task() {
    if (coroutine) {
      coroutine.destroy();
    }
  }

  // value(): Retrieve the result from the coroutine
  std::optional<int> value() { return coroutine.promise().get_value(); }

  // operator co_await(): Makes Task "awaitable"
  // - Called when you write: co_await some_task
  // - Returns a CalleeAwaiter that handles the downward call mechanism
  CalleeAwaiter operator co_await() { return CalleeAwaiter{coroutine, nullptr}; }
};

// ==============================================================================
// CalleeAwaiter Implementation
// ==============================================================================
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