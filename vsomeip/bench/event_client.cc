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
        if (opt_.rpc_calls > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            available_ = true;
            cv_.notify_all();
            return;
        }
        std::set<vsomeip::eventgroup_t> groups{vsomeip_bench::kEventgroupId};
        const auto reliability =
            opt_.transport == "tcp" ? vsomeip::reliability_type_e::RT_RELIABLE : vsomeip::reliability_type_e::RT_UNRELIABLE;
        app_->request_event(vsomeip_bench::kServiceId, vsomeip_bench::kInstanceId, vsomeip_bench::kEventId, groups,
                            vsomeip::event_type_e::ET_EVENT, reliability);
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
        if (msg->get_message_type() == vsomeip::message_type_e::MT_RESPONSE && msg->get_method() == vsomeip_bench::kEchoMethodId) {
            OnEchoResponse(msg);
            return;
        }
        if (msg->get_message_type() != vsomeip::message_type_e::MT_NOTIFICATION) {
            return;
        }
        if (msg->get_service() != vsomeip_bench::kServiceId || msg->get_instance() != vsomeip_bench::kInstanceId ||
            msg->get_method() != vsomeip_bench::kEventId) {
            return;
        }
        const std::uint64_t recv_ns = vsomeip_bench::NowNs();
        auto pl = msg->get_payload();
        const auto frame = tracker_.ingest(pl->get_data(), pl->get_length());
        if (!frame) {
            return;
        }
        const std::uint32_t seq = frame->seq;
        const double us = static_cast<double>(recv_ns - frame->send_ns) / 1000.0;

        std::lock_guard<std::mutex> lock(mutex_);
        if (last_seq_ != 0 && seq > last_seq_ + 1) {
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

    void OnEchoResponse(const std::shared_ptr<vsomeip::message>& msg) {
        const std::uint64_t recv_ns = vsomeip_bench::NowNs();
        auto pl = msg->get_payload();
        std::uint64_t send_ns = 0;
        std::uint32_t seq = 0;
        if (!ReadStamp(pl->get_data(), pl->get_length(), &send_ns, &seq)) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (seq != rpc_pending_seq_) {
            return;
        }
        rpc_rtt_us_ = static_cast<double>(recv_ns - send_ns) / 1000.0;
        rpc_done_ = true;
        cv_.notify_all();
    }

    /** Calls the echo method rpc_calls times (one in flight) and prints each round trip. */
    void RunEchoCalls() {
        if (opt_.transport == "udp" && opt_.size > opt_.max_datagram) {
            std::fprintf(stderr, "rpc: --size=%zu exceeds one datagram (%zu); echo method is not fragmented\n", opt_.size,
                         opt_.max_datagram);
            failed_ = true;
            return;
        }
        std::vector<double> rtts;
        std::vector<std::uint8_t> buf(opt_.size);
        for (int i = 0; i < opt_.rpc_calls; ++i) {
            const auto seq = static_cast<std::uint32_t>(i);
            vsomeip_bench::StampPayload(buf.data(), buf.size(), vsomeip_bench::NowNs(), seq);
            auto req = vsomeip::runtime::get()->create_request(opt_.transport == "tcp");
            req->set_service(vsomeip_bench::kServiceId);
            req->set_instance(vsomeip_bench::kInstanceId);
            req->set_method(vsomeip_bench::kEchoMethodId);
            auto payload = vsomeip::runtime::get()->create_payload();
            payload->set_data(buf);
            req->set_payload(payload);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                rpc_pending_seq_ = seq;
                rpc_done_ = false;
            }
            {
                ZoneScopedN("vsomeip_bench.rpc_send");
                app_->send(req);
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::seconds(2), [this] { return rpc_done_; })) {
                std::printf("rpc %s size=%zu call=%d timeout\n", opt_.stack.c_str(), opt_.size, i);
                continue;
            }
            std::printf("rpc %s size=%zu call=%d rtt_us=%.3f\n", opt_.stack.c_str(), opt_.size, i, rpc_rtt_us_);
            rtts.push_back(rpc_rtt_us_);
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::duration<double>(1.0 / opt_.rate_hz));
        }
        std::printf("rpc_summary %s size=%zu ok=%zu/%d mean_us=%.3f p50_us=%.3f min_us=%.3f max_us=%.3f\n", opt_.stack.c_str(), opt_.size,
                    rtts.size(), opt_.rpc_calls, vsomeip_bench::MeanUs(rtts), vsomeip_bench::PercentileUs(rtts, 0.5),
                    rtts.empty() ? 0.0 : *std::min_element(rtts.begin(), rtts.end()),
                    rtts.empty() ? 0.0 : *std::max_element(rtts.begin(), rtts.end()));
        std::fflush(stdout);
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
        }
        if (opt_.rpc_calls > 0) {
            RunEchoCalls();
            vsomeip_bench::TouchBenchSubDone();
            if (std::getenv("VSOMEIP_TRACY")) {
                FrameMark;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::quick_exit(failed_ ? 1 : 0);
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
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
    vsomeip_bench::FrameTracker tracker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool registered_{false};
    bool available_{false};
    bool done_{false};
    bool failed_{false};
    std::uint32_t rpc_pending_seq_{0};
    bool rpc_done_{false};
    double rpc_rtt_us_{0};
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
