// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Horizon/HorizonClock.h"

#include "common/Console.h"
#include "common/Horizon/Horizon.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

namespace Horizon::Clock
{
	namespace
	{
		constexpr u32 MHZ = 1'000'000;

		// Highest clocks Nintendo's own performance tables use. Anything above this needs
		// allow_over_stock and a sysmodule that patched the clock tables.
		constexpr u32 CPU_STOCK_MAX_MHZ = 1785;
		constexpr u32 GPU_STOCK_MAX_MHZ = 921;
		constexpr u32 EMC_STOCK_MAX_MHZ = 1600;

		// Sanity ceilings for allow_over_stock. These are not a promise that the hardware
		// will take such a clock: the system rejects values it has no table entry for.
		constexpr u32 CPU_SANITY_MAX_MHZ = 2600;
		constexpr u32 GPU_SANITY_MAX_MHZ = 1600;
		constexpr u32 EMC_SANITY_MAX_MHZ = 3000;

		constexpr auto WATCHDOG_INTERVAL = std::chrono::seconds(2);
		// After this many consecutive re-applies that did not stick we assume another
		// sysmodule owns the clock and stop interfering.
		constexpr unsigned MAX_REAPPLY_ATTEMPTS = 5;

		struct Module
		{
			const char* name;
			PcvModule pcv_module; // firmware < 8.0.0
			PcvModuleId module_id; // firmware >= 8.0.0 (clkrst)
			u32 stock_max_mhz;
			u32 sanity_max_mhz;

			u32 requested_hz = 0;
			u32 original_hz = 0;
			u32 applied_hz = 0;
			unsigned mismatch_streak = 0;
			bool gave_up = false;
			bool active = false;
			ClkrstSession session{};
			bool session_open = false;
		};

		std::mutex s_mutex;
		std::condition_variable s_cv;
		Config s_config;
		bool s_services_ready = false;
		bool s_services_failed = false;
		bool s_applied = false;
		bool s_stop = false;
		std::thread s_watchdog;

		std::array<Module, 3> s_modules{{
			{"CPU", PcvModule_CpuBus, PcvModuleId_CpuBus, CPU_STOCK_MAX_MHZ, CPU_SANITY_MAX_MHZ},
			{"GPU", PcvModule_GPU, PcvModuleId_GPU, GPU_STOCK_MAX_MHZ, GPU_SANITY_MAX_MHZ},
			{"EMC", PcvModule_EMC, PcvModuleId_EMC, EMC_STOCK_MAX_MHZ, EMC_SANITY_MAX_MHZ},
		}};

		bool UseClkrst()
		{
			static const bool s_use_clkrst = hosversionAtLeast(8, 0, 0);
			return s_use_clkrst;
		}

		bool InitServicesLocked()
		{
			if (s_services_ready)
				return true;
			if (s_services_failed)
				return false;

			const Result rc = UseClkrst() ? clkrstInitialize() : pcvInitialize();
			if (R_FAILED(rc))
			{
				ERROR_LOG("Horizon clock: failed to initialise the {} service: {:#010x}. Clock control disabled",
					UseClkrst() ? "clkrst" : "pcv", static_cast<unsigned>(rc));
				s_services_failed = true;
				return false;
			}

			s_services_ready = true;
			return true;
		}

		bool OpenSessionLocked(Module& m)
		{
			if (!UseClkrst() || m.session_open)
				return true;

			const Result rc = clkrstOpenSession(&m.session, m.module_id, 3);
			if (R_FAILED(rc))
			{
				WARNING_LOG("Horizon clock: cannot open a {} clock session: {:#010x}", m.name,
					static_cast<unsigned>(rc));
				return false;
			}

			m.session_open = true;
			return true;
		}

		Result GetHz(Module& m, u32* hz)
		{
			return UseClkrst() ? clkrstGetClockRate(&m.session, hz) : pcvGetClockRate(m.pcv_module, hz);
		}

		Result SetHz(Module& m, u32 hz)
		{
			return UseClkrst() ? clkrstSetClockRate(&m.session, hz) : pcvSetClockRate(m.pcv_module, hz);
		}

		void CheckModulesLocked()
		{
			for (Module& m : s_modules)
			{
				if (!m.active || m.gave_up)
					continue;

				u32 current = 0;
				if (R_FAILED(GetHz(m, &current)))
					continue;

				if (current == m.applied_hz)
				{
					m.mismatch_streak = 0;
					continue;
				}

				if (++m.mismatch_streak > MAX_REAPPLY_ATTEMPTS)
				{
					m.gave_up = true;
					WARNING_LOG("Horizon clock: {} keeps being reset to {} MHz by something else "
								"(sys-clk/HorizonOC?). Not interfering any further",
						m.name, current / MHZ);
					continue;
				}

				INFO_LOG("Horizon clock: {} is at {} MHz, re-applying {} MHz", m.name, current / MHZ,
					m.requested_hz / MHZ);
				if (R_SUCCEEDED(SetHz(m, m.requested_hz)))
				{
					u32 applied = 0;
					if (R_SUCCEEDED(GetHz(m, &applied)))
						m.applied_hz = applied;
				}
			}
		}

