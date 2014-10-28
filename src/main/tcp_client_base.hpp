#ifndef POSEIDON_TCP_CLIENT_BASE_HPP_
#define POSEIDON_TCP_CLIENT_BASE_HPP_

#include "tcp_session_base.hpp"
#include <string>
#include <boost/shared_ptr.hpp>

namespace Poseidon {

class TcpClientBase : public TcpSessionBase {
private:
	class SslImplClient;

private:
	unsigned char m_sa[28];
	unsigned m_salen;

protected:
	TcpClientBase(const std::string &ip, unsigned port);

protected:
	void onReadAvail(const void *data, std::size_t size) = 0;

protected:
	void sslConnect();
	void goResident();
};

}

#endif
