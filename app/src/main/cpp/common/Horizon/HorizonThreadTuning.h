// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

namespace Horizon
{
	/// Switches the helper-thread policy on or off. On by default; the VM manager turns it off
	/// together with thread pinning so a user who disabled pinning gets no affinity changes at all.
	void SetBackgroundThreadPolicyEnabled(bool enabled);

	/// Call at the top of a helper thread (audio, disc IO, decompression, loaders).
	///
	/// The EE thread is pinned to the lowest core the process may use. This moves the calling
	/// thread to the remaining allowed cores so it cannot take time from EE, and optionally lowers
	/// its priority by priority_penalty steps (a larger number is a lower priority).
	///
	/// Does nothing, and returns false, when the policy is off or the process has fewer than three
	/// cores to spread over. Returns true if the thread was moved.
	bool MoveCurrentThreadOffEECore(int priority_penalty = 0);
} // namespace Horizon
