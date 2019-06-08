/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

#pragma once

// cpp headers
#include <algorithm>
#include <utility>
#include <functional>
#include <vector>
// os headers
#include <Windows.h>
// wil headers
#include <wil/resource.h>
// ctl headers
#include "ctException.hpp"
#include "ctTimer.hpp"
#include "ctLocks.hpp"


namespace ctl
{
	namespace details
	{
		struct ctThreadpoolTimerCallbackInfo
		{
			std::function<void()> callback;
			FILETIME timer_expiration = {0, 0};
			unsigned long reoccuring_period = 0;

			ctThreadpoolTimerCallbackInfo() = default;
			~ctThreadpoolTimerCallbackInfo() = default;
			// non-copyable
			ctThreadpoolTimerCallbackInfo(const ctThreadpoolTimerCallbackInfo&) = delete;
			ctThreadpoolTimerCallbackInfo& operator=(const ctThreadpoolTimerCallbackInfo&) = delete;

			explicit ctThreadpoolTimerCallbackInfo(std::function<void()>&& _callback, long long _milliseconds) noexcept :
				callback(std::move(_callback))
			{
				using namespace ctTimer;
				timer_expiration = convert_msec_absolute_filetime(snap_system_time_as_msec() + _milliseconds);
			}

			explicit ctThreadpoolTimerCallbackInfo(std::function<void()>&& _callback, long long _milliseconds, unsigned long _period) noexcept :
				callback(std::move(_callback)),
				reoccuring_period(_period)
			{
				using namespace ctTimer;
				timer_expiration = convert_msec_absolute_filetime(snap_system_time_as_msec() + _milliseconds);
			}

			// supporting only move semantics
			ctThreadpoolTimerCallbackInfo(ctThreadpoolTimerCallbackInfo&& _callback_info) noexcept
			{
				callback = std::move(_callback_info.callback);
				timer_expiration = _callback_info.timer_expiration;
				reoccuring_period = _callback_info.reoccuring_period;
			}
			ctThreadpoolTimerCallbackInfo& operator=(ctThreadpoolTimerCallbackInfo&& _callback_info) noexcept
			{
				callback = std::move(_callback_info.callback);
				timer_expiration = _callback_info.timer_expiration;
				reoccuring_period = _callback_info.reoccuring_period;
				return *this;
			}

			// update FILETIME to the next time based off the reoccuring period
			void update_expiration() noexcept
			{
				// addition in hundredNs to avoid loss of precision if were to convert to milliseconds
				using namespace ctTimer;
				const auto next_timer_hundredNs = convert_filetime_hundredNs(timer_expiration) + convert_msec_hundredNs(reoccuring_period);
				timer_expiration = convert_hundredNs_absolute_filetime(next_timer_hundredNs);
			}

