// The online mode's TCP and TLS connections (mwmp/netsocket.hpp)
#include "mwmp/netsocket.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "components/debug/debuglog.hpp"

#ifdef __3DS__
#include <3ds.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "mwmp/tlsroots.hpp"
#else
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif

namespace
{
bool g_ready = false;
std::set<NetSocket::Impl*> g_open;          // closed by netExit before the service goes (a close after socExit crashed)
#ifdef __3DS__
u32* g_socBuffer = nullptr;
const u32 kSocBufferSize = 0x80000;         // the SOC service's own memory (512 KB, page aligned)
bool g_ps = false;
#endif

int lastError()
{
#if !defined(__3DS__) && defined(_WIN32)
	return WSAGetLastError();
#else
	return errno;
#endif
}

bool wouldBlock(int e)
{
#if !defined(__3DS__) && defined(_WIN32)
	return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
#else
	return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS || e == EALREADY || e == EINTR;
#endif
}

void closeFd(int fd)
{
	if (fd < 0)
		return;
#if !defined(__3DS__) && defined(_WIN32)
	closesocket(fd);
#else
	::close(fd);
#endif
}
}

bool netInit()
{
	if (g_ready)
		return true;
#ifdef __3DS__
	g_socBuffer = (u32*)memalign(0x1000, kSocBufferSize);
	if (!g_socBuffer)
	{
		logf("net: no memory for the socket service");
		return false;
	}
	Result rc = socInit(g_socBuffer, kSocBufferSize);
	if (R_FAILED(rc))
	{
		logf("net: socInit failed (0x%08lx)", (unsigned long)rc);
		free(g_socBuffer);
		g_socBuffer = nullptr;
		return false;
	}
	g_ps = R_SUCCEEDED(psInit());
#elif defined(_WIN32)
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
#endif
	g_ready = true;
	logf("net: ready");
	return true;
}

bool netReady()
{
	return g_ready;
}

struct NetSocket::Impl
{
	int fd = -1;
	State st = CLOSED;
	bool tls = false;
	std::string host, error;
#ifdef __3DS__
	bool tlsReady = false;
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_x509_crt roots;
	mbedtls_ctr_drbg_context drbg;
#endif
	void fail(const std::string& why)
	{
		error = why;
		st = FAILED;
		logf("net: %s: %s", host.c_str(), why.c_str());
	}
	void shut();
};

#ifdef __3DS__
namespace
{
// The random numbers TLS needs: the PS service's (the console's own generator), else the system tick mixed (only
// when PS is missing, which a real console never is)
int entropy(void*, unsigned char* out, size_t len)
{
	if (g_ps && R_SUCCEEDED(PS_GenerateRandomBytes(out, len)))
		return 0;
	for (size_t i = 0; i < len; i++)
		out[i] = (unsigned char)((svcGetSystemTick() >> ((i % 4) * 3)) ^ (rand() & 0xFF));
	return 0;
}

int bioSend(void* ctx, const unsigned char* buf, size_t len)
{
	int fd = *(int*)ctx;
	int n = ::send(fd, buf, len, 0);
	if (n >= 0)
		return n;
	return wouldBlock(errno) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
}

int bioRecv(void* ctx, unsigned char* buf, size_t len)
{
	int fd = *(int*)ctx;
	int n = ::recv(fd, buf, len, 0);
	if (n > 0)
		return n;
	if (n == 0)
		return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
	return wouldBlock(errno) ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_NET_RECV_FAILED;
}

std::string tlsError(int ret)
{
	char buf[120];
	mbedtls_strerror(ret, buf, sizeof(buf));
	char out[160];
	snprintf(out, sizeof(out), "TLS -0x%04x %s", -ret, buf);
	return out;
}
}
#endif

void NetSocket::Impl::shut()
{
#ifdef __3DS__
	if (tlsReady)
	{
		if (st == OPEN)
			mbedtls_ssl_close_notify(&ssl);
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_x509_crt_free(&roots);
		mbedtls_ctr_drbg_free(&drbg);
		tlsReady = false;
	}
#endif
	closeFd(fd);
	fd = -1;
	g_open.erase(this);
}

NetSocket::NetSocket() : d(new Impl)
{
}

NetSocket::~NetSocket()
{
	close();
}

void NetSocket::close()
{
	d->shut();
	if (d->st != FAILED)
		d->st = CLOSED;
}

NetSocket::State NetSocket::state() const
{
	return d->st;
}

const std::string& NetSocket::error() const
{
	return d->error;
}

bool NetSocket::connect(const std::string& host, int port, bool tls)
{
	close();
	d->st = CLOSED;
	d->host = host;
	d->tls = tls;
	d->error.clear();
	if (!netInit())
	{
		d->fail("no network");
		return false;
	}
#ifndef __3DS__
	if (tls)
	{
		d->fail("TLS needs the console build (the PC test build talks ws:// / http:// only)");
		return false;
	}
#endif
	// The name lookup waits for its answer (a few hundred ms at most on a working network)
	struct addrinfo hints = {}, *res = nullptr;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	char portText[8];
	snprintf(portText, sizeof(portText), "%d", port);
	if (getaddrinfo(host.c_str(), portText, &hints, &res) != 0 || !res)
	{
		d->fail("can't find the server's address");
		return false;
	}
	struct sockaddr_in addr;
	memcpy(&addr, res->ai_addr, sizeof(addr));
	freeaddrinfo(res);
	d->fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (d->fd < 0)
	{
		d->fail("no socket");
		return false;
	}
	g_open.insert(d.get());
#if !defined(__3DS__) && defined(_WIN32)
	u_long nb = 1;
	ioctlsocket(d->fd, FIONBIO, &nb);
#else
	fcntl(d->fd, F_SETFL, fcntl(d->fd, F_GETFL, 0) | O_NONBLOCK);
#endif
	int r = ::connect(d->fd, (struct sockaddr*)&addr, sizeof(addr));
	if (r != 0 && !wouldBlock(lastError()))
	{
		d->fail("connection refused");
		d->shut();
		return false;
	}
	d->st = CONNECTING;
	return true;
}

