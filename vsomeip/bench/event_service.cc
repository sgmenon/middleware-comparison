// SOME/IP event notify publisher for vsomeip A/B benchmarks.
#include "bench_common.h"
#include "bench_tracy.h"
#include "constants.h"
#include "fragment.h"

#include <vsomeip/vsomeip.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using vsomeip_bench::Options;
using vsomeip_bench::ParseOptions;
using vsomeip_bench::PlanChunks;
using vsomeip_bench::StampSendNs;
using vsomeip_bench::WriteChunkHeaders;

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
        const auto reliability =
            opt_.transport == "tcp" ? vsomeip::reliability_type_e::RT_RELIABLE : vsomeip::reliability_type_e::RT_UNRELIABLE;
        app_->offer_event(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventId, groups,
                          vsomeip::event_type_e::ET_EVENT, std::chrono::milliseconds::zero(), false, true, nullptr, reliability);
        app_->register_subscription_handler(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventgroupId,
                                            [this](vsomeip::client_t, std::uint32_t, std::uint32_t, bool subscribed) {
                                                if (subscribed) {
                                                    std::lock_guard<std::mutex> lock(mutex_);
                                                    subscribed_ = true;
                                                    cv_.notify_all();
                                                }
                                                return true;
                                            });
        app_->register_message_handler(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEchoMethodId,
                                       [this](const std::shared_ptr<vsomeip::message>& req) {
                                           ZoneScopedN("vsomeip_bench.echo");
                                           auto resp = vsomeip::runtime::get()->create_response(req);
                                           resp->set_payload(req->get_payload());
                                           app_->send(resp);
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
        std::uint32_t seq = 0;
        const int total = opt_.warmup + opt_.count;

        const auto plan = opt_.transport == "tcp" ? vsomeip_bench::ChunkPlan{1, opt_.size} : PlanChunks(opt_.size, opt_.max_datagram);
        TracyPlot("bench.fragments_per_frame", static_cast<int64_t>(plan.count));
        std::vector<std::uint8_t> block(opt_.size);
        for (std::size_t j = 0; j < block.size(); ++j) {
            block[j] = static_cast<std::uint8_t>((j * 131u) & 0xffu);
        }

        for (int i = 0; i < total; ++i) {
            ZoneScopedN("vsomeip_bench.publish_frame");
            const auto t0 = std::chrono::steady_clock::now();
            WriteChunkHeaders(block, seq, plan);
            StampSendNs(block, vsomeip_bench::NowNs());

            for (std::size_t c = 0; c < plan.count; ++c) {
                ZoneScopedN("vsomeip_bench.notify");
                const std::size_t off = c * plan.len;
                const std::size_t n = std::min(plan.len, block.size() - off);
                auto payload = vsomeip::runtime::get()->create_payload();
#if defined(VSOMEIP_BENCH_COVESA_E2E_HOLES)
                // COVESA protects in place, so every notified payload needs the E2E hole in front.
                std::vector<std::uint8_t> buf(vsomeip_bench::kE2eHoleBytes + n);
                std::memcpy(buf.data() + vsomeip_bench::kE2eHoleBytes, block.data() + off, n);
                payload->set_data(std::move(buf));
#else
                payload->set_data(block.data() + off, static_cast<vsomeip::length_t>(n));
#endif
                app_->notify(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventId, std::move(payload));
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
    if (opt.size < vsomeip_bench::kMinFrameBytes) {
        std::fprintf(stderr, "--size must be >= %zu\n", vsomeip_bench::kMinFrameBytes);
        return 2;
    }
    if (opt.stack.empty()) {
        opt.stack = "vsomeip";
    }
    EventService svc(std::move(opt));
    return svc.Run();
}