			void swap(ctThreadpoolTimerCallbackInfo& _in) noexcept
			{
				using std::swap;
				swap(this->callback, _in.callback);
				swap(this->timer_expiration, _in.timer_expiration);
				swap(this->reoccuring_period, _in.reoccuring_period);
			}
		};
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///
	/// ctThreadpoolTimer
	///
	/// class that encapsulates the new-to-Vista ThreadPool APIs around Timer usage
	///
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	class ctThreadpoolTimer
	{
	public:
		//
		// These c'tors can fail under low resources
		// - ctl::ctException (from the ThreadPool APIs)
		//
		explicit ctThreadpoolTimer(_In_opt_ const PTP_CALLBACK_ENVIRON _ptp_env = nullptr) noexcept :  // NOLINT
			tp_environment(_ptp_env)
		{
			if (!::InitializeCriticalSectionEx(&timer_lock, 4000, 0))
			{
				const auto gle = ::GetLastError();
				ctAlwaysFatalCondition(L"InitializeCriticalSectionEx failed unexpectedly: %u\n", gle);
			}
		}

		~ctThreadpoolTimer() noexcept
		{
			// wait for all callbacks
			::EnterCriticalSection(&timer_lock);
			// block any more items being scheduled
			exiting = true;
			::LeaveCriticalSection(&timer_lock);

			stop_all_timers();

			for (const auto& timer : tp_timers) {
				::CloseThreadpoolTimer(timer);
			}

			::DeleteCriticalSection(&timer_lock);
		}

		// non-copyable
		ctThreadpoolTimer(const ctThreadpoolTimer&) = delete;
		ctThreadpoolTimer& operator=(const ctThreadpoolTimer&) = delete;
		ctThreadpoolTimer(ctThreadpoolTimer&&) = delete;
		ctThreadpoolTimer& operator=(ctThreadpoolTimer&&) = delete;

		void schedule_singleton(std::function<void()> _function, long long _millisecond_offset)
		{
			// capture the caller's context in a lambda to be invoked in the callback
			this->insert_callback_info(
				details::ctThreadpoolTimerCallbackInfo(
					std::move(_function),
					_millisecond_offset));
		}

		void schedule_reoccuring(std::function<void()> _function, long long _millisecond_offset, unsigned long _period)
		{
			// capture the caller's context in a lambda to be invoked in the callback
			this->insert_callback_info(
				details::ctThreadpoolTimerCallbackInfo(
					std::move(_function),
					_millisecond_offset,
					_period));
		}

		void stop_all_timers() noexcept
		{
			::EnterCriticalSection(&timer_lock);
			for (const auto& timer : tp_timers) {
				::SetThreadpoolTimer(timer, nullptr, 0, 0);
			}
			::LeaveCriticalSection(&timer_lock);

			for (const auto& timer : tp_timers) {
				::WaitForThreadpoolTimerCallbacks(timer, TRUE);
			}
		}

	private:
		//
		// Private members
		//
		CRITICAL_SECTION timer_lock{};
		const PTP_CALLBACK_ENVIRON tp_environment = nullptr;  // NOLINT
		std::vector<PTP_TIMER> tp_timers;
		std::vector<details::ctThreadpoolTimerCallbackInfo> callback_objects;
		bool exiting = false;

		PTP_TIMER create_tp()
		{
			const PTP_TIMER ptp_timer = ::CreateThreadpoolTimer(ctThreadPoolTimerCallback, this, this->tp_environment);  // NOLINT
			if (!ptp_timer) {
				throw ctException(::GetLastError(), L"CreateThreadpoolTimer", L"ctl::ctThreadpoolTimer", false);
			}
			return ptp_timer;
		}

		//
		// must insert the callback info sorted based of the expected time to complete
		//
		void insert_callback_info(details::ctThreadpoolTimerCallbackInfo&& _new_request)
		{
            const ctAutoReleaseCriticalSection lock_timer(&this->timer_lock);

			if (exiting) {
				return;
			}

			// compare each callback_object to check if it contains a null function ptr
			auto unused_callback = std::find_if(
				std::begin(this->callback_objects),
				std::end(this->callback_objects),
				[](const details::ctThreadpoolTimerCallbackInfo& _info) noexcept {
					// returns if a null callback (not being used)
					return !static_cast<bool>(_info.callback);
				});

			if (unused_callback == std::end(this->callback_objects)) {
				//
				// need room in both the callback_objects && tp_timers vector for a new timer
				//
				this->callback_objects.emplace_back(std::move(_new_request));
				// ensure this is exception safe with a scope guard
				auto removeCallbackObjectOnFailure = wil::scope_exit([&]() { this->callback_objects.pop_back(); });

				PTP_TIMER temp_timer = this->create_tp();
				// ensure the timer is closed (is exception safe) with a scope guard
				auto deleteTemporaryTimerOnFailure = wil::scope_exit([&]() { ::CloseThreadpoolTimer(temp_timer); });

				// now attempt to store the new timer
				this->tp_timers.push_back(temp_timer);

				// all succeeded : dismiss the scope guards
				deleteTemporaryTimerOnFailure.release();
				removeCallbackObjectOnFailure.release();

				unused_callback = this->callback_objects.end() - 1;
			} else {
				// if we found an empty slot, swap in the user's new object
				unused_callback->swap(_new_request);
			}

			// using iterator_offect to directly express the relationship between tp_timers and callback_objects
			// - each of the vector offsets are functionally paired together
			const auto iterator_offset = unused_callback - this->callback_objects.begin();
			::SetThreadpoolTimer(
				this->tp_timers[iterator_offset],
				&this->callback_objects[iterator_offset].timer_expiration,
				this->callback_objects[iterator_offset].reoccuring_period,
				0); // specifying window length of zero for now: not a need to be less precise -> more power efficient yet
		}

		static void CALLBACK ctThreadPoolTimerCallback(
			PTP_CALLBACK_INSTANCE /*_instance*/,
			PVOID _context,
			PTP_TIMER _timer) noexcept
		{
			auto this_ptr = static_cast<ctThreadpoolTimer*>(_context);
			// save off the functor to invoke outside the lock
			std::function<void()> functor;

			// scope for the CS lock
			{
                const ctAutoReleaseCriticalSection lock_timer(&this_ptr->timer_lock);
				// find the timer that was fired to run its callback
				const auto found_timer = std::find_if(
					std::begin(this_ptr->tp_timers),
					std::end(this_ptr->tp_timers),
					[_timer](PTP_TIMER _callback_timer) noexcept
					{
						// returns if a null callback (not being used)
						return (_timer == _callback_timer);
					});
				ctFatalCondition(
					found_timer == std::end(this_ptr->tp_timers),
					L"ctThreadPoolTimer - failed to find the PTP_TIMER (%p) which initiated this timer callback (ctl::ctThreadPoolTimer %p)",
					_timer, this_ptr);

				const auto iterator_offset = found_timer - this_ptr->tp_timers.begin();
				functor = this_ptr->callback_objects[iterator_offset].callback;

				if (0 == this_ptr->callback_objects[iterator_offset].reoccuring_period)
				{
                    this_ptr->callback_objects.erase(this_ptr->callback_objects.begin() + iterator_offset);
                    ::CloseThreadpoolTimer(*(this_ptr->tp_timers.begin() + iterator_offset));
                    this_ptr->tp_timers.erase(this_ptr->tp_timers.begin() + iterator_offset);
				}
				else
				{
					// recalculate the next time this scheduled event will fire
					this_ptr->callback_objects[iterator_offset].update_expiration();
				}
			}

			// now run the user's callback outside the internal lock
			functor();
		}
	};
} // namespace
