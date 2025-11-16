#include <coroutine>
#include <iostream>
#include <optional>

// ==============================================================================
// RepeatAwaiter: A custom awaiter that implements the awaiter interface
// ==============================================================================
// The awaiter interface consists of three methods that control the suspension
// and resumption behavior of a coroutine when using co_await.
struct RepeatAwaiter {
  
  // await_ready(): Determines whether the coroutine should suspend
  // - Return true:  The result is ready, don't suspend (optimization)
  // - Return false: The result is not ready, suspend the coroutine
  // This is the first method called in the co_await expression.
  bool await_ready() { 
    return false;  // Always suspend for demonstration purposes
  }
  
  // await_suspend(): Called immediately after the coroutine is suspended
  // - Parameter: The handle of the suspended coroutine
  // - Return value controls what happens next:
  //   * void: Suspend and return control to caller
  //   * bool: true = suspend, false = resume immediately
  //   * coroutine_handle<>: Resume the returned coroutine handle
  // This is where you typically schedule async work or transfer control.
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> coroutine) {
    if (!coroutine.done()) {
      std::cout << "- In await_suspend, resuming coroutine." << std::endl;
      // Return the same coroutine handle to resume it immediately
      // This creates a "repeat" effect - suspend then immediately resume
      return coroutine;
    } else {
      std::cout << "- Coroutine is done, not resuming." << std::endl;
      // Return a no-op coroutine that does nothing when resumed
      return std::noop_coroutine();
    }
  }

  // await_resume(): Called when the coroutine resumes from suspension
  // - The return value of this method becomes the result of the co_await expression
  // - For example: auto result = co_await awaiter; // result = await_resume()'s return
  void await_resume() {
    std::cout << "- In await_resume, coroutine resumed." << std::endl;
    // void return means co_await expression has no value
  }
};

// ==============================================================================
// RepeatAwaitable: A wrapper that provides the co_await operator
// ==============================================================================
// This struct makes RepeatAwaiter "awaitable" by implementing operator co_await.
// When you use co_await on a RepeatAwaitable, it returns a RepeatAwaiter.
struct RepeatAwaitable {
  // operator co_await: Converts this awaitable into an awaiter
  // - This operator is called when co_await is used on this type
  // - It returns the actual awaiter (RepeatAwaiter) that controls suspension
  RepeatAwaiter operator co_await() { return RepeatAwaiter(); }
};

// ==============================================================================
// Promise: The promise type that defines coroutine behavior
// ==============================================================================
// The Promise type is the heart of a coroutine. It defines:
// - What happens when the coroutine is created (initial_suspend)
// - What happens when the coroutine finishes (final_suspend)
// - How values are yielded (yield_value) or returned (return_value)
// - How to get the coroutine handle (get_return_object)
//
// Template parameters:
// - T: The type of values yielded/returned by the coroutine
// - Awaiter: The awaiter type used for co_yield (default: std::suspend_always)
template <typename T, typename Awaiter = std::suspend_always> 
struct Promise {
  
  // initial_suspend(): Controls whether the coroutine starts immediately
  // - Return std::suspend_always: Coroutine is created suspended (lazy start)
  // - Return std::suspend_never: Coroutine starts executing immediately
  // Called when the coroutine is first created, before executing the body.
  auto initial_suspend() { return std::suspend_always(); }
  
  // final_suspend(): Controls what happens when the coroutine completes
  // - Return std::suspend_always: Keep coroutine alive after completion
  // - Return std::suspend_never: Destroy coroutine immediately after completion
  // Must be noexcept to prevent exceptions during cleanup.
  auto final_suspend() noexcept { return std::suspend_always(); }
  
  // unhandled_exception(): Called when an exception escapes the coroutine body
  // - Can store the exception with std::current_exception()
  // - Can rethrow it (as done here)
  // - Can handle it silently
  void unhandled_exception() { throw; }

  // yield_value(): Called when co_yield is used in the coroutine body
  // - Parameter: The value being yielded
  // - Return value: An awaiter that controls what happens after yielding
  // - The return type determines suspension behavior:
  //   * std::suspend_always: Always suspend after yielding
  //   * std::suspend_never: Never suspend after yielding
  //   * Custom awaiter: Use custom suspension logic
  auto yield_value(T value) {
    std::cout << "- Yielded value: " << value << std::endl;
    _value = value;  // Store the yielded value
    return Awaiter{};  // Return awaiter to control suspension
  }

  // return_value(): Called when co_return <value> is used in the coroutine body
  // - Parameter: The value being returned
  // - Stores the final return value from the coroutine
  // - Note: A promise must have either return_value() OR return_void(), not both
  void return_value(T value) {
    std::cout << "- Returned value: " << value << std::endl;
    _value = value;  // Store the final return value
  }

  // get_return_object(): Called to create the coroutine's return object (Task)
  // - This is called when the coroutine is first invoked
  // - Returns a handle that can be used to control the coroutine
  // - The return type must match the function's return type (Task in this case)
  std::coroutine_handle<Promise> get_return_object() {
    return std::coroutine_handle<Promise>::from_promise(*this);
  }

  // get_value(): User-defined helper method to retrieve the stored value
  // - Not part of the promise interface, but useful for accessing results
  // - Returns std::optional to handle cases where no value has been set yet
  std::optional<T> get_value() { return _value; }

  // _value: Storage for yielded or returned values
  // - Uses std::optional to represent "no value yet" state
  // - Updated by yield_value() when co_yield is used
  // - Updated by return_value() when co_return is used
  std::optional<T> _value{};
};