NetSocket::State NetSocket::poll()
{
	if (d->st == CONNECTING)
	{
		// Connected once the socket can be written to. (The console's SO_ERROR reads a negative "in progress" while it
		// connects, as Fractum Halo found: only a refusal counts as failing)
#if !defined(__3DS__) && defined(_WIN32)
		fd_set w, e;
		FD_ZERO(&w);
		FD_ZERO(&e);
		FD_SET((SOCKET)d->fd, &w);
		FD_SET((SOCKET)d->fd, &e);
		timeval tv = { 0, 0 };
		if (select(0, nullptr, &w, &e, &tv) <= 0)
			return d->st;
		if (FD_ISSET((SOCKET)d->fd, &e))
		{
			d->fail("connection refused");
			d->shut();
			return d->st;
		}
#else
		struct pollfd p = { d->fd, POLLOUT, 0 };
		if (::poll(&p, 1, 0) <= 0)
			return d->st;
		if (p.revents & (POLLERR | POLLHUP))
		{
			d->fail("connection refused");
			d->shut();
			return d->st;
		}
		if (!(p.revents & POLLOUT))
			return d->st;
#endif
		if (!d->tls)
		{
			d->st = OPEN;
			logf("net: connected to %s", d->host.c_str());
			return d->st;
		}
#ifdef __3DS__
		mbedtls_ssl_init(&d->ssl);
		mbedtls_ssl_config_init(&d->conf);
		mbedtls_x509_crt_init(&d->roots);
		mbedtls_ctr_drbg_init(&d->drbg);
		d->tlsReady = true;
		static const char kPers[] = "mw3ds online";
		int ret = mbedtls_ctr_drbg_seed(&d->drbg, entropy, nullptr, (const unsigned char*)kPers, sizeof(kPers) - 1);
		if (ret == 0)
			ret = mbedtls_x509_crt_parse(&d->roots, (const unsigned char*)kMpTlsRoots, sizeof(kMpTlsRoots));
		if (ret == 0)
			ret = mbedtls_ssl_config_defaults(&d->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
				MBEDTLS_SSL_PRESET_DEFAULT);
		if (ret == 0)
		{
			mbedtls_ssl_conf_authmode(&d->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
			mbedtls_ssl_conf_ca_chain(&d->conf, &d->roots, nullptr);
			mbedtls_ssl_conf_rng(&d->conf, mbedtls_ctr_drbg_random, &d->drbg);
			ret = mbedtls_ssl_setup(&d->ssl, &d->conf);
		}
		if (ret == 0)
			ret = mbedtls_ssl_set_hostname(&d->ssl, d->host.c_str());
		if (ret != 0)
		{
			d->fail(tlsError(ret));
			d->shut();
			return d->st;
		}
		mbedtls_ssl_set_bio(&d->ssl, &d->fd, bioSend, bioRecv, nullptr);
		d->st = HANDSHAKE;
#endif
	}
#ifdef __3DS__
	if (d->st == HANDSHAKE)
	{
		int ret = mbedtls_ssl_handshake(&d->ssl);
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			return d->st;
		if (ret != 0)
		{
			uint32_t flags = mbedtls_ssl_get_verify_result(&d->ssl);
			d->fail(flags && flags != (uint32_t)-1 ? "the server's certificate isn't trusted" : tlsError(ret));
			d->shut();
			return d->st;
		}
		d->st = OPEN;
		logf("net: connected to %s (%s)", d->host.c_str(), mbedtls_ssl_get_ciphersuite(&d->ssl));
	}
#endif
	return d->st;
}

int NetSocket::send(const void* data, int len)
{
	if (d->st != OPEN)
		return -1;
	if (len <= 0)
		return 0;
#ifdef __3DS__
	if (d->tls)
	{
		int n = mbedtls_ssl_write(&d->ssl, (const unsigned char*)data, len);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
			return 0;
		if (n < 0)
		{
			d->fail(tlsError(n));
			d->shut();
			return -1;
		}
		return n;
	}
#endif
	int n = (int)::send(d->fd, (const char*)data, len, 0);
	if (n >= 0)
		return n;
	if (wouldBlock(lastError()))
		return 0;
	d->fail("connection lost");
	d->shut();
	return -1;
}

int NetSocket::recv(void* data, int len)
{
	if (d->st != OPEN)
		return -1;
#ifdef __3DS__
	if (d->tls)
	{
		int n = mbedtls_ssl_read(&d->ssl, (unsigned char*)data, len);
		if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
			return 0;
		if (n <= 0)
		{
			if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
				d->error = "the server closed the connection";
			else
				d->error = tlsError(n);
			d->st = FAILED;
			d->shut();
			return -1;
		}
		return n;
	}
#endif
	int n = (int)::recv(d->fd, (char*)data, len, 0);
	if (n > 0)
		return n;
	if (n < 0 && wouldBlock(lastError()))
		return 0;
	d->error = n == 0 ? "the server closed the connection" : "connection lost";
	d->st = FAILED;
	d->shut();
	return -1;
}

void netExit()
{
	if (!g_ready)
		return;
	while (!g_open.empty())
		(*g_open.begin())->shut();
#ifdef __3DS__
	if (g_ps)
		psExit();
	g_ps = false;
	socExit();
	free(g_socBuffer);
	g_socBuffer = nullptr;
#elif defined(_WIN32)
	WSACleanup();
#endif
	g_ready = false;
}
