#pragma once

// A TCP connection for the online mode (FractumSeraph/mw3ds), with TLS when asked, driven without blocking the frame:
// connect() starts it, poll() moves the connection and the TLS handshake on, send() / recv() take what they can now.
// The console build runs it on libctru's sockets and devkitPro's Mbed TLS (the roots in mwmp/tlsroots.hpp); the PC test
// build (tools/test/native) on Winsock without TLS, for a server on the same machine.
#include <memory>
#include <string>

bool netInit();            // the console's socket service (once; false: no network). Safe to call again
void netExit();            // every connection closed, then the service (before the console's other services go)
bool netReady();

class NetSocket
{
public:
	enum State { CLOSED, CONNECTING, HANDSHAKE, OPEN, FAILED };
	NetSocket();
	~NetSocket();
	NetSocket(const NetSocket&) = delete;
	NetSocket& operator=(const NetSocket&) = delete;
	// Starts connecting (the name lookup itself waits for its answer); false: failed at once, error() says why
	bool connect(const std::string& host, int port, bool tls);
	State poll();                              // the connection and TLS handshake moved on; the state now
	State state() const;
	int send(const void* data, int len);       // bytes taken (0: try again later); < 0 failed
	int recv(void* data, int len);             // bytes read (0: nothing yet); < 0 closed or failed
	void close();
	const std::string& error() const;
	struct Impl;
private:
	std::unique_ptr<Impl> d;
};
