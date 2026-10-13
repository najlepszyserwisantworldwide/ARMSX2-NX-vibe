// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Horizon/HorizonThreadTuning.h"

#include "common/Console.h"
#include "common/Horizon/Horizon.h"

#include <algorithm>
#include <atomic>

namespace Horizon
{
	namespace
	{
		std::atomic<bool> s_policy_enabled{true};

		// Lowest priority (largest number) a thread is allowed to ask for
		constexpr s32 LOWEST_PRIORITY = 0x3F;
	} // namespace

	void SetBackgroundThreadPolicyEnabled(bool enabled)
	{
		s_policy_enabled.store(enabled, std::memory_order_relaxed);
	}

	bool MoveCurrentThreadOffEECore(int priority_penalty)
	{
		if (!s_policy_enabled.load(std::memory_order_relaxed))
			return false;

		u64 allowed_cores = 0;
		if (R_FAILED(svcGetInfo(&allowed_cores, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || allowed_cores == 0)
			return false;

		// EE is pinned to the lowest allowed core (see VMManager::SetEmuThreadAffinities).
		// With fewer than three cores there is nowhere better to put a helper thread.
		if (__builtin_popcountll(allowed_cores) < 3)
			return false;

		const u64 ee_core_bit = allowed_cores & (~allowed_cores + 1);
		const u64 mask = allowed_cores & ~ee_core_bit;
		const s32 preferred_core = __builtin_ctzll(mask);

		const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferred_core, static_cast<u32>(mask));
		if (R_FAILED(rc))
		{
			WARNING_LOG("Horizon: could not move helper thread off the EE core (mask 0x{:x}): {:#010x}",
				mask, static_cast<unsigned>(rc));
			return false;
		}

		if (priority_penalty > 0)
		{
			s32 priority = 0;
			if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)))
			{
				const s32 lowered = std::min(priority + priority_penalty, LOWEST_PRIORITY);
				const Result prc = svcSetThreadPriority(CUR_THREAD_HANDLE, static_cast<u32>(lowered));
				if (R_FAILED(prc))
				{
					WARNING_LOG("Horizon: could not lower helper thread priority to {}: {:#010x}", lowered,
						static_cast<unsigned>(prc));
				}
			}
		}

		return true;
	}
} // namespace Horizon
