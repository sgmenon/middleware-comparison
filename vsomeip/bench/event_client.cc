// SOME/IP event notify subscriber for vsomeip A/B benchmarks.
#include "bench_common.h"
#include "bench_tracy.h"
#include "constants.h"
#include "fragment.h"

#include <vsomeip/vsomeip.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace {

using vsomeip_bench::Options;
using vsomeip_bench::ParseOptions;
using vsomeip_bench::PrintCsv;
using vsomeip_bench::ReadStamp;
using vsomeip_bench::Reassembler;

class EventClient {
   public:
    explicit EventClient(Options opt) : opt_(std::move(opt)), app_(vsomeip::runtime::get()->create_application()) {}

    int Run() {
        if (!app_->init()) {
            std::fprintf(stderr, "vsomeip init failed\n");
            return 1;
        }
        app_->register_state_handler([this](vsomeip::state_type_e st) { OnState(st); });
        app_->register_availability_handler(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId,
                                            [this](vsomeip::service_t, vsomeip::instance_t, bool avail) { OnAvailability(avail); });

        app_->register_message_handler(vsomeip::ANY_SERVICE, vsomeip::ANY_INSTANCE, vsomeip::ANY_METHOD,
                                       [this](const std::shared_ptr<vsomeip::message>& msg) { OnMessage(msg); });

        worker_ = std::thread([this] { WaitAndReport(); });
        app_->start();
        if (worker_.joinable()) {
            worker_.join();
        }
        return failed_ ? 1 : 0;
    }

   private:
    void OnState(vsomeip::state_type_e st) {
        if (st == vsomeip::state_type_e::ST_REGISTERED) {
            app_->request_service(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId);
            std::lock_guard<std::mutex> lock(mutex_);
            registered_ = true;
            cv_.notify_all();
        }
    }

    void OnAvailability(bool avail) {
        if (!avail) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (available_) {
                return;
            }
        }
        std::set<vsomeip::eventgroup_t> groups{vsomeip_bench::kEventgroupId};
        app_->request_event(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventId, groups,
                            vsomeip::event_type_e::ET_EVENT, vsomeip::reliability_type_e::RT_UNRELIABLE);
        app_->subscribe(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventgroupId);
        if (const char* sync_dir = std::getenv("VSOMEIP_BENCH_SYNC_DIR")) {
            std::filesystem::path ready = std::filesystem::path(sync_dir) / ".bench-sub-ready";
            std::error_code ec;
            std::filesystem::create_directories(ready.parent_path(), ec);
            std::ofstream(ready).put('1');
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            available_ = true;
            cv_.notify_all();
        }
    }

    void OnMessage(const std::shared_ptr<vsomeip::message>& msg) {
        ZoneScopedN("vsomeip_bench.on_message");
        if (msg->get_message_type() != vsomeip::message_type_e::MT_NOTIFICATION) {
            return;
        }
        if (msg->get_service() != vsomeip_bench::kServiceId || msg->get_instance() != vsomeip_bench::kInstanceId ||
            msg->get_method() != vsomeip_bench::kEventId) {
            return;
        }
        auto pl = msg->get_payload();
        const auto* data = pl->get_data();
        const std::size_t len = pl->get_length();
        std::optional<std::vector<std::uint8_t>> assembled;
        {
            ZoneScopedN("vsomeip_bench.reassemble");
            assembled = reassembler_.ingest(data, len);
        }
        if (!assembled) {
            return;
        }
        if (assembled->size() != opt_.size) {
            return;
        }

        std::uint64_t send_ns = 0;
        std::uint32_t seq = 0;
        if (!ReadStamp(assembled->data(), assembled->size(), &send_ns, &seq)) {
            return;
        }

        const std::uint64_t recv_ns = vsomeip_bench::NowNs();
        const double us = static_cast<double>(recv_ns - send_ns) / 1000.0;

        std::lock_guard<std::mutex> lock(mutex_);
        if (seq > last_seq_ + 1) {
            gap_count_ += seq - last_seq_ - 1;
        }
        last_seq_ = seq;

        if (seq < static_cast<std::uint32_t>(opt_.warmup)) {
            return;
        }
        VSOMEIP_BENCH_PLOT_LATENCY_US("bench.frame_latency_us", us);
        latencies_us_.push_back(us);
        if (static_cast<int>(latencies_us_.size()) >= opt_.count) {
            done_ = true;
            cv_.notify_all();
        }
    }

    void WaitAndReport() {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
            while (std::chrono::steady_clock::now() < deadline) {
                if (cv_.wait_for(lock, std::chrono::seconds(10), [this] { return registered_ && available_; })) {
                    break;
                }
                std::fprintf(stderr, "waiting for vsomeip: registered=%d available=%d (SD / routing)\n", registered_ ? 1 : 0,
                             available_ ? 1 : 0);
            }
            if (!registered_ || !available_) {
                std::fprintf(stderr, "timeout waiting for vsomeip registration/service (routing manager up?)\n");
                failed_ = true;
                vsomeip_bench::TouchBenchSubDone();
                std::quick_exit(1);
            }
            int wait_sec = 120;
            if (const char* v = std::getenv("VSOMEIP_BENCH_WAIT_SEC")) {
                wait_sec = std::max(1, std::atoi(v));
            } else if (opt_.size >= 10'485'760) {
                wait_sec = 3600;
            } else if (opt_.size >= 4'194'304) {
                wait_sec = 900;
            }
            cv_.wait_for(lock, std::chrono::seconds(wait_sec), [this] { return done_; });
        }
        PrintCsv(opt_, latencies_us_, gap_count_);
        vsomeip_bench::TouchBenchSubDone();
        if (std::getenv("VSOMEIP_TRACY")) {
            FrameMark;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            app_->clear_all_handler();
            app_->stop();
            return;
        }
        // vsomeip shutdown can block indefinitely; benchmark is done.
        std::quick_exit(failed_ ? 1 : 0);
    }

    Options opt_;
    std::shared_ptr<vsomeip::application> app_;
    Reassembler reassembler_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool registered_{false};
    bool available_{false};
    bool done_{false};
    bool failed_{false};
    std::uint32_t last_seq_{0};
    std::uint64_t gap_count_{0};
    std::vector<double> latencies_us_;
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
    EventClient client(std::move(opt));
    return client.Run();
}
