// OpenMW's Lua serialization for the online mode (mwmp/lser.hpp)
#include "mwmp/lser.hpp"

#include <cstdio>
#include <cstring>

namespace
{
const uint8_t kFormatVersion = 0;
enum : uint8_t
{
	T_NUMBER = 0x0, T_LONG_STRING = 0x1, T_BOOLEAN = 0x2, T_TABLE_START = 0x3, T_TABLE_END = 0x4,
	T_VEC2 = 0x10, T_VEC3 = 0x11, T_TRANSFORM_M = 0x12, T_TRANSFORM_Q = 0x13, T_VEC4 = 0x14, T_COLOR = 0x15,
};
const uint8_t kShortString = 0x20, kCustomFull = 0x40, kCustomCompact = 0x80;

// (the console and the PC are both little-endian, as the format is)
template <typename T>
bool take(const uint8_t*& p, const uint8_t* end, T& v)
{
	if ((size_t)(end - p) < sizeof(T))
		return false;
	memcpy(&v, p, sizeof(T));
	p += sizeof(T);
	return true;
}

bool decodeValue(const uint8_t*& p, const uint8_t* end, LValue& out, int depth)
{
	if (p >= end || depth > 32)
		return false;
	uint8_t type = *p++;
	if (type & (kCustomCompact | kCustomFull))
	{
		size_t nameLen, dataLen;
		if (type & kCustomCompact)
		{
			nameLen = (type & 7) + 1;
			dataLen = (type >> 3) & 15;
		}
		else
		{
			nameLen = (type & 63) + 1;
			uint32_t n;
			if (!take(p, end, n))
				return false;
			dataLen = n;
		}
		if ((size_t)(end - p) < nameLen + dataLen)
			return false;
		std::string name((const char*)p, nameLen);
		p += nameLen;
		if (name == "o" && dataLen == 8)
		{
			out = LValue::refNum(0, 0);
			memcpy(&out.refIndex, p, 4);
			memcpy(&out.refFile, p + 4, 4);
		}
		else
		{
			out = LValue();
			out.type = LValue::OTHER;
			out.str = name;
		}
		p += dataLen;
		return true;
	}
	if (type & kShortString)
	{
		size_t n = type & 0x1F;
		if ((size_t)(end - p) < n)
			return false;
		out = LValue(std::string((const char*)p, n));
		p += n;
		return true;
	}
	switch (type)
	{
	case T_NUMBER:
	{
		double d;
		if (!take(p, end, d))
			return false;
		out = LValue(d);
		return true;
	}
	case T_BOOLEAN:
	{
		uint8_t b;
		if (!take(p, end, b))
			return false;
		out = LValue::boolean(b != 0);
		return true;
	}
	case T_LONG_STRING:
	{
		uint32_t n;
		if (!take(p, end, n) || (size_t)(end - p) < n)
			return false;
		out = LValue(std::string((const char*)p, n));
		p += n;
		return true;
	}
	case T_TABLE_START:
	{
		out = LValue::newTable();
		while (p < end && *p != T_TABLE_END)
		{
			LValue k, v;
			if (!decodeValue(p, end, k, depth + 1) || !decodeValue(p, end, v, depth + 1))
				return false;
			out.table.emplace_back(std::move(k), std::move(v));
		}
		if (p >= end)
			return false;
		p++;
		return true;
	}
	case T_VEC2: case T_VEC3: case T_VEC4: case T_TRANSFORM_M: case T_TRANSFORM_Q:
	{
		int n = type == T_VEC2 ? 2 : type == T_VEC3 ? 3 : type == T_TRANSFORM_M ? 16 : 4;
		out = LValue();
		out.type = LValue::VEC;
		for (int i = 0; i < n; i++)
		{
			double d;
			if (!take(p, end, d))
				return false;
			out.vec.push_back(d);
		}
		return true;
	}
	case T_COLOR:
	{
		out = LValue();
		out.type = LValue::VEC;
		for (int i = 0; i < 4; i++)
		{
			float f;
			if (!take(p, end, f))
				return false;
			out.vec.push_back(f);
		}
		return true;
	}
	default:
		return false;
	}
}

void put(std::string& out, const void* p, size_t n)
{
	out.append((const char*)p, n);
}

void encodeValue(const LValue& v, std::string& out)
{
	switch (v.type)
	{
	case LValue::NUMBER:
	{
		out.push_back((char)T_NUMBER);
		put(out, &v.num, 8);
		break;
	}
	case LValue::BOOL:
		out.push_back((char)T_BOOLEAN);
		out.push_back(v.num != 0.0 ? 1 : 0);
		break;
	case LValue::STRING:
		if (v.str.size() < 32)
			out.push_back((char)(kShortString | v.str.size()));
		else
		{
			out.push_back((char)T_LONG_STRING);
			uint32_t n = (uint32_t)v.str.size();
			put(out, &n, 4);
		}
		out += v.str;
		break;
	case LValue::TABLE:
		out.push_back((char)T_TABLE_START);
		for (auto& kv : v.table)
		{
			if (kv.second.type == LValue::NIL || kv.first.type == LValue::NIL)
				continue;                     // (a nil field is an omitted key, PROTOCOL.md's convention)
			encodeValue(kv.first, out);
			encodeValue(kv.second, out);
		}
		out.push_back((char)T_TABLE_END);
		break;
	case LValue::REFNUM:
		out.push_back((char)(kCustomCompact | (8 << 3) | 0));     // "o", 8 bytes
		out.push_back('o');
		put(out, &v.refIndex, 4);
		put(out, &v.refFile, 4);
		break;
	default:
		// nil and what we never send: a lone nil has no encoding; as a table value it is left out above
		out.push_back((char)T_BOOLEAN);
		out.push_back(0);
		break;
	}
}

void describe(const LValue& v, std::string& out, size_t maxLen)
{
	if (out.size() > maxLen)
		return;
	char buf[48];
	switch (v.type)
	{
	case LValue::NUMBER:
		snprintf(buf, sizeof(buf), "%g", v.num);
		out += buf;
		break;
	case LValue::BOOL: out += v.num != 0.0 ? "true" : "false"; break;
	case LValue::STRING: out += "\"" + v.str.substr(0, 60) + "\""; break;
	case LValue::REFNUM:
		snprintf(buf, sizeof(buf), "ref(%u,%d)", (unsigned)v.refIndex, (int)v.refFile);
		out += buf;
		break;
	case LValue::TABLE:
		out += "{";
		for (size_t i = 0; i < v.table.size() && out.size() <= maxLen; i++)
		{
			if (i)
				out += ", ";
			if (v.table[i].first.type == LValue::STRING)
				out += v.table[i].first.str;
			else
			{
				out += "[";
				describe(v.table[i].first, out, maxLen);
				out += "]";
			}
			out += "=";
			describe(v.table[i].second, out, maxLen);
		}
		out += "}";
		break;
	case LValue::VEC: out += "vec"; break;
	case LValue::OTHER: out += "<" + v.str + ">"; break;
	default: out += "nil"; break;
	}
}
}

