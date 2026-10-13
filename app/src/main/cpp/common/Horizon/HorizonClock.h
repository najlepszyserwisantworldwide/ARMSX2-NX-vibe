// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstdint>

namespace Horizon
{
	/// Direct control of CPU/GPU/EMC clocks while a game is running.
	///
	/// Off by default. When enabled, the requested clocks are applied by Acquire() and the
	/// clocks found at that moment are put back by Release(). Requests above the stock
	/// Nintendo maximums are only honoured when allow_over_stock is set.
	///
	/// This does not bypass the sysmodule that owns the clock tables: values the system
	/// rejects are logged and left alone, and if another sysmodule (sys-clk, HorizonOC)
	/// keeps changing a clock back we stop fighting it after a few attempts.
	namespace Clock
	{
		struct Config
		{
			bool enabled = false;
			// MHz, 0 = leave this module untouched
			std::uint32_t cpu_mhz = 0;
			std::uint32_t gpu_mhz = 0;
			std::uint32_t emc_mhz = 0;
			bool allow_over_stock = false;
		};

		/// Stores the configuration. Call before Acquire(); safe to call again after a
		/// settings change (takes effect on the next Acquire()).
		void Configure(const Config& config);

		/// Applies the configured clocks. No-op when disabled or already applied.
		void Acquire();

		/// Restores the clocks that were active before Acquire(). No-op if not applied.
		void Release();

		/// Release() plus closing the clock services. Call once on exit.
		void Shutdown();
	} // namespace Clock
} // namespace Horizon
