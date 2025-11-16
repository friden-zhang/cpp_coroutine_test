#include <coroutine>
#include <iostream>
#include <optional>

// ==============================================================================
// PreviousAwaiter: Implements the "return" mechanism (UPWARD traversal)
// ==============================================================================
// This awaiter is used in final_suspend() to automatically resume the caller
// coroutine when the current coroutine completes. It traverses UP the call chain.
//
// Execution flow:
//   world() completes → PreviousAwaiter resumes hello()
//   hello() completes → PreviousAwaiter tries to resume main (but main is not a coroutine)
//
// Relationship with CalleeAwaiter:
// - CalleeAwaiter: Downward traversal (caller → callee, going deeper)
// - PreviousAwaiter: Upward traversal (callee → caller, climbing back)
struct PreviousAwaiter {
  // previous: Handle to the caller coroutine (who is waiting for us to complete)
  // - Set by CalleeAwaiter when caller does co_await
  // - Used to resume caller when this coroutine completes
  std::coroutine_handle<> previous{std::noop_coroutine()};

  // await_ready(): Always return false to ensure await_suspend is called
  // - This allows us to check if there's a caller to resume
  auto await_ready() noexcept { return false; }

  // await_suspend(): Called when this coroutine completes (co_return)
  // - Returns the caller's handle to resume it (symmetric transfer UP)
  // - This is the "return" mechanism - climbing back up the call chain
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> coroutine) noexcept {
    if (previous && !previous.done()) {
      std::cout
          << "- [PreviousAwaiter] Climbing up: resuming previous coroutine."
          << std::endl;
      return previous;  // Resume the caller (symmetric transfer)
    } else {
      std::cout << "- No previous coroutine to resume." << std::endl;
      return std::noop_coroutine();  // No caller, return no-op
    }
  }

  // await_resume(): Called after resuming, but does nothing for PreviousAwaiter
  // - The actual resumption happens via symmetric transfer in await_suspend
  auto await_resume() noexcept {}
};

// ==============================================================================
// Promise: Defines coroutine behavior and manages state
// ==============================================================================
// The Promise type is the heart of a coroutine. It controls:
// - When to suspend (initial_suspend, final_suspend)
// - How to store values (yield_value, return_value)
// - How to link coroutines (previous pointer for call chain)
//
// This Promise is shared by both Task and WorldTask types.
struct Promise {

  // initial_suspend(): Coroutine starts suspended (lazy execution)
  // - The coroutine won't run until explicitly resumed
  auto initial_suspend() { return std::suspend_always{}; }
  
  // final_suspend(): Called when co_return is executed
  // - Returns PreviousAwaiter to automatically resume the caller
  // - This implements the automatic "return" mechanism
  // - Must be noexcept (C++ coroutine requirement)
  auto final_suspend() noexcept { return PreviousAwaiter{previous}; }

  // unhandled_exception(): Rethrow any exception that escapes the coroutine
  auto unhandled_exception() { throw; }

  // yield_value(): Called when co_yield is used
  // - Stores the yielded value in _value
  // - Returns suspend_always to pause execution
  // - Control returns to the resumer (typically main or another coroutine)
  auto yield_value(int value) {
    std::cout << "- Yielded value: " << value << std::endl;
    _value = value;
    return std::suspend_always{};  // Suspend after yielding
  }

  // return_value(): Called when co_return is used with a value
  // - Stores the final return value
  // - After this, final_suspend() is automatically called
  void return_value(int value) {
    std::cout << "- Returned value: " << value << std::endl;
    _value = value;
  }

  // get_return_object(): Creates the coroutine's return object (Task/WorldTask)
  // - Called when the coroutine is first created
  // - Returns a handle that can be used to control the coroutine
  std::coroutine_handle<Promise> get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  // value(): Retrieves the stored value (from yield or return)
  std::optional<int> value() { return _value; }

  // _value: Storage for yielded or returned values
  std::optional<int> _value;
  
  // previous: Handle to the caller coroutine (set by CalleeAwaiter)
  // - Used by PreviousAwaiter to resume the caller when this coroutine completes
  // - Forms the "call stack" for coroutine chains
  std::coroutine_handle<> previous{std::noop_coroutine()};
};

