// 这个文件是 Poseidon 服务器应用程序框架的一部分。
// Copyleft 2014 - 2016, LH_Mouse. All wrongs reserved.

#include "../precompiled.hpp"
#include "dns_daemon.hpp"
#include <netdb.h>
#include <unistd.h>
#include "../log.hpp"
#include "../atomic.hpp"
#include "../exception.hpp"
#include "../thread.hpp"
#include "../mutex.hpp"
#include "../condition_variable.hpp"
#include "../job_promise.hpp"
#include "../sock_addr.hpp"
#include "../ip_port.hpp"
#include "../profiler.hpp"

namespace Poseidon {

namespace {
	SockAddr real_dns_look_up(const std::string &host_raw, unsigned port_raw){
		std::string host;
		if(!host_raw.empty() && (host_raw.begin()[0] == '[') && (host_raw.end()[-1] == ']')){
			host.assign(host_raw.begin() + 1, host_raw.end() - 1);
		} else {
			host.assign(host_raw.begin(), host_raw.end());
		}
		char port[16];
		std::sprintf(port, "%u", port_raw);

		::addrinfo *res;
		const int gai_code = ::getaddrinfo(host.c_str(), port, NULLPTR, &res);
		if(gai_code != 0){
			const AUTO(err_msg, ::gai_strerror(gai_code));
			LOG_POSEIDON_DEBUG("DNS lookup failure: host = ", host, ", port = ", port, ", gai_code = ", gai_code, ", err_msg = ", err_msg);
			DEBUG_THROW(Exception, SharedNts::view(err_msg));
		}

		SockAddr sock_addr;
		try {
			sock_addr = SockAddr(res->ai_addr, res->ai_addrlen);
			::freeaddrinfo(res);
		} catch(...){
			::freeaddrinfo(res);
			throw;
		}
		LOG_POSEIDON_DEBUG("DNS lookup success: host = ", host, ", port = ", port, ", result = ", get_ip_port_from_sock_addr(sock_addr));
		return sock_addr;
	}

	class QueryOperation {
	private:
		const boost::shared_ptr<JobPromise> m_promise;
		const boost::shared_ptr<SockAddr> m_sock_addr;

		const std::string m_host;
		const unsigned m_port;

	public:
		QueryOperation(boost::shared_ptr<JobPromise> promise,
			boost::shared_ptr<SockAddr> sock_addr, std::string host, unsigned port)
			: m_promise(STD_MOVE(promise))
			, m_sock_addr(STD_MOVE(sock_addr)), m_host(STD_MOVE(host)), m_port(port)
		{
		}

	public:
		void execute() const {
			if(m_promise.unique()){
				LOG_POSEIDON_DEBUG("Discarding isolated DNS query: m_host = ", m_host);
				return;
			}

			try {
				*m_sock_addr = real_dns_look_up(m_host, m_port);
				m_promise->set_success();
			} catch(Exception &e){
				LOG_POSEIDON_INFO("Exception thrown: what = ", e.what());
#ifdef POSEIDON_CXX11
				m_promise->set_exception(std::current_exception());
#else
				m_promise->set_exception(boost::copy_exception(e));
#endif
			} catch(std::exception &e){
				LOG_POSEIDON_INFO("std::exception thrown: what = ", e.what());
#ifdef POSEIDON_CXX11
				m_promise->set_exception(std::current_exception());
#else
				m_promise->set_exception(boost::copy_exception(std::runtime_error(e.what())));
#endif
			}
		}
	};

	volatile bool g_running = false;
	Thread g_thread;

	Mutex g_mutex;
	ConditionVariable g_new_operation;
	std::deque<boost::shared_ptr<QueryOperation> > g_operations;

	bool pump_one_element() NOEXCEPT {
		PROFILE_ME;

		boost::shared_ptr<QueryOperation> operation;
		{
			const Mutex::UniqueLock lock(g_mutex);
			if(!g_operations.empty()){
				operation = g_operations.front();
			}
		}
		if(!operation){
			return false;
		}

		try {
			operation->execute();
		} catch(std::exception &e){
			LOG_POSEIDON_WARNING("std::exception thrown: what = ", e.what());
		} catch(...){
			LOG_POSEIDON_WARNING("Unknown exception thrown.");
		}
		const Mutex::UniqueLock lock(g_mutex);
		g_operations.pop_front();
		return true;
	}

	void daemon_loop(){
		PROFILE_ME;

		for(;;){
			while(pump_one_element()){
				// noop
			}

			if(!atomic_load(g_running, ATOMIC_CONSUME)){
				break;
			}

			Mutex::UniqueLock lock(g_mutex);
			g_new_operation.timed_wait(lock, 100);
		}
	}

	void thread_proc(){
		PROFILE_ME;
		LOG_POSEIDON_INFO("DNS daemon started.");

		daemon_loop();

		LOG_POSEIDON_INFO("DNS daemon stopped.");
	}
}

void DnsDaemon::start(){
	if(atomic_exchange(g_running, true, ATOMIC_ACQ_REL) != false){
		LOG_POSEIDON_FATAL("Only one daemon is allowed at the same time.");
		std::abort();
	}
	LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "Starting DNS daemon...");

	Thread(thread_proc, "   D").swap(g_thread);
}
void DnsDaemon::stop(){
	if(atomic_exchange(g_running, false, ATOMIC_ACQ_REL) == false){
		return;
	}
	LOG_POSEIDON(Logger::SP_MAJOR | Logger::LV_INFO, "Stopping DNS daemon...");

	if(g_thread.joinable()){
		g_thread.join();
	}
	g_operations.clear();
}

SockAddr DnsDaemon::look_up(const std::string &host, unsigned port){
	PROFILE_ME;

	return real_dns_look_up(host, port);
}

boost::shared_ptr<const JobPromise> DnsDaemon::enqueue_for_looking_up(boost::shared_ptr<SockAddr> sock_addr, std::string host, unsigned port){
	PROFILE_ME;

	AUTO(promise, boost::make_shared<JobPromise>());
	{
		const Mutex::UniqueLock lock(g_mutex);
		g_operations.push_back(boost::make_shared<QueryOperation>(
			promise, STD_MOVE(sock_addr), STD_MOVE(host), port));
		g_new_operation.signal();
	}
	return STD_MOVE_IDN(promise);
}

}
