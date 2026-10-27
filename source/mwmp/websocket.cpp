// The online mode's WebSocket and HTTP clients (mwmp/websocket.hpp)
#include "mwmp/websocket.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "components/debug/debuglog.hpp"

namespace
{
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const unsigned char* p, size_t n)
{
	std::string o;
	for (size_t i = 0; i < n; i += 3)
	{
		unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
		o += kB64[(v >> 18) & 63];
		o += kB64[(v >> 12) & 63];
		o += i + 1 < n ? kB64[(v >> 6) & 63] : '=';
		o += i + 2 < n ? kB64[v & 63] : '=';
	}
	return o;
}

std::string lowerCase(std::string s)
{
	for (char& c : s)
		if (c >= 'A' && c <= 'Z')
			c += 32;
	return s;
}

// What can go now from `out`, and what has come in appended to `in`; false: the connection is gone
bool pump(NetSocket& sock, std::string& out, std::string& in)
{
	while (!out.empty())
	{
		int n = sock.send(out.data(), (int)out.size());
		if (n < 0)
			return false;
		if (n == 0)
			break;
		out.erase(0, n);
	}
	char buf[4096];
	for (int k = 0; k < 64; k++)          // (at most 256 KB a frame: the rest waits for the next)
	{
		int n = sock.recv(buf, sizeof(buf));
		if (n < 0)
			return false;
		if (n == 0)
			break;
		in.append(buf, n);
	}
	return true;
}
}

bool NetUrl::parse(const std::string& u)
{
	size_t s = u.find("://");
	if (s == std::string::npos)
		return false;
	std::string scheme = lowerCase(u.substr(0, s));
	if (scheme == "wss" || scheme == "https")
		tls = true;
	else if (scheme == "ws" || scheme == "http")
		tls = false;
	else
		return false;
	port = tls ? 443 : 80;
	std::string rest = u.substr(s + 3);
	size_t slash = rest.find('/');
	std::string hostPort = rest.substr(0, slash);
	path = slash == std::string::npos ? "/" : rest.substr(slash);
	size_t colon = hostPort.rfind(':');
	if (colon != std::string::npos)
	{
		port = atoi(hostPort.c_str() + colon + 1);
		hostPort.resize(colon);
	}
	host = hostPort;
	return !host.empty() && port > 0 && port < 65536;
}

std::string NetUrl::origin() const
{
	char p[16] = "";
	if (port != (tls ? 443 : 80))
		snprintf(p, sizeof(p), ":%d", port);
	return std::string(tls ? "https://" : "http://") + host + p;
}

// ---- WebSocket

void WebSocket::fail(const std::string& why)
{
	if (st != FAILED)
		logf("ws: %s", why.c_str());
	err = why;
	st = FAILED;
	sock.close();
}

bool WebSocket::open(const std::string& u, const std::string& subprotocol)
{
	sock.close();
	in.clear();
	out.clear();
	partial.clear();
	received.clear();
	upgraded = false;
	err.clear();
	proto = subprotocol;
	if (!url.parse(u))
	{
		fail("not a ws:// or wss:// address: " + u);
		return false;
	}
	if (!sock.connect(url.host, url.port, url.tls))
	{
		fail(sock.error());
		return false;
	}
	unsigned char nonce[16];
	for (unsigned char& c : nonce)
		c = (unsigned char)(rand() & 0xFF);
	key = base64(nonce, sizeof(nonce));
	char hostLine[300];
	if (url.port == (url.tls ? 443 : 80))
		snprintf(hostLine, sizeof(hostLine), "%s", url.host.c_str());
	else
		snprintf(hostLine, sizeof(hostLine), "%s:%d", url.host.c_str(), url.port);
	out = "GET " + url.path + " HTTP/1.1\r\nHost: " + hostLine + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\nOrigin: " + url.origin() + "\r\n"
		"User-Agent: mw3ds\r\n" + (proto.empty() ? "" : "Sec-WebSocket-Protocol: " + proto + "\r\n") + "\r\n";
	st = CONNECTING;
	return true;
}

// The server's 101 answer; what follows it is already frames
bool WebSocket::readHandshake()
{
	size_t end = in.find("\r\n\r\n");
	if (end == std::string::npos)
	{
		if (in.size() > 16384)
			fail("the server's answer is too long");
		return false;
	}
	std::string head = in.substr(0, end);
	in.erase(0, end + 4);
	int code = 0;
	sscanf(head.c_str(), "HTTP/%*s %d", &code);
	if (code != 101)
	{
		size_t eol = head.find("\r\n");
		fail("the server refused the connection (" + head.substr(0, eol) + ")");
		return false;
	}
	upgraded = true;
	st = OPEN;
	logf("ws: open %s%s", url.host.c_str(), url.path.c_str());
	return true;
}

