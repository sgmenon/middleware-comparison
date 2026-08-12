#pragma once

#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mw_bench {

// Minimal helper to spawn a child process and kill it on destruction.
class ChildProcess {
   public:
    ChildProcess(std::string path, std::vector<std::string> args, std::vector<std::string> env_extra = {})
        : path_(std::move(path)), args_(std::move(args)), env_extra_(std::move(env_extra)) {
        pid_ = fork();
        if (pid_ < 0) {
            throw std::runtime_error("fork failed");
        }
        if (pid_ == 0) {
            for (auto& kv : env_extra_) {
                ::putenv(kv.data());
            }
            std::vector<char*> argv;
            argv.push_back(path_.data());
            for (auto& a : args_) {
                argv.push_back(a.data());
            }
            argv.push_back(nullptr);
            ::execv(path_.c_str(), argv.data());
            _exit(127);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    ~ChildProcess() {
        if (pid_ > 0) {
            ::kill(pid_, SIGTERM);
            int status = 0;
            ::waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    pid_t pid() const { return pid_; }

   private:
    std::string path_;
    std::vector<std::string> args_;
    std::vector<std::string> env_extra_;
    pid_t pid_{-1};
};

inline std::string WriteTempFile(const std::string& prefix, const std::string& contents) {
    auto dir = std::filesystem::temp_directory_path();
    auto path = dir / (prefix + "." + std::to_string(::getpid()) + ".tmp");
    std::ofstream out(path);
    out << contents;
    out.close();
    return path.string();
}

}  // namespace mw_bench
