// SOME/IP event notify publisher for vsomeip A/B benchmarks.
#include "bench_common.h"
#include "constants.h"
#include "fragment.h"

#include <vsomeip/vsomeip.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using vsomeip_bench::FragmentPayload;
using vsomeip_bench::Options;
using vsomeip_bench::ParseOptions;
using vsomeip_bench::StampPayload;

class EventService {
   public:
    explicit EventService(Options opt) : opt_(std::move(opt)), app_(vsomeip::runtime::get()->create_application()) {}

    int Run() {
        if (!app_->init()) {
            std::fprintf(stderr, "vsomeip init failed\n");
            return 1;
        }
        app_->register_state_handler([this](vsomeip::state_type_e st) { OnState(st); });
        std::set<vsomeip::eventgroup_t> groups{vsomeip_bench::kEventgroupId};
        app_->offer_event(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventId, groups,
                          vsomeip::event_type_e::ET_EVENT, std::chrono::milliseconds::zero(), false, true, nullptr,
                          vsomeip::reliability_type_e::RT_UNRELIABLE);
        app_->register_subscription_handler(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventgroupId,
                                            [this](vsomeip::client_t, std::uint32_t, std::uint32_t, bool subscribed) {
                                                if (subscribed) {
                                                    std::lock_guard<std::mutex> lock(mutex_);
                                                    subscribed_ = true;
                                                    cv_.notify_all();
                                                }
                                                return true;
                                            });

        worker_ = std::thread([this] { Worker(); });
        app_->start();
        if (worker_.joinable()) {
            worker_.join();
        }
        return 0;
    }

   private:
    void OnState(vsomeip::state_type_e st) {
        if (st == vsomeip::state_type_e::ST_REGISTERED) {
            app_->offer_service(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId);
            std::system("touch /tmp/vsomeip-service-ready");
            std::lock_guard<std::mutex> lock(mutex_);
            registered_ = true;
            cv_.notify_all();
        }
    }

    void Worker() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return registered_; });
            cv_.wait(lock, [this] { return subscribed_; });
        }
        if (const char* sync_dir = std::getenv("VSOMEIP_BENCH_SYNC_DIR")) {
            const std::filesystem::path ready = std::filesystem::path(sync_dir) / ".bench-sub-ready";
            for (int i = 0; i < 600; ++i) {
                if (std::filesystem::exists(ready)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        const auto period = std::chrono::duration<double>(1.0 / opt_.rate_hz);
        std::vector<std::uint8_t> body(opt_.size);
        std::uint32_t seq = 0;
        const int total = opt_.warmup + opt_.count;

        for (int i = 0; i < total; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            const std::uint64_t send_ns = vsomeip_bench::NowNs();
            StampPayload(body.data(), body.size(), send_ns, seq);
            const auto chunks = FragmentPayload(seq, body.data(), body.size(), opt_.max_datagram);

            for (const auto& chunk : chunks) {
                auto payload = vsomeip::runtime::get()->create_payload();
                payload->set_data(chunk);
                app_->notify(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventId, payload);
            }
            ++seq;
            const auto t1 = std::chrono::steady_clock::now();
            const auto elapsed = t1 - t0;
            if (elapsed < period) {
                std::this_thread::sleep_for(period - elapsed);
            }
        }

        app_->stop_offer_service(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId);
        app_->clear_all_handler();
        app_->stop();
    }

    Options opt_;
    std::shared_ptr<vsomeip::application> app_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool registered_{false};
    bool subscribed_{false};
    std::thread worker_;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!ParseOptions(argc, argv, &opt)) {
        return 2;
    }
    if (opt.stack.empty()) {
        opt.stack = "vsomeip";
    }
    EventService svc(std::move(opt));
    return svc.Run();
}
