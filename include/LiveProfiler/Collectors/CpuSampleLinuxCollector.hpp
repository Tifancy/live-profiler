#pragma once
#include <linux/perf_event.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include "BaseCollector.hpp"
#include "../Models/CpuSampleModel.hpp"
#include "../Utils/Allocators/FreeListAllocator.hpp"
#include "../Utils/Allocators/SingletonAllocator.hpp"
#include "../Utils/Platform/Linux/LinuxEpollDescriptor.hpp"
#include "../Utils/Platform/Linux/LinuxExecutableSymbolResolver.hpp"
#include "../Utils/Platform/Linux/LinuxPerfEntry.hpp"
#include "../Utils/Platform/Linux/LinuxPerfUtils.hpp"
#include "../Utils/Platform/Linux/LinuxProcessAddressLocator.hpp"
#include "../Utils/Platform/Linux/LinuxProcessUtils.hpp"

namespace LiveProfiler {
	/**
	 * Collector for collecting cpu samples on linux, based on perf_events
	 */
	class CpuSampleLinuxCollector : public BaseCollector<CpuSampleModel> {
	public:
		/** Default parameters */
		static const std::size_t DefaultMaxFreeResult = 1024;
		static const std::size_t DefaultMmapPageCount = 4;
		static const std::size_t DefaultSamplePeriod = 100000;
		static const std::size_t DefaultMaxFreePerfEntry = 1024;
		static const std::size_t DefaultMaxFreeAddressLocator = 1024;

		/** Reset the state to it's initial state */
		void reset() override {
			// clear all monitoring threads
			for (auto& pair : tidToPerfEntry_) {
				unmonitorThread(std::move(pair.second));
			}
			tidToPerfEntry_.clear();
			threads_.clear();
			// reset last threads updated time
			threadsUpdated_ = {};
			// reset enabled
			enabled_ = false;
			// the filter will remain because it's set externally
		}

		/** Enable performance data collection */
		void enable() override {
			// reset and enable all perf events, ignore any errors
			for (auto& pair : tidToPerfEntry_) {
				assert(pair.second != nullptr);
				LinuxPerfUtils::perfEventEnable(pair.second->getFd(), true);
			}
			// all newly monitored threads should call perfEventEnable
			enabled_ = true;
		}

		/** Collect performance data for the specified timeout period */
		std::vector<std::unique_ptr<CpuSampleModel>>& collect(
			std::chrono::high_resolution_clock::duration timeout) & override {
			// update the threads to monitor every specified interval
			auto now = std::chrono::high_resolution_clock::now();
			if (now - threadsUpdated_ > threadsUpdateInterval_) {
				threads_.clear();
				LinuxProcessUtils::listProcesses(threads_, filter_, true);
				updatePerfEvents();
				threadsUpdated_ = now;
			}
			// clear results
			for (auto& result : results_) {
				resultAllocator_.deallocate(std::move(result));
			}
			results_.clear();
			// poll events
			auto& events = epoll_.wait(timeout);
			for (auto& event : events) {
				// get entry by tid
				pid_t tid = static_cast<pid_t>(event.data.u64);
				auto it = tidToPerfEntry_.find(tid);
				if (it == tidToPerfEntry_.end()) {
					// thread no longer be monitored
					continue;
				}
				// check events
				if ((event.events & EPOLLIN) == EPOLLIN) {
					// take samples
					takeSamples(it->second);
				} else if ((event.events & (EPOLLERR | EPOLLHUP)) != 0) {
					// thread no longer exist
					unmonitorThread(std::move(it->second));
					tidToPerfEntry_.erase(it);
				}
			}
			return results_;
		}

		/** Disable performance data collection */
		void disable() override {
			// disable all perf events, ignore any errors
			for (auto& pair : tidToPerfEntry_) {
				assert(pair.second != nullptr);
				LinuxPerfUtils::perfEventDisable(pair.second->getFd());
			}
			// reset enabled
			enabled_ = false;
		}

		/** Set how often to take a sample, the unit is cpu clock */
		void setSamplePeriod(std::size_t samplePeriod) {
			samplePeriod_ = samplePeriod;
		}