LValue& LValue::set(const std::string& key, const LValue& value)
{
	type = TABLE;
	for (auto& kv : table)
		if (kv.first.type == STRING && kv.first.str == key)
		{
			kv.second = value;
			return kv.second;
		}
	table.emplace_back(LValue(key), value);
	return table.back().second;
}

LValue& LValue::push(const LValue& value)
{
	type = TABLE;
	table.emplace_back(LValue(arraySize() + 1), value);
	return table.back().second;
}

const LValue* LValue::get(const char* key) const
{
	if (type != TABLE)
		return nullptr;
	for (auto& kv : table)
		if (kv.first.type == STRING && kv.first.str == key)
			return &kv.second;
	return nullptr;
}

const LValue* LValue::at(int index) const
{
	if (type != TABLE)
		return nullptr;
	for (auto& kv : table)
		if (kv.first.type == NUMBER && kv.first.num == (double)index)
			return &kv.second;
	return nullptr;
}

int LValue::arraySize() const
{
	int n = 0;
	while (at(n + 1))
		n++;
	return n;
}

double LValue::numberAt(const char* key, double fallback) const
{
	const LValue* v = get(key);
	return v && v->type == NUMBER ? v->num : fallback;
}

std::string LValue::stringAt(const char* key, const std::string& fallback) const
{
	const LValue* v = get(key);
	return v && v->type == STRING ? v->str : fallback;
}

bool LValue::boolAt(const char* key, bool fallback) const
{
	const LValue* v = get(key);
	return v && v->type == BOOL ? v->num != 0.0 : fallback;
}

bool lserDecode(const uint8_t* data, size_t len, LValue& out)
{
	out = LValue();
	if (len == 0)
		return true;                         // (an empty body is nil)
	if (data[0] != kFormatVersion)
		return false;
	const uint8_t* p = data + 1;
	const uint8_t* end = data + len;
	return decodeValue(p, end, out, 0) && p == end;
}

std::string lserEncode(const LValue& value)
{
	std::string out;
	if (value.type == LValue::NIL)
		return out;
	out.push_back((char)kFormatVersion);
	encodeValue(value, out);
	return out;
}

std::string lserDescribe(const LValue& value, size_t maxLen)
{
	std::string out;
	describe(value, out, maxLen);
	if (out.size() > maxLen)
		out = out.substr(0, maxLen) + "...";
	return out;
}
