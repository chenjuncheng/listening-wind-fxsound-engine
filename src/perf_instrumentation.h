/*
 * perf_instrumentation.h
 *
 * 听风者引擎性能插桩头文件
 * 用法：#include 到 fxsound_engine_main.cpp，在 processTimer() 前后调用宏
 *
 * 输出格式（stderr，JSON Lines）：
 *   {"type":"perf","stat":"processTimer","count":1000,"avg_us":123.4,"p50_us":120.0,"p95_us":180.0,"max_us":250.0,"min_us":100.0}
 */

#ifndef PERF_INSTRUMENTATION_H
#define PERF_INSTRUMENTATION_H

#include <windows.h>
#include <psapi.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>

// ============================================================================
// Configuration
// ============================================================================
#ifndef PERF_SAMPLE_EVERY_N
#define PERF_SAMPLE_EVERY_N 1000   // 每 N 次 processTimer() 输出一次统计
#endif

#ifndef PERF_HISTOGRAM_BUCKETS
#define PERF_HISTOGRAM_BUCKETS 20  // 延迟直方图 bucket 数量
#endif

// ============================================================================
// PerfSampler: 高精度计时采样器
// ============================================================================
class PerfSampler {
public:
    struct Stats {
        int64_t count = 0;
        double avg_us = 0.0;
        double p50_us = 0.0;
        double p95_us = 0.0;
        double p99_us = 0.0;
        double max_us = 0.0;
        double min_us = 1e9;
        double stddev_us = 0.0;
    };

    PerfSampler() {
        QueryPerformanceFrequency(&freq_);
    }

    void record(int64_t elapsed_ticks) {
        samples_.push_back(elapsed_ticks);
    }

    bool shouldReport() const {
        return (int)samples_.size() >= PERF_SAMPLE_EVERY_N;
    }

    Stats computeStats() {
        Stats s;
        int n = (int)samples_.size();
        if (n == 0) return s;

        s.count = n;

        // Convert all to microseconds
        std::vector<double> us;
        us.reserve(n);
        double sum = 0.0;
        for (auto t : samples_) {
            double v = ticksToUs(t);
            us.push_back(v);
            sum += v;
            s.max_us = (std::max)(s.max_us, v);
            s.min_us = (std::min)(s.min_us, v);
        }
        s.avg_us = sum / n;

        std::sort(us.begin(), us.end());
        s.p50_us = percentile(us, 0.50);
        s.p95_us = percentile(us, 0.95);
        s.p99_us = percentile(us, 0.99);

        // Standard deviation
        double sq_sum = 0.0;
        for (double v : us) {
            double d = v - s.avg_us;
            sq_sum += d * d;
        }
        s.stddev_us = std::sqrt(sq_sum / n);

        samples_.clear();
        return s;
    }

    void printStats(const char* name, const Stats& s) {
        fprintf(stderr,
            "{\"type\":\"perf\",\"stat\":\"%s\",\"count\":%lld,"
            "\"avg_us\":%.1f,\"p50_us\":%.1f,\"p95_us\":%.1f,\"p99_us\":%.1f,"
            "\"max_us\":%.1f,\"min_us\":%.1f,\"stddev_us\":%.1f}\n",
            name, s.count, s.avg_us, s.p50_us, s.p95_us, s.p99_us, s.max_us, s.min_us, s.stddev_us);
    }

    // Print histogram buckets (log scale) for visual inspection
    void printHistogram(const char* name, const std::vector<double>& us) {
        if (us.empty()) return;
        int n = (int)us.size();
        double minV = *std::min_element(us.begin(), us.end());
        double maxV = *std::max_element(us.begin(), us.end());
        if (maxV <= minV) maxV = minV + 1.0;

        std::vector<int> buckets(PERF_HISTOGRAM_BUCKETS, 0);
        double logMin = std::log10((std::max)(minV, 1.0));
        double logMax = std::log10(maxV);
        double step = (logMax - logMin) / PERF_HISTOGRAM_BUCKETS;

        for (double v : us) {
            int idx = (int)((std::log10((std::max)(v, 1.0)) - logMin) / step);
            if (idx < 0) idx = 0;
            if (idx >= PERF_HISTOGRAM_BUCKETS) idx = PERF_HISTOGRAM_BUCKETS - 1;
            buckets[idx]++;
        }

        fprintf(stderr, "[PERF] Histogram for %s (%d samples):\n", name, n);
        for (int i = 0; i < PERF_HISTOGRAM_BUCKETS; i++) {
            double bmin = std::pow(10.0, logMin + i * step);
            double bmax = std::pow(10.0, logMin + (i + 1) * step);
            int barLen = (std::min)(buckets[i] * 50 / n, 50);
            std::string bar(barLen, '#');
            std::string spaces((std::max)(0, 50 - barLen), ' ');
            fprintf(stderr, "  %6.1f-%6.1fus |%s%s (%d)\n",
                bmin, bmax, bar.c_str(), spaces.c_str(), buckets[i]);
        }
    }

private:
    LARGE_INTEGER freq_;
    std::vector<int64_t> samples_;

    double ticksToUs(int64_t ticks) const {
        return (double)ticks * 1000000.0 / (double)freq_.QuadPart;
    }

    static double percentile(const std::vector<double>& sorted, double p) {
        int n = (int)sorted.size();
        if (n == 0) return 0.0;
        double pos = p * (n - 1);
        int idx = (int)pos;
        double frac = pos - idx;
        if (idx + 1 >= n) return sorted[idx];
        return sorted[idx] * (1.0 - frac) + sorted[idx + 1] * frac;
    }
};

// ============================================================================
// Convenience macros for single-shot timing
// ============================================================================
class PerfScopedTimer {
public:
    PerfScopedTimer(PerfSampler& sampler) : sampler_(sampler) {
        QueryPerformanceCounter(&start_);
    }
    ~PerfScopedTimer() {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        sampler_.record(end.QuadPart - start_.QuadPart);
    }
private:
    PerfSampler& sampler_;
    LARGE_INTEGER start_;
};

#define PERF_SCOPED_TIMER(sampler) PerfScopedTimer _perf_timer_##__LINE__(sampler)

// ============================================================================
// Process-level resource monitoring (Windows API)
// ============================================================================
static void printProcessStats() {
    PROCESS_MEMORY_COUNTERS pmc = {};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        FILETIME createTime, exitTime, kernelTime, userTime;
        if (GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime)) {
            ULARGE_INTEGER k, u;
            k.LowPart = kernelTime.dwLowDateTime; k.HighPart = kernelTime.dwHighDateTime;
            u.LowPart = userTime.dwLowDateTime; u.HighPart = userTime.dwHighDateTime;
            // FILETIME is in 100-nanosecond intervals
            double kernelMs = (double)k.QuadPart / 10000.0;
            double userMs = (double)u.QuadPart / 10000.0;
            fprintf(stderr,
                "{\"type\":\"perf\",\"stat\":\"process\","
                "\"working_set_mb\":%.2f,\"peak_working_set_mb\":%.2f,"
                "\"kernel_ms\":%.1f,\"user_ms\":%.1f}\n",
                (double)pmc.WorkingSetSize / (1024.0 * 1024.0),
                (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0),
                kernelMs, userMs);
        }
    }
}

#endif // PERF_INSTRUMENTATION_H
