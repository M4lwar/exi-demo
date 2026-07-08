# Threading & startup — libexificient C++ integration

How to use the library from a multi-threaded C++ app and hide the schema-load
latency off your startup path. Runnable reference: **`threading_example.cpp`**
(the `threading-example` build target); concurrent-encode reference:
`exi_demo.cpp` (`cmd_threads`).

## The model — three invariants

- **One isolate per process.** `graal_create_isolate` is cheap (stands up the
  GC heap, ~ms). Call it once, on the main thread, up front.
- **Per-thread tokens.** A `graal_isolatethread_t*` is valid **only on the
  thread that obtained it**. Every thread that calls any `exi_*` function must
  `graal_attach_thread` for its own token and `graal_detach_thread` when done.
  **Never pass a token to another thread** — that is undefined behavior and
  shows up as heap corruption.
- **Shared contexts.** An `exi_ctx` is a global, isolate-wide handle. Create it
  once (on any thread), then share the same handle across every thread. It is
  immutable and thread-safe, and it **outlives the thread that created it**.

## The costs — why you thread the load

| Call | Cost | Where it belongs |
|---|---|---|
| `graal_create_isolate` | ~ms (GC heap) | main thread, up front |
| `exi_create(path, ...)` | **seconds** for a large schema (XSD parse + grammar build) | a background thread — this is what you hide |
| `exi_encode` / `exi_decode` / `exi_peek_root` | fast, per-call | any attached thread, concurrent on the shared ctx |

## Pattern — load off the startup path

```cpp
// main thread, up front: cheap.
graal_isolate_t* isolate = nullptr;
graal_isolatethread_t* main_t = nullptr;
graal_create_isolate(nullptr, &isolate, &main_t);

// hide the SLOW exi_create. Capture BY VALUE — this task outlives the current
// scope, so a [&] capture would dangle (a real heap-corruption source).
std::future<exi_ctx> fut = std::async(std::launch::async,
    [isolate, schema = std::string(path)] {
        graal_isolatethread_t* wt = nullptr;
        graal_attach_thread(isolate, &wt);              // its OWN token
        exi_ctx ctx = nullptr;
        exi_status s = exi_create(wt, schema.c_str(), 0, &ctx);
        graal_detach_thread(wt);                        // ctx survives this
        if (s != EXI_OK) throw std::runtime_error("exi_create failed");
        return ctx;                                     // valid on any thread
    });

// main proceeds with the rest of startup, unblocked...
exi_ctx ctx = fut.get();                                // block only if not ready

// use it from worker threads, each with its own token, sharing the one ctx:
std::thread([&] {
    graal_isolatethread_t* wt = nullptr;
    graal_attach_thread(isolate, &wt);
    char* out = nullptr; size_t n = 0;
    exi_encode(wt, ctx, xml.data(), xml.size(), &out, &n);
    exi_free(wt, out);                                  // free with THIS token
    graal_detach_thread(wt);
}).join();                                              // join before captures die
```

## Pitfalls

- **Never share a token across threads.** Pass the `graal_isolate_t*` (the
  isolate handle) to a thread; it attaches for its own token.
- **In async / fire-and-forget tasks, capture handles by value**, not `[&]`.
  `[&]` dangles the instant the spawning scope returns → heap corruption. If you
  do use `[&]`, you must `join()` before those locals go out of scope.
- **`exi_ctx` survives its creator's detach** — hand it back freely (e.g. via
  `std::future`).
- **`exi_last_error` is per-thread.** Read it on the failing thread immediately,
  before any suspension/`co_await`, or you'll get another task's message.
- **`exi_free` / `exi_destroy` need a live token** — call them from an attached
  thread (any attached thread; not necessarily the allocating one).

## Simpler alternative — bake the schema

If the schema is known at build time, a **baked** build makes
`exi_create(NULL, ...)` ~ms instead of seconds, so there is nothing to hide and
no background thread is needed. The load-off-startup pattern above is for the
**runtime / arbitrary schema** case (the generic build), where you can't know
the schema until run time.
