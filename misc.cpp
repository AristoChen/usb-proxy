#include <math.h>
#include <cctype>

#include "misc.h"

std::string hexToAscii(std::string input) {
	std::string output = input;
	size_t pos = output.find("\\x");
	while (pos != std::string::npos) {
		std::string substr = output.substr(pos + 2, 2);

		std::istringstream iss(substr);
		iss.flags(std::ios::hex);
		int i;
		iss >> i;
		output = output.replace(pos, 4, 1, char(i));
		pos = output.find("\\x");
	}
	return output;
}

int hexToDecimal(int input) {
	int output = 0;
	int i = 0;
	while (input != 0) {
		output += (input % 10) * pow(16, i);
		input /= 10;
		i++;
	}
	return output;
}

// Parse a hex byte string into a byte vector.
// Accepts continuous ("000a0500") or space-separated ("00 0a 05 00") formats.
// Returns an empty vector on parse error.
std::vector<uint8_t> parseHexBytes(const std::string &hex_str)
{
	std::vector<uint8_t> result;
	std::string s;

	for (char c : hex_str) {
		if (!std::isspace((unsigned char)c))
			s += c;
	}

	if (s.size() % 2 != 0)
		return {};

	auto nibble = [](unsigned char c) -> uint8_t {
		return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0';
	};

	for (size_t i = 0; i < s.size(); i += 2) {
		if (!std::isxdigit((unsigned char)s[i]) ||
		    !std::isxdigit((unsigned char)s[i + 1]))
			return {};
		result.push_back((nibble((unsigned char)s[i]) << 4) |
				 nibble((unsigned char)s[i + 1]));
	}

	return result;
}