		/** Set how many pages for mmap ring buffer,
		 * this count is not contains metadata page, and should be power of 2.
		 */
		void setMmapPageCount(std::size_t mmapPageCount) {
			mmapPageCount_ = mmapPageCount;
		}

		/** Set how often to update the list of processes */
		template <class Rep, class Period>
		void setProcessesUpdateInterval(std::chrono::duration<Rep, Period> interval) {
			threadsUpdateInterval_ = std::chrono::duration_cast<
				std::decay_t<decltype(threadsUpdateInterval_)>>(interval);
		}

		/** Use the specified function to decide which processes to monitor */
		void filterProcessBy(const std::function<bool(pid_t)>& filter) {
			filter_ = filter;
		}

		/** Use the specified process name to decide which processes to monitor */
		void filterProcessByName(const std::string& name) {
			filterProcessBy(LinuxProcessUtils::getProcessFilterByName(name));
		}

		/** Constructor */
		CpuSampleLinuxCollector() :
			results_(),
			resultAllocator_(DefaultMaxFreeResult),
			filter_(),
			threads_(),
			threadsUpdated_(),
			threadsUpdateInterval_(std::chrono::milliseconds(100)),
			tidToPerfEntry_(),
			perfEntryAllocator_(DefaultMaxFreePerfEntry),
			samplePeriod_(DefaultSamplePeriod),
			mmapPageCount_(DefaultMmapPageCount),
			enabled_(false),
			epoll_(),
			pidToAddressLocator_(),
			addressLocatorAllocator_(DefaultMaxFreeAddressLocator),
			pathAllocator_(std::make_shared<decltype(pathAllocator_)::element_type>()),
			resolverAllocator_(std::make_shared<decltype(resolverAllocator_)::element_type>()) { }

	protected:
		/** Update the threads to monitor based on `threads_` */
		void updatePerfEvents() {
			std::sort(threads_.begin(), threads_.end());
			// find out which threads newly created
			for (pid_t tid : threads_) {
				if (tidToPerfEntry_.find(tid) != tidToPerfEntry_.end()) {
					continue;
				}
				tidToPerfEntry_.emplace(tid, monitorThread(tid));
			}
			// find out which threads no longer exist and clear tidToPerfEntry_
			for (auto it = tidToPerfEntry_.begin(); it != tidToPerfEntry_.end();) {
				pid_t tid = it->first;
				if (std::binary_search(threads_.cbegin(), threads_.cend(), tid)) {
					// thread still exist
					++it;
				} else {
					// thread no longer exist
					unmonitorThread(std::move(it->second));
					it = tidToPerfEntry_.erase(it);
				}
			}
			// find out which processes no longer exist and clear pidToAddressLocator_
			for (auto it = pidToAddressLocator_.begin(); it != pidToAddressLocator_.end();) {
				pid_t pid = it->first;
				if (std::binary_search(threads_.cbegin(), threads_.cend(), pid)) {
					// process still exist
					++it;
				} else {
					// process no longer exist
					it = pidToAddressLocator_.erase(it);
				}
			}
		}

		/** Monitor specified thread, will not access tidToPerfEntry_ */
		std::unique_ptr<LinuxPerfEntry> monitorThread(pid_t tid) {
			// open perf event
			auto entry = perfEntryAllocator_.allocate();
			entry->setPid(tid);
			LinuxPerfUtils::monitorSample(
				entry, samplePeriod_, mmapPageCount_,
				PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_CALLCHAIN);
			// enable events if collecting
			if (enabled_) {
				LinuxPerfUtils::perfEventEnable(entry->getFd(), true);
			}
			// register to epoll, use edge trigger and associated data is tid
			epoll_.add(entry->getFd(), EPOLLIN | EPOLLET, static_cast<std::uint64_t>(tid));
			return entry;
		}

		/** Unmonitor specified thread, will not access tidToPerfEntry_ */
		void unmonitorThread(std::unique_ptr<LinuxPerfEntry>&& entry) {
			assert(entry != nullptr);
			// unregister from epoll
			epoll_.del(entry->getFd());
			// disable events
			LinuxPerfUtils::perfEventDisable(entry->getFd());
			// return instance to allocator
			perfEntryAllocator_.deallocate(std::move(entry));
		}

