#include <iostream>     // For standard input/output operations (e.g., cout, cerr)
#include <vector>       // For std::vector to hold worker threads
#include <queue>        // For std::queue to manage tasks (using std::deque as underlying container)
#include <thread>       // For std::thread to create and manage threads
#include <mutex>        // For std::mutex to protect shared resources
#include <condition_variable> // For std::condition_variable to synchronize threads
#include <functional>   // For std::function to store callable tasks
#include <future>       // For std::future and std::promise to get task results
#include <stdexcept>    // For std::runtime_error

// A robust ThreadPool class for concurrent task execution.
class ThreadPool {
public:
    // Constructor: Initializes the thread pool with a specified number of worker threads.
    // Throws std::invalid_argument if num_threads is 0.
    explicit ThreadPool(size_t num_threads) : stop_all_threads(false) {
        if (num_threads == 0) {
            throw std::invalid_argument("Number of threads cannot be zero.");
        }

        // Create and launch worker threads. Each thread will execute the worker_thread_loop.
        for (size_t i = 0; i < num_threads; ++i) {
            workers.emplace_back([this] { worker_thread_loop(); });
        }
        std::cout << "ThreadPool initialized with " << num_threads << " threads.\n";
    }

    // Destructor: Gracefully shuts down the thread pool, ensuring all worker threads
    // complete their current tasks and terminate.
    ~ThreadPool() {
        // Set the shutdown flag to true within a locked scope to ensure visibility
        // across all threads.
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop_all_threads = true;
        }
        // Notify all waiting worker threads so they can check the `stop_all_threads`
        // flag and exit their execution loops.
        condition.notify_all();

        // Join all worker threads. This ensures that the main thread waits for all
        // worker threads to finish their execution before the ThreadPool object is
        // destroyed. This prevents resources being freed while threads are still
        // attempting to access them.
        for (std::thread& worker : workers) {
            if (worker.joinable()) { // Check if the thread is joinable (i.e., not already joined)
                worker.join();       // Wait for the thread to finish
            }
        }
        std::cout << "ThreadPool shutdown complete.\n";
    }

    // Submits a task to the thread pool for asynchronous execution.
    // This function takes any callable object (function, lambda, functor) and its
    // arguments, and returns a std::future that will eventually hold the result
    // of the task.
    template <class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
        // Determine the return type of the function F when called with Args.
        using return_type = typename std::result_of<F(Args...)>::type;

        // Create a std::packaged_task. This object pairs a callable target (our task)
        // with a std::promise, allowing its result to be retrieved via a std::future.
        auto task_ptr = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        // Get the future associated with this packaged_task. This future will be
        // returned to the caller.
        std::future<return_type> res = task_ptr->get_future();

        {
            // Acquire a unique lock on the queue_mutex to protect access to the tasks queue.
            std::unique_lock<std::mutex> lock(queue_mutex);

            // If the pool is in the process of stopping, prevent new task submissions
            // to avoid issues during shutdown.
            if (stop_all_threads) {
                throw std::runtime_error("Cannot submit task: ThreadPool is shutting down.");
            }

            // Enqueue the task. The task stored in the queue is a lambda that simply
            // executes the packaged_task.
            tasks.emplace([task_ptr]() { (*task_ptr)(); });
        }
        // Notify one waiting worker thread that a new task is available in the queue.
        condition.notify_one();

        // Return the future to the caller, allowing them to retrieve the task's result
        // when it's ready.
        return res;
    }

