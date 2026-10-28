#pragma once

// The online mode's WebSocket (RFC 6455) and HTTP/1.1 clients over NetSocket (FractumSeraph/mw3ds), each moved on once
// a frame by update(): never waiting on the network. ws:// and http:// in plain TCP, wss:// and https:// in TLS.
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "mwmp/netsocket.hpp"

// ws[s]://host[:port]/path or http[s]://... split up (false: not one of those)
struct NetUrl
{
	bool tls = false;
	std::string host, path = "/";
	int port = 80;
	bool parse(const std::string& url);
	std::string origin() const;          // http[s]://host[:port], for the Origin header
};

class WebSocket
{
public:
	enum State { CLOSED, CONNECTING, OPEN, FAILED };
	struct Message { bool text; std::string data; };
	// Starts the connection and the upgrade; subprotocol: Sec-WebSocket-Protocol (openmw-web's "omw-mp.2")
	bool open(const std::string& url, const std::string& subprotocol);
	State update();                                       // the network moved on (reads, writes, the handshake)
	State state() const { return st; }
	void sendText(const std::string& text);
	void sendBinary(const std::string& data);
	bool next(Message& out);                              // a received message, oldest first
	void close();                                         // a close frame, then the connection
	const std::string& error() const { return err; }
	size_t queued() const { return out.size(); }          // bytes waiting to go (a stalled link grows it)
private:
	NetSocket sock;
	NetUrl url;
	std::string proto, key, in, out;
	std::string partial;                                  // a fragmented message so far
	bool partialText = false, upgraded = false;
	State st = CLOSED;
	std::string err;
	std::deque<Message> received;
	void fail(const std::string& why);
	void frame(int opcode, const std::string& payload);
	bool readHandshake();
	bool readFrames();
};

class HttpRequest
{
public:
	enum State { IDLE, RUNNING, DONE, FAILED };
	// method, url, extra header lines ("Authorization: Bearer x"), body (sent as application/json when not empty)
	bool start(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
		const std::string& body);
	State update();
	State state() const { return st; }
	void cancel() { sock.close(); st = IDLE; }           // given up: the connection closed
	int status = 0;
	std::string body;                                     // the response's, de-chunked
	std::string header(const char* name) const;          // a response header's value ("" none), name in any case
	size_t received() const { return in.size() + streamed; } // bytes of the answer so far (a download's progress)
	bool sendOrigin = true;                               // (the updater's requests to GitHub go without one)
	// A 200's body (sent with a Content-Length) handed to this as it arrives instead of kept in body: a download
	// larger than memory (the game data's packs). Returning false stops the request (FAILED, error() "stopped").
	std::function<bool(const char* data, size_t n)> sink;
	const std::string& error() const { return err; }
private:
	NetSocket sock;
	NetUrl url;
	std::string request, in, err, head;
	size_t sent = 0;
	State st = IDLE;
	bool streaming = false;                               // (sink: the head read, the body going to it)
	size_t streamed = 0, length = 0;
	bool finish();
	bool stream(const char* data, size_t n);
};