		/** Take samples for executing instruction (actually is the next instruction) */
		void takeSamples(std::unique_ptr<LinuxPerfEntry>& entry) {
			assert(entry != nullptr);
			auto& records = entry->getRecords(1);
			for (auto* record : records) {
				// check if the record is sample
				if (record->type != PERF_RECORD_SAMPLE) {
					continue;
				}
				auto* data = reinterpret_cast<const CpuSampleRawData*>(record);
				// setup model data
				auto result = resultAllocator_.allocate();
				result->setIp(data->ip);
				result->setPid(data->pid);
				result->setTid(data->tid);
				// resolve symbol names
				pid_t pid = data->pid;
				auto addressLocatorIt = pidToAddressLocator_.find(pid);
				if (addressLocatorIt == pidToAddressLocator_.end()) {
					auto pair = pidToAddressLocator_.emplace(pid,
						addressLocatorAllocator_.allocate(pid, pathAllocator_));
					addressLocatorIt = pair.first;
				}
				auto pathAndOffset = addressLocatorIt->second->locate(data->ip, false);
				if (pathAndOffset.first != nullptr) {
					auto resolver = resolverAllocator_->allocate(std::move(pathAndOffset.first));
					result->setSymbolName(resolver->resolve(pathAndOffset.second));
				}
				auto& callChainIps = result->getCallChainIps();
				auto& callChainSymbolNames = result->getCallChainSymbolNames();
				for (std::size_t i = 0; i < data->nr; ++i) {
					if (data->nr > 100) {
						std::cout << data->header.type << " " << PERF_RECORD_SAMPLE << std::endl;
						std::cout << data->header.size << " " << data->header.misc << std::endl;
						std::cout << data->pid << " " << data->tid << std::endl;
						std::cout << data->ip << std::endl;
						std::cout << i << " " << data->nr << std::endl;
						abort();
					}
					auto callChainIp = data->ips[i];
					// TODO: resolve kernel call
					// TODO: resolve libc call
					// TODO: callchain is not backtrace
					// if (callChainIp == data->ip) {
					// 	continue; // don't record top ip (i = 1) and bottom ip
					// }
					callChainIps.emplace_back(callChainIp);
					pathAndOffset = addressLocatorIt->second->locate(callChainIp, false);
					if (pathAndOffset.first == nullptr) {
						callChainSymbolNames.emplace_back(nullptr);
					} else {
						auto resolver = resolverAllocator_->allocate(std::move(pathAndOffset.first));
						callChainSymbolNames.emplace_back(resolver->resolve(pathAndOffset.second));
					}
				}
				// append model data
				results_.emplace_back(std::move(result));
			}
			// all records handled, update read offset
			entry->updateReadOffset();
		}

		/** See man perf_events, section PERF_RECORD_SAMPLE */
		struct CpuSampleRawData {
			::perf_event_header header;
			std::uint64_t ip;
			std::uint32_t pid;
			std::uint32_t tid;
			std::uint64_t nr;
			std::uint64_t ips[];
		};

	protected:
		std::vector<std::unique_ptr<CpuSampleModel>> results_;
		FreeListAllocator<CpuSampleModel> resultAllocator_;

		std::function<bool(pid_t)> filter_;
		std::vector<pid_t> threads_;
		std::chrono::high_resolution_clock::time_point threadsUpdated_;
		std::chrono::high_resolution_clock::duration threadsUpdateInterval_;

		std::unordered_map<pid_t, std::unique_ptr<LinuxPerfEntry>> tidToPerfEntry_;
		FreeListAllocator<LinuxPerfEntry> perfEntryAllocator_;

		std::size_t samplePeriod_;
		std::size_t mmapPageCount_;
		bool enabled_;

		LinuxEpollDescriptor epoll_;

		std::unordered_map<pid_t, std::unique_ptr<LinuxProcessAddressLocator>> pidToAddressLocator_;
		FreeListAllocator<LinuxProcessAddressLocator> addressLocatorAllocator_;
		std::shared_ptr<SingletonAllocator<std::string, std::string>> pathAllocator_;
		std::shared_ptr<SingletonAllocator<
			std::shared_ptr<std::string>, LinuxExecutableSymbolResolver>> resolverAllocator_;
	};
}

