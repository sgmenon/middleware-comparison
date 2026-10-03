#pragma once

#include "tracy/Tracy.hpp"

// Plot helpers (no-op when Tracy is disabled at compile time).
#define VSOMEIP_BENCH_PLOT_LATENCY_US(name, us) TracyPlot(name, static_cast<int64_t>(us))
