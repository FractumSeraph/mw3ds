#pragma once

// OpenMW's Lua serialization ("LSER", components/lua/serialization.cpp FORMAT_VERSION 0): the body of every
// openmw-web multiplayer event (server/PROTOCOL.md 0x0002). A value tree here, encoded and decoded the same way: numbers
// are doubles, strings short (< 32 bytes) or long, tables key / value pairs, RefNum userdata ("o", 8 bytes) by
// content file and index; vectors are read (and kept as numbers) so a body that holds one still decodes.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct LValue
{
	enum Type { NIL, NUMBER, STRING, BOOL, TABLE, REFNUM, VEC, OTHER };
	Type type = NIL;
	double num = 0.0;                 // NUMBER, BOOL (0 / 1)
	std::string str;                  // STRING; OTHER: the userdata's type name
	std::vector<std::pair<LValue, LValue>> table;
	uint32_t refIndex = 0;            // REFNUM
	int32_t refFile = 0;
	std::vector<double> vec;          // VEC (vec2 / vec3 / vec4 / colour / transforms)

	LValue() {}
	LValue(double n) : type(NUMBER), num(n) {}
	LValue(int n) : type(NUMBER), num(n) {}
	LValue(const char* s) : type(STRING), str(s) {}
	LValue(const std::string& s) : type(STRING), str(s) {}
	static LValue boolean(bool b) { LValue v; v.type = BOOL; v.num = b ? 1.0 : 0.0; return v; }
	static LValue newTable() { LValue v; v.type = TABLE; return v; }
	static LValue refNum(uint32_t index, int32_t file) { LValue v; v.type = REFNUM; v.refIndex = index; v.refFile = file; return v; }

	// Tables: set a key (replacing it), append to the array part (1-based keys), read a string or 1-based key
	LValue& set(const std::string& key, const LValue& value);
	LValue& push(const LValue& value);
	const LValue* get(const char* key) const;
	const LValue* at(int index) const;
	int arraySize() const;            // the 1-based keys from 1 up without a gap

	bool isNil() const { return type == NIL; }
	double number(double fallback = 0.0) const { return type == NUMBER || type == BOOL ? num : fallback; }
	bool truthy() const { return type != NIL && !(type == BOOL && num == 0.0); }
	const std::string& string() const { return str; }
	// Read one field: the number, string or flag at key (the fallback when it's missing or of another type)
	double numberAt(const char* key, double fallback = 0.0) const;
	std::string stringAt(const char* key, const std::string& fallback = "") const;
	bool boolAt(const char* key, bool fallback = false) const;
};

// A whole body: the format byte, then the value. False on anything malformed (depth > 32, a cut-off string ...)
bool lserDecode(const uint8_t* data, size_t len, LValue& out);
std::string lserEncode(const LValue& value);
// For the log: a short readable form ({name="x", hp={c=10}} ...), cut at maxLen
std::string lserDescribe(const LValue& value, size_t maxLen = 300);