void WebSocket::frame(int opcode, const std::string& payload)
{
	std::string f;
	f.push_back((char)(0x80 | opcode));
	size_t n = payload.size();
	if (n < 126)
		f.push_back((char)(0x80 | n));
	else if (n < 65536)
	{
		f.push_back((char)(0x80 | 126));
		f.push_back((char)(n >> 8));
		f.push_back((char)(n & 0xFF));
	}
	else
	{
		f.push_back((char)(0x80 | 127));
		for (int i = 7; i >= 0; i--)
			f.push_back((char)((unsigned long long)n >> (8 * i)));
	}
	unsigned char mask[4];
	for (unsigned char& m : mask)
		m = (unsigned char)(rand() & 0xFF);
	f.append((const char*)mask, 4);
	size_t at = f.size();
	f += payload;
	for (size_t i = 0; i < n; i++)
		f[at + i] ^= mask[i & 3];
	out += f;
}

bool WebSocket::readFrames()
{
	for (;;)
	{
		if (in.size() < 2)
			return true;
		const unsigned char* p = (const unsigned char*)in.data();
		bool fin = p[0] & 0x80;
		int opcode = p[0] & 0x0F;
		bool masked = p[1] & 0x80;
		unsigned long long n = p[1] & 0x7F;
		size_t head = 2;
		if (n == 126)
		{
			if (in.size() < 4)
				return true;
			n = (p[2] << 8) | p[3];
			head = 4;
		}
		else if (n == 127)
		{
			if (in.size() < 10)
				return true;
			n = 0;
			for (int i = 0; i < 8; i++)
				n = (n << 8) | p[2 + i];
			head = 10;
		}
		if (n > (16u << 20))
		{
			fail("a message too big to hold");
			return false;
		}
		size_t maskAt = head;
		if (masked)
			head += 4;
		if (in.size() < head + n)
			return true;
		std::string payload = in.substr(head, (size_t)n);
		if (masked)
			for (size_t i = 0; i < payload.size(); i++)
				payload[i] ^= in[maskAt + (i & 3)];
		in.erase(0, head + (size_t)n);
		switch (opcode)
		{
		case 0x0:                           // continuation
			partial += payload;
			if (fin)
			{
				received.push_back({ partialText, partial });
				partial.clear();
			}
			break;
		case 0x1: case 0x2:
			if (fin)
				received.push_back({ opcode == 0x1, payload });
			else
			{
				partial = payload;
				partialText = opcode == 0x1;
			}
			break;
		case 0x8:                           // close: the server's code and reason
		{
			int code = payload.size() >= 2 ? ((unsigned char)payload[0] << 8 | (unsigned char)payload[1]) : 1005;
			std::string reason = payload.size() > 2 ? payload.substr(2) : "";
			frame(0x8, payload.substr(0, 2));
			pump(sock, out, in);
			char why[200];
			snprintf(why, sizeof(why), "the server closed the connection (%d%s%s)", code, reason.empty() ? "" : " ",
				reason.c_str());
			fail(why);
			return false;
		}
		case 0x9:                           // ping: pong it
			frame(0xA, payload);
			break;
		default:                            // pong, reserved
			break;
		}
	}
}

WebSocket::State WebSocket::update()
{
	if (st != CONNECTING && st != OPEN)
		return st;
	NetSocket::State s = sock.poll();
	if (s == NetSocket::FAILED || s == NetSocket::CLOSED)
	{
		fail(sock.error().empty() ? "the connection was lost" : sock.error());
		return st;
	}
	if (s != NetSocket::OPEN)
		return st;
	if (!pump(sock, out, in))
	{
		// what came in before the close still counts (the server's last words: a SessionDisconnect)
		if (upgraded)
			readFrames();
		fail(sock.error().empty() ? "the connection was lost" : sock.error());
		return st;
	}
	if (!upgraded && !readHandshake())
		return st;
	readFrames();
	if (st == OPEN && !out.empty())
		pump(sock, out, in);
	return st;
}

void WebSocket::sendText(const std::string& text)
{
	if (st == OPEN || st == CONNECTING)
		frame(0x1, text);
}

void WebSocket::sendBinary(const std::string& data)
{
	if (st == OPEN || st == CONNECTING)
		frame(0x2, data);
}

bool WebSocket::next(Message& m)
{
	if (received.empty())
		return false;
	m = std::move(received.front());
	received.pop_front();
	return true;
}