// ==============================================================================
// Task: RAII wrapper for hello() coroutine
// ==============================================================================
// Task manages the lifetime of the hello() coroutine. It provides:
// - Automatic cleanup via destructor
// - Access to the coroutine's yielded/returned values
// - A handle to control the coroutine (resume, check done, etc.)
struct Task {
  using promise_type = Promise;
  std::coroutine_handle<Promise> coroutine;

  // Constructor: Takes ownership of the coroutine handle
  Task(std::coroutine_handle<Promise> handle) : coroutine(handle) {}

  // value(): Retrieves the current value from the promise
  std::optional<int> value() { return coroutine.promise()._value; }

  // Destructor: Cleans up the coroutine when Task is destroyed
  ~Task() {
    if (coroutine) {
      coroutine.destroy();
    }
  }
};

// ==============================================================================
// WorldTask: RAII wrapper for world() coroutine with co_await support
// ==============================================================================
// WorldTask is similar to Task but adds co_await support via CalleeAwaiter.
// This allows it to be used in co_await expressions within other coroutines.
struct WorldTask {
  using promise_type = Promise;
  std::coroutine_handle<Promise> coroutine;

  // Constructor: Takes ownership of the coroutine handle
  WorldTask(std::coroutine_handle<Promise> handle) : coroutine(handle) {}

  // Destructor: Cleans up the coroutine
  ~WorldTask() {
    if (coroutine) {
      coroutine.destroy();
    }
  }

  // value(): Retrieves the current value from the promise
  std::optional<int> value() { return coroutine.promise()._value; }

  // ============================================================================
  // CalleeAwaiter: Implements the "call" mechanism (DOWNWARD traversal)
  // ============================================================================
  // This awaiter enables co_await support for WorldTask. When you write:
  //   int result = co_await world_task;
  // CalleeAwaiter:
  // 1. Suspends the caller (hello)
  // 2. Sets up the return path (world.previous = hello)
  // 3. Executes world() completely (through all yields until co_return)
  // 4. Returns the final value to the caller
  //
  // Key difference from typical awaiters:
  // - The while loop executes ALL of world() immediately
  // - All co_yield values are consumed internally
  // - Only the final co_return value is returned via await_resume()
  struct CalleeAwaiter {
    std::coroutine_handle<Promise> callee;  // The coroutine being called (world)
    std::coroutine_handle<> caller;          // The coroutine doing the calling (hello)

    // await_ready(): Check if callee is already done
    // - Returns true if callee is complete (optimization to skip suspension)
    // - Returns false if callee needs to be executed
    bool await_ready() noexcept { return callee ? callee.done() : false; }

    // await_suspend(): The "call" mechanism - executes callee completely
    // - Suspends the caller (hello)
    // - Sets up return path: callee.previous = caller
    // - Executes callee in a loop until completion
    // - Returns noop_coroutine() because callee's final_suspend already
    //   resumed the caller via PreviousAwaiter
    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
      // Set up the return path for PreviousAwaiter
      callee.promise().previous = awaiting_coroutine;

      // Execute callee completely (through all co_yield and co_return)
      // Each resume() executes until the next suspension point
      // Loop iterations:
      //   1st: world() starts → co_yield 1 → suspends
      //   2nd: world() resumes → co_yield 2 → suspends
      //   3rd: world() resumes → co_return 42 → completes
      //        final_suspend() → PreviousAwaiter resumes caller (hello)
      while (!callee.done()) {
        callee.resume();
      }

      // Don't resume any coroutine here because:
      // - callee has already completed
      // - callee's final_suspend() used PreviousAwaiter to resume caller
      // - Returning callee would cause segfault (accessing completed coroutine)
      return std::noop_coroutine();
    }

    // await_resume(): Called when control returns to caller
    // - Returns the final value from callee (co_return value)
    // - This value becomes the result of the co_await expression
    // - Example: int val = co_await world_task;  // val = 42
    int await_resume() noexcept {
      std::cout << "- [CalleeAwaiter] Resuming caller after callee "
                   "completion."
                << std::endl;
      auto val = callee.promise().value();
      return val.value_or(0);  // Return final value or 0 if none
    }
  };

  // operator co_await(): Makes WorldTask "awaitable"
  // - Called when WorldTask is used in a co_await expression
  // - Returns CalleeAwaiter which handles the actual suspension/resumption
  CalleeAwaiter operator co_await() {
    return CalleeAwaiter{coroutine, nullptr};
  }
};