private:
    // The main loop executed by each worker thread.
    void worker_thread_loop() {
        while (true) {
            std::function<void()> task; // Holds the task to be executed by the current thread.

            {
                // Acquire a unique lock on the queue_mutex.
                std::unique_lock<std::mutex> lock(queue_mutex);

                // Wait until either the task queue is not empty OR the `stop_all_threads`
                // flag is set. The predicate lambda prevents spurious wake-ups and ensures
                // the condition is genuinely met.
                condition.wait(lock, [this] {
                    return !tasks.empty() || stop_all_threads;
                });

                // Check for shutdown condition: if the pool is stopping AND the queue
                // is empty, it means there are no more tasks to process, and the thread
                // should exit.
                if (stop_all_threads && tasks.empty()) {
                    break; // Exit the worker thread loop
                }

                // Retrieve the next task from the front of the queue.
                // std::move is used for efficiency as we are taking ownership.
                task = std::move(tasks.front());
                tasks.pop();
            } // The unique_lock is automatically released here as it goes out of scope,
              // allowing other threads to access the queue while this thread executes its task.

            // Execute the retrieved task. This is done outside the locked section to
            // maximize concurrency (i.e., multiple threads can process tasks while
            // the queue remains accessible for submission/popping).
            try {
                task(); // Execute the task
            } catch (const std::exception& e) {
                // Log any exceptions thrown by the executed task.
                std::cerr << "Task threw an exception: " << e.what() << std::endl;
            } catch (...) {
                // Catch any other unknown exceptions.
                std::cerr << "Task threw an unknown exception.\n";
            }
        }
    }

    // Member variables:
    std::vector<std::thread> workers; // Stores the actual std::thread objects for worker threads.
    std::queue<std::function<void()>> tasks; // The thread-safe queue for pending tasks.

    std::mutex queue_mutex;             // Mutex to protect access to `tasks` and `stop_all_threads`.
    std::condition_variable condition;  // Condition variable to signal task availability or shutdown.

    bool stop_all_threads;              // Flag indicating whether the thread pool should shut down.
};

// Main function to demonstrate the ThreadPool in action.
int main() {
    std::cout << "Starting Full ThreadPool demonstration...\n";

    try {
        // Create a thread pool with 4 worker threads.
        ThreadPool pool(4);

        // --- Example 1: Submitting tasks that return void ---
        std::cout << "\n--- Submitting void tasks ---\n";
        for (int i = 0; i < 5; ++i) {
            pool.submit([i]() {
                std::cout << "Void Task " << i << " executing on thread ID: "
                          << std::this_thread::get_id() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Simulate work
            });
        }

        // --- Example 2: Submitting tasks that return a value ---
        std::cout << "\n--- Submitting value-returning tasks ---\n";
        std::vector<std::future<int>> results; // To store futures for returned values

        for (int i = 0; i < 5; ++i) {
            // Submit a task that calculates a square and returns an int.
            results.push_back(pool.submit([i]() {
                std::cout << "Value Task " << i << " calculating on thread ID: "
                          << std::this_thread::get_id() << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Simulate work
                return i * i; // Return the calculated value
            }));
        }

        // --- Example 3: Submitting a task that throws an exception ---
        std::cout << "\n--- Submitting a task that throws an exception ---\n";
        auto error_future = pool.submit([]() -> int {
            std::cout << "Error Task executing on thread ID: "
                      << std::this_thread::get_id() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            throw std::runtime_error("Simulated task error!");
            return 0; // Unreachable, but required for return type consistency
        });

        // --- Retrieve results from value-returning tasks ---
        std::cout << "\n--- Retrieving results ---\n";
        for (size_t i = 0; i < results.size(); ++i) {
            try {
                // .get() blocks until the result is available and retrieves it.
                // It will also re-throw any exceptions stored in the future.
                std::cout << "Result for Value Task " << i << ": " << results[i].get() << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error retrieving result for Value Task " << i << ": " << e.what() << std::endl;
            }
        }

        // --- Retrieve result from the error-throwing task ---
        try {
            std::cout << "Attempting to get result from Error Task...\n";
            error_future.get(); // This will re-throw the exception
        } catch (const std::exception& e) {
            std::cerr << "Caught expected exception from Error Task: " << e.what() << std::endl;
        }

        // Give some time for all tasks to potentially finish before the pool destructor is called.
        // In a real application, you might have a more explicit mechanism to wait for all
        // submitted tasks to complete before allowing the pool to destruct.
        std::this_thread::sleep_for(std::chrono::seconds(1));

    } catch (const std::exception& e) {
        std::cerr << "An error occurred during ThreadPool creation or operation: " << e.what() << std::endl;
    }

    std::cout << "\nDemonstration finished. ThreadPool will now shut down gracefully.\n";
    return 0;
}
