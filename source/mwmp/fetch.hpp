#pragma once

// One GET moved on a frame at a time, its redirects followed (GitHub's release files are served from another host):
// the self-updater's and the data download's (FractumSeraph/mw3ds)
#include <cstdio>
#include <string>
#include <vector>

#include "mwmp/websocket.hpp"

struct Fetch
{
	HttpRequest req;
	std::string url;
	int redirects = 0;
	bool failed = false, done = false;
	std::string err;
	void start(const std::string& u, bool api)
	{
		url = u;
		redirects = 0;
		failed = done = false;
		err.clear();
		begin(api);
	}
	void begin(bool api)
	{
		req.sendOrigin = false;
		std::vector<std::string> h;
		if (api)
		{
			h.push_back("Accept: application/vnd.github+json");
			h.push_back("X-GitHub-Api-Version: 2022-11-28");
		}
		else
			h.push_back("Accept: application/octet-stream");
		if (!req.start("GET", url, h, ""))
		{
			failed = true;
			err = req.error();
		}
	}
	void update()
	{
		if (failed || done)
			return;
		HttpRequest::State s = req.update();
		if (s == HttpRequest::FAILED)
		{
			failed = true;
			err = req.error();
		}
		else if (s == HttpRequest::DONE)
		{
			int st = req.status;
			if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && redirects < 5)
			{
				std::string loc = req.header("location");
				if (loc.empty())
				{
					failed = true;
					err = "a redirect without a Location";
					return;
				}
				if (loc[0] == '/')
				{
					NetUrl u;
					u.parse(url);
					loc = (u.tls ? "https://" : "http://") + u.host
						+ (u.port != (u.tls ? 443 : 80) ? ":" + std::to_string(u.port) : "") + loc;
				}
				url = loc;
				redirects++;
				begin(false);
			}
			else if (st != 200)
			{
				failed = true;
				char b[48];
				snprintf(b, sizeof(b), "the server answered %d", st);
				err = b;
			}
			else
				done = true;
		}
	}
};