// ==============================================================================
// world(): A coroutine that yields multiple values and returns a final value
// ==============================================================================
// This coroutine demonstrates:
// - Multiple co_yield statements (yielding 1, then 2)
// - Final co_return statement (returning 42)
// - When called via co_await, all yields are consumed by CalleeAwaiter's loop
// - Only the final co_return value (42) is returned to the caller
WorldTask world() {
  std::cout << "WorldTask started." << std::endl;
  
  // First yield: suspends and stores 1
  // When used in co_await, CalleeAwaiter immediately resumes it
  co_yield 1;
  
  std::cout << "WorldTask resuming after first yield." << std::endl;
  
  // Second yield: suspends and stores 2
  // Again, CalleeAwaiter immediately resumes it
  co_yield 2;
  
  std::cout << "WorldTask resuming after second yield." << std::endl;
  
  // Final return: stores 42 and triggers final_suspend()
  // final_suspend() → PreviousAwaiter → resumes hello()
  // This value (42) is what await_resume() returns to the caller
  co_return 42;
}

// ==============================================================================
// hello(): A coroutine that calls world() and yields its own values
// ==============================================================================
// This coroutine demonstrates:
// - Calling another coroutine via co_await (world_task)
// - Receiving the final value from the callee (val1 = 42)
// - Yielding its own values (42, 100)
// - Returning its own final value (200)
//
// Execution flow:
// 1. Creates world() coroutine (suspended)
// 2. co_await world_task triggers CalleeAwaiter
// 3. CalleeAwaiter executes world() completely (all yields + return)
// 4. Receives 42 from world()
// 5. Yields 42, then 100
// 6. Returns 200
Task hello() {
  // Create world() coroutine (initially suspended)
  auto world_task = world();
  
  // co_await world_task:
  // - Calls operator co_await() → returns CalleeAwaiter
  // - CalleeAwaiter::await_suspend() executes world() completely
  // - CalleeAwaiter::await_resume() returns 42
  // - val1 receives 42
  int val1 = co_await world_task;
  
  std::cout << "Hello received from WorldTask: " << val1 << std::endl;

  // Yield the value received from world()
  co_yield val1;  // Yields 42
  
  // Yield another value
  co_yield 100;
  
  // Return final value
  // This triggers final_suspend() → PreviousAwaiter
  // But hello().previous is noop, so it just completes
  co_return 200;
}

// ==============================================================================
// main(): Entry point - drives the coroutine execution
// ==============================================================================
// Demonstrates manual coroutine control:
// - Creates hello() coroutine (which internally creates world())
// - Manually resumes hello() in a loop
// - Retrieves the final result after completion
//
// Execution timeline:
// 1. task = hello() - creates suspended hello() coroutine
// 2. First resume():
//    - hello() starts
//    - Creates world() coroutine
//    - co_await world_task → CalleeAwaiter executes
//    - CalleeAwaiter loop: world() executes completely (all yields + return 42)
//    - hello() receives 42, prints it
//    - hello() executes co_yield 42 → suspends
// 3. Second resume():
//    - hello() continues from co_yield 42
//    - Executes co_yield 100 → suspends
// 4. Third resume():
//    - hello() continues from co_yield 100
//    - Executes co_return 200 → completes
//    - final_suspend() → PreviousAwaiter (but previous is noop)
// 5. Loop exits (task.coroutine.done() == true)
// 6. Prints final result: 200
int main() {
  // Create hello() coroutine (suspended at initial_suspend)
  auto task = hello();
  
  // Resume hello() repeatedly until it completes
  // Each resume() runs until the next suspension point (co_yield or co_return)
  while (task.coroutine && !task.coroutine.done()) {
    task.coroutine.resume();
  }
  
  // After hello() completes, retrieve the final value (200)
  std::cout << "\nFinal result: " << *task.value() << std::endl;
  
  return 0;
}