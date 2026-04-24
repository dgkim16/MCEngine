#include "MCEngine.h"
#include <fstream>
#include <future>


void MCEngine::ReadBackGpuTimer(float dt) {
    UINT64* timestamps = nullptr;
    CD3DX12_RANGE readRange(0, sizeof(UINT64) * FrameResource::GpuTimerCount);

    if (SUCCEEDED(mCurrFrameResource->GpuTimestampReadback->Map(0, &readRange, reinterpret_cast<void**>(&timestamps))))
    {
        UINT64 frequency = 0;
        ThrowIfFailed(mCommandQueue->GetTimestampFrequency(&frequency));
        double totalMs = 0.0;
        for (int i = 0; i < mCurrFrameResource->GpuTimerCount - 1; i++) {
            UINT64 delta = timestamps[i + 1] - timestamps[i];
            mCurrFrameResource->GpuFrameMs[i] = (double)delta * 1000.0 / (double)frequency;
            totalMs += mCurrFrameResource->GpuFrameMs[i];
        }
        mCurrFrameResource->totalGpuFrameMs = totalMs;
        mCurrFrameResource->GpuTimestampReadback->Unmap(0, nullptr);
    }
}

/*
* How FrameProfiler looks like in MCEngine.h
struct FrameProfiler {
    bool        recording = false;
    float       timeAccum = 0.0f;
    static constexpr float kDuration = 5.0f;
    std::vector<double> cpuSamples;
    std::vector<double> gpuSamples;
} mProfiler;
*/

// saves recorded ms to text file
void MCEngine::TickProfiler(float dt)
{
    if (!mProfiler.recording)
        return;
    mProfiler.cpuSamples.push_back(dt * 1000.0);
    mProfiler.gpuSamples.push_back(mCurrFrameResource->totalGpuFrameMs);

    for (size_t i = 0; i < mCurrFrameResource->GpuFrameMs.size(); ++i)
        mProfiler.gpuStageSamples[i].push_back(mCurrFrameResource->GpuFrameMs[i]);

    mProfiler.timeAccum += dt;

    if (mProfiler.timeAccum < FrameProfiler::kDuration)
        return;

    mProfiler.recording = false;

    auto cpuSamples = mProfiler.cpuSamples;
    auto gpuSamples = mProfiler.gpuSamples;
    auto gpuStageSamples = mProfiler.gpuStageSamples;

    std::thread([cpuSamples, gpuSamples, gpuStageSamples]() mutable
        {
            CreateDirectoryA("ProfileResults", nullptr);

            SYSTEMTIME st;
            GetLocalTime(&st);
            char filename[128];
            snprintf(filename, sizeof(filename),
                "ProfileResults\\profile_%04d%02d%02d_%02d%02d%02d.txt",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);

            std::ofstream f(filename);
            if (!f.is_open())
            {
                std::cout << "[Profiler] Failed to open " << filename << "\n";
                return;
            }

            auto printStats = [&f](const char* label, const std::vector<double>& v)
                {
                    if (v.empty())
                        return;

                    std::vector<double> sorted = v;
                    std::sort(sorted.begin(), sorted.end());

                    const size_t n = sorted.size();
                    double lo = sorted.front();
                    double hi = sorted.back();

                    double sum = 0.0;
                    for (double x : sorted)
                        sum += x;
                    double mean = sum / static_cast<double>(n);

                    double q1 = 0.0;
                    double q3 = 0.0;

                    size_t half = n / 2;
                    {
                        auto b1 = sorted.begin();
                        size_t len1 = half;
                        if (len1 > 0)
                        {
                            if (len1 % 2 == 0)
                                q1 = (*(b1 + len1 / 2 - 1) + *(b1 + len1 / 2)) * 0.5;
                            else
                                q1 = *(b1 + len1 / 2);
                        }
                        else
                        {
                            q1 = sorted.front();
                        }

                        auto b3 = sorted.begin() + (n % 2 == 0 ? half : half + 1);
                        size_t len3 = n - (n % 2 == 0 ? half : half + 1);
                        if (len3 > 0)
                        {
                            if (len3 % 2 == 0)
                                q3 = (*(b3 + len3 / 2 - 1) + *(b3 + len3 / 2)) * 0.5;
                            else
                                q3 = *(b3 + len3 / 2);
                        }
                        else
                        {
                            q3 = sorted.back();
                        }
                    }

                    f << std::fixed << std::setprecision(3);
                    f << "[" << label << "] "
                        << "samples=" << n
                        << "  lo=" << lo
                        << "  hi=" << hi
                        << "  mean=" << mean
                        << "  Q1=" << q1
                        << "  Q3=" << q3
                        << "\n";

                    f << "[" << label << " values]";
                    for (double x : v)
                        f << " " << x;
                    f << "\n";
                };

            static const char* kGpuStageNames[FrameResource::GpuTimerCount - 1] =
            {
                "begin -> after scene color",
                "after scene color -> depth debug",
                "depth debug -> msaa resolve",
                "msaa resolve -> force alpha",
                "force alpha -> blurs",
                "blurs -> sobel",
                "sobel -> change RT/depth to back buffer",
                "change RT/depth to back buffer -> imgui",
                "imgui -> present"
            };
            f << "\n=== 5s Frame Time Profile ===\n";
            printStats("CPU ms", cpuSamples);
            printStats("GPU ms", gpuSamples);

            for (size_t i = 0; i < gpuStageSamples.size(); ++i)
                printStats(kGpuStageNames[i], gpuStageSamples[i]);

            f << "==============================\n\n";

            std::cout << "[Profiler] Written to " << filename << "\n";
        }).detach();
}