// ==============================================================================
// Promise<void> Partial Specialization (commented out)
// ==============================================================================
// This would be a partial specialization for coroutines that don't yield/return values.
// Key differences from the main template:
// - Uses return_void() instead of return_value(T)
// - No _value member needed since nothing is stored
// - yield_value() takes no parameters (just controls suspension)
//
// Note: Partial specializations cannot have default template arguments.
// The default value (std::suspend_always) is inherited from the primary template.
//
// template <typename Awaiter> struct Promise<void, Awaiter> {
//   auto initial_suspend() { return std::suspend_always{}; }
//   auto final_suspend() noexcept { return std::suspend_always{}; }
//   void unhandled_exception() { throw; }
//   auto yield_value() { return Awaiter{}; }
//   void return_void() {}
//   std::coroutine_handle<Promise> get_return_object() {
//     return std::coroutine_handle<Promise>::from_promise(*this);
//   }
// };

// ==============================================================================
// Task: The coroutine return type (RAII wrapper for coroutine handle)
// ==============================================================================
// Task is what the coroutine function returns. It manages the lifetime of the
// coroutine and provides an interface to interact with it.
//
// Key responsibilities:
// - Owns the coroutine handle (manages its lifetime)
// - Provides methods to resume and query the coroutine
// - Ensures proper cleanup when destroyed
struct Task {
  // promise_type: Required typedef that tells the compiler which Promise to use
  // - The compiler looks for this type to know how to construct the coroutine
  // - Must match the Promise template used in the coroutine implementation
  // using promise_type = Promise<int, std::suspend_always>;  // Default behavior
  using promise_type = Promise<int, RepeatAwaiter>;  // Custom behavior with RepeatAwaiter
  
  // coroutine: Handle to the underlying coroutine
  // - Used to resume, destroy, and query the coroutine state
  // - Must be properly managed to avoid memory leaks
  std::coroutine_handle<promise_type> coroutine;

  // Constructor: Initializes Task with a coroutine handle
  // - Called by Promise::get_return_object() when the coroutine is created
  // - Takes ownership of the coroutine handle
  Task(std::coroutine_handle<promise_type> handle) : coroutine(handle) {}

  // Copy constructor and assignment operator: DELETED
  // - Coroutine handles should not be copied to prevent double-destruction
  // - Only one Task should own a given coroutine handle
  // - Attempting to copy will result in a compile error
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;
  
  // Move constructor: Transfers ownership of the coroutine handle
  // - Moves the coroutine handle from 'other' to this Task
  // - Sets other.coroutine to nullptr to prevent double-destruction
  // - noexcept ensures no exceptions are thrown during move
  Task(Task &&other) noexcept : coroutine(other.coroutine) {
    other.coroutine = nullptr;
  }
  
  // Move assignment operator: Transfers ownership with proper cleanup
  // - Destroys the current coroutine (if any) before taking ownership
  // - Moves the coroutine handle from 'other' to this Task
  // - Sets other.coroutine to nullptr to prevent double-destruction
  Task &operator=(Task &&other) noexcept {
    if (this != &other) {  // Self-assignment check
      if (coroutine) {
        coroutine.destroy();  // Clean up existing coroutine
      }
      coroutine = other.coroutine;
      other.coroutine = nullptr;
    }
    return *this;
  }

  // Destructor: Cleans up the coroutine when Task is destroyed
  // - Calls coroutine.destroy() to free the coroutine frame memory
  // - Only destroys if coroutine is not nullptr (important after move)
  // - This is RAII: Resource Acquisition Is Initialization
  ~Task() {
    if (coroutine) {
      coroutine.destroy();
    }
  }

  // value(): Helper method to retrieve the current value from the promise
  // - Accesses the promise through the coroutine handle
  // - Returns std::optional<int> which may be empty if no value has been set
  // - Can be called after co_yield or co_return to get the result
  std::optional<int> value() { return coroutine.promise().get_value(); }
};

// ==============================================================================
// work(): Example coroutine function that demonstrates yielding and returning
// ==============================================================================
// This function is a coroutine because it uses co_yield and co_return.
// - Return type is Task, which must have a promise_type typedef
// - co_yield 1: Yields value 1, suspends, and waits for resume
// - co_yield 2: Yields value 2, suspends, and waits for resume
// - co_return 3: Returns final value 3 and marks coroutine as done
Task work() {
  co_yield 1;   // First suspension point - yields 1
  co_yield 2;   // Second suspension point - yields 2
  co_return 3;  // Final value - marks coroutine as done
}

// ==============================================================================
// main(): Demonstrates how to use a coroutine
// ==============================================================================
int main() {
  // Step 1: Create the coroutine
  // - Calling work() creates a coroutine in suspended state (initial_suspend)
  // - The coroutine body has not started executing yet
  std::cout << "Creating coroutine..." << std::endl;
  Task task = work();
  std::cout << "coroutine created." << std::endl;

  // Step 2: Resume the coroutine repeatedly until it's done
  // - task.coroutine.done() returns false while coroutine is still running
  // - Each resume() call continues execution until the next suspension point
  while (!task.coroutine.done()) {
    std::cout << "Resuming coroutine..." << std::endl;
    task.coroutine.resume();  // Resume execution from last suspension point
    std::cout << "Coroutine resumed." << std::endl;
    
    // Step 3: Check if a value was yielded or returned
    // - task.value() retrieves the value from the promise (if any)
    // - Returns std::optional<int>, which is empty if no value was set
    if (auto val = task.value()) {
      std::cout << "Got value from coroutine: " << *val << std::endl;
    } else {
      std::cout << "No value yielded yet." << std::endl;
    }
  }
  
  // Step 4: Coroutine is done
  // - The while loop exits when task.coroutine.done() returns true
  // - The Task destructor will automatically clean up the coroutine
  std::cout << "Coroutine completed." << std::endl;
}