		void WatchdogLoop()
		{
			std::unique_lock lock(s_mutex);
			while (!s_stop)
			{
				s_cv.wait_for(lock, WATCHDOG_INTERVAL, []() { return s_stop; });
				if (s_stop)
					break;
				CheckModulesLocked();
			}
		}
	} // namespace

	void Configure(const Config& config)
	{
		std::lock_guard lock(s_mutex);
		s_config = config;
	}

	void Acquire()
	{
		std::lock_guard lock(s_mutex);
		if (!s_config.enabled || s_applied)
			return;
		if (!InitServicesLocked())
			return;

		const std::array<u32, 3> requested_mhz = {s_config.cpu_mhz, s_config.gpu_mhz, s_config.emc_mhz};

		bool any = false;
		for (std::size_t i = 0; i < s_modules.size(); i++)
		{
			Module& m = s_modules[i];
			m.requested_hz = 0;
			m.original_hz = 0;
			m.applied_hz = 0;
			m.mismatch_streak = 0;
			m.gave_up = false;
			m.active = false;

			const u32 wanted_mhz = requested_mhz[i];
			if (wanted_mhz == 0)
				continue;

			const u32 ceiling_mhz = s_config.allow_over_stock ? m.sanity_max_mhz : m.stock_max_mhz;
			const u32 target_mhz = std::min(wanted_mhz, ceiling_mhz);
			if (target_mhz != wanted_mhz)
			{
				WARNING_LOG("Horizon clock: {} request of {} MHz is above the {} MHz limit, using {} MHz",
					m.name, wanted_mhz, ceiling_mhz, target_mhz);
			}

			if (!OpenSessionLocked(m))
				continue;

			u32 original_hz = 0;
			Result rc = GetHz(m, &original_hz);
			if (R_FAILED(rc))
			{
				WARNING_LOG("Horizon clock: cannot read the {} clock: {:#010x}", m.name, static_cast<unsigned>(rc));
				continue;
			}

			const u32 target_hz = target_mhz * MHZ;
			rc = SetHz(m, target_hz);
			if (R_FAILED(rc))
			{
				WARNING_LOG("Horizon clock: system rejected {} MHz for {}: {:#010x}", target_mhz, m.name,
					static_cast<unsigned>(rc));
				continue;
			}

			// The system may round to a table entry, so remember what actually stuck
			u32 applied_hz = 0;
			if (R_FAILED(GetHz(m, &applied_hz)))
				applied_hz = target_hz;

			m.requested_hz = target_hz;
			m.original_hz = original_hz;
			m.applied_hz = applied_hz;
			m.active = true;
			any = true;
			INFO_LOG("Horizon clock: {} {} -> {} MHz", m.name, original_hz / MHZ, applied_hz / MHZ);
		}

		s_applied = any;
		if (any)
		{
			s_stop = false;
			s_watchdog = std::thread(WatchdogLoop);
		}
	}

	void Release()
	{
		std::thread watchdog;
		{
			std::lock_guard lock(s_mutex);
			if (!s_applied)
				return;
			s_stop = true;
			watchdog = std::move(s_watchdog);
		}
		s_cv.notify_all();
		if (watchdog.joinable())
			watchdog.join();

		std::lock_guard lock(s_mutex);
		for (Module& m : s_modules)
		{
			if (!m.active)
				continue;
			m.active = false;

			u32 current = 0;
			if (R_FAILED(GetHz(m, &current)))
				continue;

			// Only undo our own change. If something else moved the clock, it owns it now.
			if (current != m.applied_hz)
			{
				INFO_LOG("Horizon clock: {} is at {} MHz, not ours to restore", m.name, current / MHZ);
				continue;
			}

			if (R_SUCCEEDED(SetHz(m, m.original_hz)))
				INFO_LOG("Horizon clock: {} restored to {} MHz", m.name, m.original_hz / MHZ);
		}
		s_applied = false;
	}

	void Shutdown()
	{
		Release();

		std::lock_guard lock(s_mutex);
		for (Module& m : s_modules)
		{
			if (m.session_open)
			{
				clkrstCloseSession(&m.session);
				m.session_open = false;
			}
		}

		if (s_services_ready)
		{
			if (UseClkrst())
				clkrstExit();
			else
				pcvExit();
			s_services_ready = false;
		}
	}
} // namespace Horizon::Clock
