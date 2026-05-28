#include "pch.h"
#include <CppUnitTest.h>
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include "../Test1/MidiAssignmentWindow.h"
// PerfTest_LedRefresh.cpp
// Measures the cost of the unconditional ListView_SetItem LED refresh loop
// that fires every 16ms on the MIDI Assignment Window timer.







#pragma comment(lib, "comctl32.lib")

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// ---------------------------------------------------------------------------
// Helpers – replicate only the decision logic from RefreshInputLeds / RefreshOutputLeds
// without calling real ListView APIs so we can measure pure CPU cost of the
// loop body (the part that runs on every timer tick regardless of state change).
// ---------------------------------------------------------------------------

constexpr DWORD kActivityWindowMs = 120;

// Simulates the per-slot decision made inside RefreshInputLeds
static int ComputeLedImageIndex(DWORD lastMsg, DWORD now)
{
    return (lastMsg != 0 && (now - lastMsg) < kActivityWindowMs) ? 1 : 0;
}

// Simulates one full LED-refresh pass over N slots (the expensive loop body)
static void SimulateRefreshLeds(const std::vector<DWORD>& lastMsgTimes, DWORD now,
                                std::vector<int>& outImages)
{
    int n = static_cast<int>(lastMsgTimes.size());
    outImages.resize(n);
    for (int i = 0; i < n; ++i)
        outImages[i] = ComputeLedImageIndex(lastMsgTimes[i], now);
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
namespace AhlbornBridgePerfTests
{
    TEST_CLASS(LedRefreshPerf)
    {
        static const int kSlots = 16;           // realistic: up to 16 assigned devices
        static const int kIterations = 62500;   // 62500 × 16ms = ~1000 s of simulated ticks
        std::vector<DWORD> m_lastMsgTimes;
        std::vector<int>   m_outImages;

    public:
        TEST_METHOD_INITIALIZE(Setup)
        {
            m_lastMsgTimes.resize(kSlots, 0);
            // Half the slots have recent activity (within window), half are cold
            DWORD now = GetTickCount();
            for (int i = 0; i < kSlots / 2; ++i)
                m_lastMsgTimes[i] = now - 50; // recent
            for (int i = kSlots / 2; i < kSlots; ++i)
                m_lastMsgTimes[i] = now - 500; // stale
        }

        // Baseline: current behaviour – always compute image index for every slot
        TEST_METHOD(Baseline_UnconditionalRefresh)
        {
            DWORD now = GetTickCount();
            for (int iter = 0; iter < kIterations; ++iter)
            {
                SimulateRefreshLeds(m_lastMsgTimes, now, m_outImages);
                now += 16; // advance simulated clock by one 16ms tick
            }
            // Access result to prevent dead-code elimination
            Logger::WriteMessage(("Last image[0]=" + std::to_string(m_outImages[0])).c_str());
        }

        // Candidate: skip the ListView update when the computed state did not change
        TEST_METHOD(Candidate_DirtyFlagRefresh)
        {
            std::vector<int> prevImages(kSlots, -1); // -1 = never set
            DWORD now = GetTickCount();
            for (int iter = 0; iter < kIterations; ++iter)
            {
                SimulateRefreshLeds(m_lastMsgTimes, now, m_outImages);
                int updates = 0;
                for (int i = 0; i < kSlots; ++i)
                {
                    if (m_outImages[i] != prevImages[i])
                    {
                        prevImages[i] = m_outImages[i];
                        ++updates; // this is where ListView_SetItem would be called
                    }
                }
                (void)updates;
                now += 16;
            }
            Logger::WriteMessage(("Last image[0]=" + std::to_string(m_outImages[0])).c_str());
        }
    };
}