void WebSocket::close()
{
	if (st == OPEN)
	{
		frame(0x8, std::string("\x03\xE8", 2));    // 1000: normal
		pump(sock, out, in);
	}
	sock.close();
	st = CLOSED;
}

// ---- HTTP

bool HttpRequest::start(const std::string& method, const std::string& u, const std::vector<std::string>& headers,
	const std::string& reqBody)
{
	sock.close();
	in.clear();
	body.clear();
	head.clear();
	err.clear();
	status = 0;
	sent = 0;
	if (!url.parse(u))
	{
		err = "not an http:// or https:// address: " + u;
		st = FAILED;
		return false;
	}
	if (!sock.connect(url.host, url.port, url.tls))
	{
		err = sock.error();
		st = FAILED;
		return false;
	}
	request = method + " " + url.path + " HTTP/1.1\r\nHost: " + url.host + "\r\nConnection: close\r\nUser-Agent: mw3ds\r\n";
	if (sendOrigin)
		request += "Origin: " + url.origin() + "\r\n";
	bool accept = false;
	for (auto& h : headers)
	{
		request += h + "\r\n";
		accept |= lowerCase(h.substr(0, 7)) == "accept:";
	}
	if (!accept)
		request += "Accept: application/json\r\n";
	if (!reqBody.empty() || method == "POST" || method == "PUT" || method == "PATCH")
	{
		char len[64];
		snprintf(len, sizeof(len), "Content-Type: application/json\r\nContent-Length: %u\r\n", (unsigned)reqBody.size());
		request += len;
	}
	request += "\r\n" + reqBody;
	st = RUNNING;
	return true;
}

// The whole answer is in (the server closed after it, Connection: close): status, headers, body (de-chunked)
bool HttpRequest::finish()
{
	size_t end = in.find("\r\n\r\n");
	if (end == std::string::npos)
	{
		err = "no answer from the server";
		st = FAILED;
		return false;
	}
	head = in.substr(0, end);
	std::string lower = lowerCase(head);
	sscanf(lower.c_str(), "http/%*s %d", &status);
	std::string raw = in.substr(end + 4);
	in.clear();
	in.shrink_to_fit();                  // (a download's whole file: not held twice over)
	if (lower.find("transfer-encoding: chunked") != std::string::npos)
	{
		size_t p = 0;
		while (p < raw.size())
		{
			size_t eol = raw.find("\r\n", p);
			if (eol == std::string::npos)
				break;
			unsigned long n = strtoul(raw.substr(p, eol - p).c_str(), nullptr, 16);
			if (n == 0)
				break;
			body += raw.substr(eol + 2, n);
			p = eol + 2 + n + 2;
		}
	}
	else
		body = std::move(raw);
	st = DONE;
	return true;
}

std::string HttpRequest::header(const char* name) const
{
	std::string lower = lowerCase(head), key = "\r\n" + lowerCase(name) + ":";
	size_t at = lower.find(key);
	if (at == std::string::npos)
		return "";
	size_t from = at + key.size(), to = head.find("\r\n", from);
	std::string v = head.substr(from, to == std::string::npos ? std::string::npos : to - from);
	while (!v.empty() && (v[0] == ' ' || v[0] == '\t'))
		v.erase(0, 1);
	while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
		v.pop_back();
	return v;
}

HttpRequest::State HttpRequest::update()
{
	if (st != RUNNING)
		return st;
	NetSocket::State s = sock.poll();
	if (s == NetSocket::FAILED)
	{
		// a close straight after the answer reads as a failed socket: what came is the answer
		if (!in.empty())
			finish();
		else
		{
			err = sock.error();
			st = FAILED;
		}
		return st;
	}
	if (s != NetSocket::OPEN)
		return st;
	while (sent < request.size())
	{
		int n = sock.send(request.data() + sent, (int)(request.size() - sent));
		if (n < 0)
		{
			err = sock.error();
			st = FAILED;
			return st;
		}
		if (n == 0)
			return st;
		sent += n;
	}
	char buf[4096];
	for (;;)
	{
		int n = sock.recv(buf, sizeof(buf));
		if (n < 0)
		{
			finish();
			sock.close();
			return st;
		}
		if (n == 0)
			break;
		in.append(buf, n);
	}
	// (a server that keeps the connection open: done once Content-Length is all here)
	size_t end = in.find("\r\n\r\n");
	if (end != std::string::npos)
	{
		std::string head = lowerCase(in.substr(0, end));
		size_t cl = head.find("content-length:");
		if (cl != std::string::npos && in.size() >= end + 4 + strtoul(head.c_str() + cl + 15, nullptr, 10))
		{
			finish();
			sock.close();
		}
	}
	return st;
}
