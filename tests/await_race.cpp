// Regression tests for CAsyncResourceGatherer::await().
// 针对 CAsyncResourceGatherer::await() 的回归测试。
//
// A waiter must never hang, never return before the resource is published, and every
// concurrent waiter must be woken.
// 等待者不得挂死、不得在资源发布前返回，且所有并发等待者都必须被唤醒。

#include <hyprgraphics/resource/AsyncResourceGatherer.hpp>
#include <hyprgraphics/resource/resources/AsyncResource.hpp>
#include <hyprutils/memory/Atomic.hpp>
#include <hyprutils/memory/UniquePtr.hpp>

#include <atomic>
#include <chrono>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace Hyprgraphics;
using namespace Hyprutils::Memory;

namespace {

    // Short render so enqueue/await overlap in the racy window.
    // 渲染耗时很短，让 enqueue 与 await 落在竞态窗口里。
    struct SFakeResource : IAsyncResource {
        std::atomic<bool> payloadVisible{false};
        std::atomic<int>  renders{0};

        void              render() override {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            payloadVisible.store(true, std::memory_order_relaxed);
            renders.fetch_add(1, std::memory_order_relaxed);
        }
    };

    // Both flags are written by the worker before the waiter is notified.
    // 两个标志都由 worker 在唤醒等待者之前写入。
    bool published(SFakeResource& res) {
        return res.m_ready.load() && res.payloadVisible.load();
    }

    int  failures = 0;

    void report(const char* name, bool ok) {
        std::println("{:<34} {}", name, ok ? "PASS" : "FAIL");
        if (!ok)
            failures++;
    }

    // The worker may finish before await() is even called.
    // worker 可能在 await() 调用之前就完成。
    bool completionBeforeAwait() {
        auto  gatherer = makeUnique<CAsyncResourceGatherer>();
        auto  res      = makeAtomicShared<SFakeResource>();

        auto& REF = *res;

        gatherer->enqueue(res);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        gatherer->await(res);
        return published(REF);
    }

    // await() must return with the payload published, every time.
    // 每次 await() 返回时，负载都必须已发布。
    bool awaitReturnsPublished() {
        auto gatherer = makeUnique<CAsyncResourceGatherer>();

        for (int i = 0; i < 3000; ++i) {
            auto res = makeAtomicShared<SFakeResource>();
            gatherer->enqueue(res);
            gatherer->await(res);

            if (!published(*res))
                return false;
        }

        return true;
    }

    // Every waiter on the same resource must be woken.
    // 同一资源上的每个等待者都必须被唤醒。
    bool concurrentWaiters() {
        auto gatherer = makeUnique<CAsyncResourceGatherer>();
        auto res      = makeAtomicShared<SFakeResource>();

        gatherer->enqueue(res);

        std::atomic<int>         early{0};
        std::vector<std::thread> threads;
        threads.reserve(8);
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&] {
                gatherer->await(res);
                if (!published(*res))
                    early.fetch_add(1);
            });
        }
        for (auto& t : threads)
            t.join();

        return early.load() == 0;
    }

}

int main(int argc, char** argv) {
    // Optional single-scenario selector, so a hanging scenario does not hide the others.
    // 可选单场景选择器，避免一个挂死场景掩盖其它场景。
    const std::string ONLY = argc > 1 ? argv[1] : "";

    if (ONLY.empty() || ONLY == "completion")
        report("completion-before-await", completionBeforeAwait());
    if (ONLY.empty() || ONLY == "published")
        report("await-returns-published", awaitReturnsPublished());
    if (ONLY.empty() || ONLY == "concurrent")
        report("concurrent-waiters", concurrentWaiters());

    if (failures)
        std::println("{} scenario(s) failed", failures);
    return failures ? 1 : 0;
}
