#pragma once

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace ns_white_backing {

constexpr uint32_t modifier_none = 0;
constexpr uint32_t modifier_shift = 1u << 0;
constexpr uint32_t modifier_ctrl = 1u << 1;
constexpr uint32_t modifier_alt = 1u << 2;
constexpr uint32_t modifier_win = 1u << 3;
constexpr uint32_t supported_modifier_mask = modifier_shift | modifier_ctrl | modifier_alt;

inline uint32_t sanitize_modifiers(uint32_t modifiers) {
	return modifiers & supported_modifier_mask;
}

inline std::string trim_copy(std::string value) {
	const size_t begin = value.find_first_not_of(" \t\r\n");
	const size_t end = value.find_last_not_of(" \t\r\n");
	return begin == std::string::npos ? std::string() : value.substr(begin, end - begin + 1);
}

inline std::string upper_copy(std::string value) {
	for (char &character : value)
		character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	return value;
}

inline bool try_parse_virtual_key(const std::string &raw_value, uint32_t &out) {
	const std::string value = upper_copy(trim_copy(raw_value));
	if (value.empty())
		return false;
	if (value == "NONE" || value == "DISABLED") {
		out = 0;
		return true;
	}
	if (value.size() == 1 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= '0' && value[0] <= '9'))) {
		out = static_cast<uint32_t>(value[0]);
		return true;
	}

	bool numeric = true;
	for (const char character : value) {
		if (!std::isdigit(static_cast<unsigned char>(character))) {
			numeric = false;
			break;
		}
	}
	if (numeric) {
		char *end = nullptr;
		const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
		if (end != nullptr && *end == '\0' && parsed <= 0xFFu) {
			out = static_cast<uint32_t>(parsed);
			return true;
		}
		return false;
	}

	if (value.size() >= 2 && value[0] == 'F') {
		char *end = nullptr;
		const unsigned long function_number = std::strtoul(value.c_str() + 1, &end, 10);
		if (end != nullptr && *end == '\0' && function_number >= 1 && function_number <= 24) {
			out = VK_F1 + static_cast<uint32_t>(function_number - 1);
			return true;
		}
	}

	struct named_key {
		const char *name;
		uint32_t value;
	};
	static constexpr named_key keys[] = {
		{ "SPACE", VK_SPACE }, { "TAB", VK_TAB }, { "ENTER", VK_RETURN }, { "RETURN", VK_RETURN },
		{ "ESC", VK_ESCAPE }, { "ESCAPE", VK_ESCAPE }, { "BACKSPACE", VK_BACK }, { "DELETE", VK_DELETE },
		{ "INSERT", VK_INSERT }, { "HOME", VK_HOME }, { "END", VK_END }, { "PAGEUP", VK_PRIOR },
		{ "PGUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT }, { "PGDN", VK_NEXT }, { "UP", VK_UP },
		{ "DOWN", VK_DOWN }, { "LEFT", VK_LEFT }, { "RIGHT", VK_RIGHT },
	};
	for (const named_key &key : keys) {
		if (value == key.name) {
			out = key.value;
			return true;
		}
	}
	return false;
}

inline bool try_parse_modifiers(const std::string &raw_value, uint32_t &out) {
	const std::string value = upper_copy(trim_copy(raw_value));
	if (value.empty() || value == "NONE" || value == "0") {
		out = modifier_none;
		return true;
	}

	out = modifier_none;
	std::string token;
	auto consume_token = [&out, &token]() -> bool {
		if (token.empty())
			return true;
		if (token == "SHIFT")
			out |= modifier_shift;
		else if (token == "CTRL" || token == "CONTROL")
			out |= modifier_ctrl;
		else if (token == "ALT")
			out |= modifier_alt;
		else if (token == "WIN" || token == "WINDOWS")
			out |= modifier_win;
		else
			return false;
		token.clear();
		return true;
	};

	for (const char character : value) {
		if (character == '+' || character == ',' || character == '|' || std::isspace(static_cast<unsigned char>(character))) {
			if (!consume_token())
				return false;
		} else {
			token += character;
		}
	}
	return consume_token();
}

template <typename IsDown>
inline bool hotkey_down(uint32_t key, uint32_t modifiers, IsDown &&is_down) {
	modifiers = sanitize_modifiers(modifiers);
	if (key == 0 || !is_down(key))
		return false;
	const bool shift_down = is_down(VK_SHIFT) || is_down(VK_LSHIFT) || is_down(VK_RSHIFT);
	const bool ctrl_down = is_down(VK_CONTROL) || is_down(VK_LCONTROL) || is_down(VK_RCONTROL);
	const bool alt_down = is_down(VK_MENU) || is_down(VK_LMENU) || is_down(VK_RMENU);
	return ((modifiers & modifier_shift) == 0 || shift_down) &&
		((modifiers & modifier_ctrl) == 0 || ctrl_down) &&
		((modifiers & modifier_alt) == 0 || alt_down);
}

inline bool hotkey_down(uint32_t key, uint32_t modifiers) {
	return hotkey_down(key, modifiers, [](uint32_t candidate) {
		return (GetAsyncKeyState(static_cast<int>(candidate)) & 0x8000) != 0;
	});
}

} // namespace ns_white_backing
