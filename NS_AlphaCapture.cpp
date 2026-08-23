// NS_AlphaCapture - replays selected FF14 color draws into a private RGBA32F target.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define ImTextureID ImU64
#include "imgui.h"
#include "reshade.hpp"
#include "crc32_hash.hpp"
#include "NS_Hotkey.hpp"
#include "shader_rules.hpp"

#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;
using namespace reshade::api;

namespace {

constexpr const char *default_file_naming =
	"%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%-%TimeMS%";

struct shader_rule {
	uint32_t pixel = 0;
	uint32_t vertex = 0;
	uint32_t first_index = 0;
	uint32_t index_count = 0;
	int32_t vertex_offset = 0;
};

struct config {
	uint32_t capture_key = VK_F10;
	uint32_t capture_modifiers = ns_white_backing::modifier_ctrl | ns_white_backing::modifier_shift;
	uint32_t reload_key = VK_F9;
	uint32_t reload_modifiers = ns_white_backing::modifier_ctrl | ns_white_backing::modifier_shift;
	bool output_black = false;
	bool output_white = false;
	bool output_transparent = true;
	std::string file_naming = default_file_naming;
	bool auto_match = true;
	bool auto_highlight = true;
	bool lens_only = false;
	bool lens_capture = true;
	uint32_t lens_pixel_shader_hash = 3361469263u;
	uint32_t lens_first_index = 6448u;
	uint32_t lens_index_count = 276u;
};

struct rgba_image {
	UINT width = 0;
	UINT height = 0;
	std::vector<uint8_t> pixels;
};

struct rgba32f_image {
	UINT width = 0;
	UINT height = 0;
	std::vector<float> pixels;
};

struct shader_hashes {
	uint32_t pixel = 0;
	uint32_t vertex = 0;
};

struct shader_candidate {
	shader_rule rule;
	uint32_t draw_count = 0;
	uint64_t render_target = 0;
};

struct nonindexed_candidate {
	uint32_t pixel = 0;
	uint32_t vertex = 0;
	uint32_t vertex_count = 0;
	uint32_t instance_count = 0;
	uint32_t first_vertex = 0;
	uint32_t first_instance = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t draw_count = 0;
};

struct command_state {
	uint64_t pixel_pipeline = 0;
	uint64_t vertex_pipeline = 0;
};

struct mesh_signature {
	uint64_t index_buffer = 0;
	uint32_t index_offset = 0;
	uint32_t index_format = 0;
	uint64_t vertex_buffer0 = 0;
	uint32_t vertex_offset0 = 0;
	uint32_t vertex_stride0 = 0;
	uint32_t first_index = 0;
	uint32_t index_count = 0;
	int32_t vertex_offset = 0;

	bool operator==(const mesh_signature &other) const {
		// FFXIV frequently rotates dynamic buffers between frames. Match the stable
		// geometry layout, while retaining the live handles for replay validation.
		return index_offset == other.index_offset && index_format == other.index_format &&
			vertex_offset0 == other.vertex_offset0 && vertex_stride0 == other.vertex_stride0 &&
			first_index == other.first_index && index_count == other.index_count &&
			vertex_offset == other.vertex_offset;
	}
};

struct draw_arguments {
	uint32_t vertex_count = 0;
	uint32_t index_count = 0;
	uint32_t instance_count = 0;
	uint32_t first_vertex = 0;
	uint32_t first_index = 0;
	int32_t vertex_offset = 0;
	uint32_t first_instance = 0;
};

struct mesh_signature_hash {
	size_t operator()(const mesh_signature &mesh) const {
		size_t value = 0;
		const auto mix = [&value](uint64_t part) {
			value ^= static_cast<size_t>(part) + static_cast<size_t>(0x9E3779B97F4A7C15ull) +
				(value << 6) + (value >> 2);
		};
		mix(mesh.index_offset);
		mix(mesh.index_format);
		mix(mesh.vertex_offset0);
		mix(mesh.vertex_stride0);
		mix(mesh.first_index);
		mix(mesh.index_count);
		mix(static_cast<uint32_t>(mesh.vertex_offset));
		return value;
	}
};

struct replay_resources {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11Texture2D> black_texture;
	ComPtr<ID3D11RenderTargetView> black_rtv;
	ComPtr<ID3D11ShaderResourceView> black_srv;
	ComPtr<ID3D11Texture2D> white_texture;
	ComPtr<ID3D11RenderTargetView> white_rtv;
	ComPtr<ID3D11ShaderResourceView> white_srv;
	ComPtr<ID3D11Texture2D> scene_black_texture;
	ComPtr<ID3D11ShaderResourceView> scene_black_srv;
	ComPtr<ID3D11Texture2D> scene_white_texture;
	ComPtr<ID3D11ShaderResourceView> scene_white_srv;
	ComPtr<ID3D11BlendState> alpha_blend;
	ComPtr<ID3D11BlendState> additive_blend;
	ComPtr<ID3D11BlendState> opaque_blend;
	UINT sample_count = 0;
	UINT sample_quality = 0;
	UINT width = 0;
	UINT height = 0;
};

struct state {
	config cfg;
	std::wstring config_path;
	std::wstring reshade_ini_path;
	std::wstring default_output_dir;
	std::wstring log_path;
	std::wstring output_dir;
	uint64_t capture_serial = 0;
	bool config_loaded = false;
	std::string config_error;
	std::string notification_message;
	bool notification_success = false;
	ULONGLONG notification_expires_at = 0;
	std::mutex pipeline_mutex;
	std::unordered_map<uint64_t, shader_hashes> pipeline_hashes;
	std::unordered_map<uint64_t, command_state> command_states;
	std::unordered_set<uint32_t> discard_shader_hashes;
	std::unordered_set<mesh_signature, mesh_signature_hash> learned_meshes;
	std::vector<ns_alpha_rules::rule_group> rule_groups;
	std::vector<ns_alpha_rules::nonindexed_rule> nonindexed_rules;
	std::vector<shader_candidate> shader_candidates;
	std::vector<nonindexed_candidate> nonindexed_candidates;
	bool shader_selector_active = false;
	ns_alpha_rules::preview_state preview;
	int group_editor_index = -1;
	ns_alpha_rules::rule_group group_editor_work;
	std::array<char, 256> group_name_input = {};
	int rule_editor_index = -1;
	std::vector<ns_alpha_rules::capture_rule> rule_editor_work;
	std::vector<std::array<char, 64>> rule_editor_names;
	std::string rule_editor_error;
	bool hunting_open = false;
	int hunting_target_group = -1;
	int hunting_stage = 0;
	size_t hunting_cursor = 0;
	std::unordered_set<uint32_t> hunting_marked_pixel;
	std::unordered_set<uint32_t> hunting_marked_vertex;
	replay_resources replay;
	bool replay_capture_active = false;
	bool replay_frame_started = false;
	bool replay_frame_has_draws = false;
	uint32_t replay_draw_count = 0;
	uint32_t replay_nonindexed_draw_count = 0;
	uint32_t replay_clear_count = 0;
	uint64_t replay_frame_target_resource = 0;
	std::vector<ComPtr<ID3D11Resource>> learned_scene_targets;
	bool locale_zh = false;
	int hotkey_capture_target = 0;
	uint32_t hotkey_modifier_latch = ns_white_backing::modifier_none;
	uint32_t hotkey_suppress_key = 0;
	std::array<char, 1024> output_path_input = {};
	std::array<char, 512> file_naming_input = {};
};

state g;
thread_local uint32_t g_replay_depth = 0;

bool shader_contains_discard(const void *code, size_t code_size);
bool capture_failure(const std::string &detail);
bool current_render_target_is_learned(ID3D11DeviceContext *context);
void sync_file_naming_input();
bool read_reshade_setting(const char *section, const char *key, std::string &value);
void log_line(const char *format, ...);

uint64_t command_key(command_list *cmd_list) {
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(cmd_list));
}

bool is_target_depth_draw(uint32_t pixel_hash, uint32_t vertex_hash,
	uint32_t index_count, uint32_t first_index, int32_t vertex_offset) {
	if (pixel_hash != 3243681800 || vertex_offset != 0)
		return false;
	if (vertex_hash == 3735619600)
		return (index_count == 2484 && first_index == 6496) ||
			(index_count == 576 && first_index == 2920);
	if (vertex_hash == 767615418)
		return index_count == 5472 && first_index == 11040;
	return false;
}

bool is_configured_shader_rule(const shader_hashes &hashes, uint32_t index_count,
	uint32_t first_index, int32_t vertex_offset) {
	return ns_alpha_rules::groups_match(g.rule_groups, hashes.pixel, hashes.vertex,
		first_index, index_count, vertex_offset);
}

bool is_configured_nonindexed_rule(const shader_hashes &hashes,
	uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
	uint32_t first_instance, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv,
	ID3D11BlendState *blend) {
	if (rtv == nullptr || blend == nullptr)
		return false;
	ComPtr<ID3D11Resource> resource;
	rtv->GetResource(&resource);
	ComPtr<ID3D11Texture2D> texture;
	if (resource == nullptr || FAILED(resource.As(&texture)))
		return false;
	D3D11_TEXTURE2D_DESC target_desc = {};
	texture->GetDesc(&target_desc);
	D3D11_BLEND_DESC blend_desc = {};
	blend->GetDesc(&blend_desc);
	const D3D11_RENDER_TARGET_BLEND_DESC &rt_blend = blend_desc.RenderTarget[0];
	for (const ns_alpha_rules::nonindexed_rule &rule : g.nonindexed_rules) {
		if (ns_alpha_rules::nonindexed_rule_matches(rule, hashes.pixel, hashes.vertex,
			vertex_count, instance_count, first_vertex, first_instance, 0, 0, 0,
			target_desc.Width, target_desc.Height, dsv != nullptr, rt_blend.BlendEnable != FALSE,
			static_cast<uint32_t>(rt_blend.SrcBlend), static_cast<uint32_t>(rt_blend.DestBlend),
			static_cast<uint32_t>(rt_blend.RenderTargetWriteMask)))
			return true;
	}
	return false;
}

void show_notification(bool success, const std::string &message) {
	g.notification_success = success;
	g.notification_message = message.substr(0, 180);
	g.notification_expires_at = GetTickCount64() + 3000;
}

const char *text(const char *english, const char *chinese) {
	return g.locale_zh ? chinese : english;
}

std::string capture_failure_message(const std::string &detail) {
	return std::string(text("NS Alpha Capture - Capture failed: ", "NS Alpha Capture - 捕获失败：")) + detail;
}

bool read_ini_setting(const std::wstring &path, const char *section, const char *key, std::string &value);

bool system_prefers_chinese() {
	ULONG language_count = 0;
	ULONG buffer_size = 0;
	if (!GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK,
		&language_count, nullptr, &buffer_size) || buffer_size == 0)
		return false;
	std::vector<wchar_t> languages(buffer_size);
	if (!GetThreadPreferredUILanguages(MUI_LANGUAGE_NAME | MUI_UI_FALLBACK,
		&language_count, languages.data(), &buffer_size) || language_count == 0)
		return false;
	const wchar_t *language = languages.data();
	return (language[0] == L'z' || language[0] == L'Z') &&
		(language[1] == L'h' || language[1] == L'H') &&
		(language[2] == L'\0' || language[2] == L'-' || language[2] == L'_');
}

void update_locale(effect_runtime *) {
	std::string language;
	read_reshade_setting("OVERLAY", "Language", language);
	std::transform(language.begin(), language.end(), language.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	g.locale_zh = language.empty() ? system_prefers_chinese() :
		language == "6" || language == "zh" ||
		language.rfind("zh-", 0) == 0 || language.rfind("zh_", 0) == 0;
}

std::string key_name(uint32_t key) {
	if (key >= 'A' && key <= 'Z')
		return std::string(1, static_cast<char>(key));
	if (key >= '0' && key <= '9')
		return std::string(1, static_cast<char>(key));
	if (key >= VK_F1 && key <= VK_F24)
		return "F" + std::to_string(key - VK_F1 + 1);
	static constexpr std::pair<uint32_t, const char *> names[] = {
		{ VK_SPACE, "SPACE" }, { VK_TAB, "TAB" }, { VK_RETURN, "ENTER" },
		{ VK_ESCAPE, "ESC" }, { VK_BACK, "BACKSPACE" }, { VK_DELETE, "DELETE" },
		{ VK_INSERT, "INSERT" }, { VK_HOME, "HOME" }, { VK_END, "END" },
		{ VK_PRIOR, "PAGEUP" }, { VK_NEXT, "PAGEDOWN" }, { VK_UP, "UP" },
		{ VK_DOWN, "DOWN" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
	};
	for (const auto &entry : names)
		if (entry.first == key)
			return entry.second;
	return "NONE";
}

std::string format_hotkey(uint32_t key, uint32_t modifiers) {
	if (key == 0)
		return text("Press a key", "请按下一个键");
	std::string result;
	if (modifiers & ns_white_backing::modifier_ctrl) result += "Ctrl + ";
	if (modifiers & ns_white_backing::modifier_shift) result += "Shift + ";
	if (modifiers & ns_white_backing::modifier_alt) result += "Alt + ";
	return result + key_name(key);
}

bool replace_ini_value(std::string &content, const std::string &name, const std::string &value) {
	bool replaced = false;
	for (size_t start = 0; start <= content.size();) {
		const size_t end = content.find('\n', start);
		const size_t line_end = end == std::string::npos ? content.size() : end;
		const bool has_cr = line_end > start && content[line_end - 1] == '\r';
		const size_t logical_end = has_cr ? line_end - 1 : line_end;
		const size_t equal = content.find('=', start);
		if (equal != std::string::npos && equal < logical_end &&
			content.compare(start, equal - start, name) == 0) {
			const std::string replacement = name + "=" + value + (has_cr ? "\r" : "");
			content.replace(start, line_end - start, replacement);
			replaced = true;
			start += replacement.size();
		} else {
			start = end == std::string::npos ? content.size() + 1 : end + 1;
		}
	}
	return replaced;
}

bool set_capture_ini_value(std::string &content, const std::string &name,
	const std::string &value) {
	if (replace_ini_value(content, name, value))
		return true;

	const size_t section = content.find("[Capture]");
	if (section == std::string::npos)
		return false;
	const std::string newline = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";
	size_t insert_at = content.size();
	for (size_t start = content.find('\n', section); start != std::string::npos;) {
		++start;
		if (start < content.size() && content[start] == '[') {
			insert_at = start;
			break;
		}
		start = content.find('\n', start);
	}
	if (insert_at == content.size() && !content.empty() && content.back() != '\n') {
		content += newline;
		insert_at = content.size();
	}
	content.insert(insert_at, name + "=" + value + newline);
	return true;
}

std::string trim_ascii(const std::string &value);
uint32_t pack_toggle_key(uint32_t key, uint32_t modifiers);

bool set_section_ini_value(std::string &content, const std::string &section,
	const std::string &name, const std::string &value) {
	bool in_section = false;
	bool section_found = false;
	const std::string newline = content.find("\r\n") != std::string::npos ? "\r\n" : "\n";
	for (size_t start = 0; start <= content.size();) {
		const size_t end = content.find('\n', start);
		const size_t line_end = end == std::string::npos ? content.size() : end;
		const bool has_cr = line_end > start && content[line_end - 1] == '\r';
		const size_t logical_end = has_cr ? line_end - 1 : line_end;
		std::string line = content.substr(start, logical_end - start);
		const std::string trimmed = trim_ascii(line);
		if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
			if (in_section && section_found) {
				content.insert(start, name + "=" + value + newline);
				return true;
			}
			in_section = trimmed == "[" + section + "]";
			section_found = in_section;
		}
		if (in_section) {
			const size_t equals = content.find('=', start);
			if (equals != std::string::npos && equals < logical_end &&
				trim_ascii(content.substr(start, equals - start)) == name) {
				const std::string replacement = name + "=" + value + (has_cr ? "\r" : "");
				content.replace(start, line_end - start, replacement);
				return true;
			}
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	if (in_section && section_found) {
		if (!content.empty() && content.back() != '\n')
			content += newline;
		content += name + "=" + value + newline;
		return true;
	}
	return false;
}

bool save_rule_groups() {
	std::string error;
	if (!ns_alpha_rules::save_rule_groups_file(g.config_path, g.rule_groups, error)) {
		show_notification(false, text("Could not save shader groups", "无法保存着色器组"));
		return false;
	}
	return true;
}

std::string hotkey_modifier_name(uint32_t modifiers) {
	std::string value;
	if (modifiers & ns_white_backing::modifier_ctrl) value += "Ctrl+";
	if (modifiers & ns_white_backing::modifier_shift) value += "Shift+";
	if (modifiers & ns_white_backing::modifier_alt) value += "Alt+";
	if (!value.empty()) value.pop_back();
	return value.empty() ? "None" : value;
}

bool save_hotkey(const char *key_name_value, const char *modifiers_name,
	uint32_t key, uint32_t modifiers) {
	FILE *file = nullptr;
	if (_wfopen_s(&file, g.config_path.c_str(), L"rb") != 0 || file == nullptr)
		return false;
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	std::string content(length > 0 ? static_cast<size_t>(length) : 0, '\0');
	const bool read_ok = content.empty() || fread(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	if (!read_ok)
		return false;
	if (!replace_ini_value(content, key_name_value, key_name(key)) ||
		!replace_ini_value(content, modifiers_name, hotkey_modifier_name(modifiers)))
		return false;
	if (_wfopen_s(&file, g.config_path.c_str(), L"wb") != 0 || file == nullptr)
		return false;
	const bool write_ok = fwrite(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	return write_ok;
}

bool save_output_selection(bool output_black, bool output_white, bool output_transparent) {
	if (!output_black && !output_white && !output_transparent)
		return false;
	FILE *file = nullptr;
	if (_wfopen_s(&file, g.config_path.c_str(), L"rb") != 0 || file == nullptr)
		return false;
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	std::string content(length > 0 ? static_cast<size_t>(length) : 0, '\0');
	const bool read_ok = content.empty() || fread(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	if (!read_ok ||
		!set_capture_ini_value(content, "OutputBlack", output_black ? "1" : "0") ||
		!set_capture_ini_value(content, "OutputWhite", output_white ? "1" : "0") ||
		!set_capture_ini_value(content, "OutputTransparent", output_transparent ? "1" : "0"))
		return false;
	if (_wfopen_s(&file, g.config_path.c_str(), L"wb") != 0 || file == nullptr)
		return false;
	const bool write_ok = fwrite(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	return write_ok;
}

std::string trim_ascii(const std::string &value);

bool save_file_naming(const std::string &configured_value) {
	const std::string value = trim_ascii(configured_value);
	if (value.empty() || value.size() >= 512 || value.find_first_of("\r\n") != std::string::npos)
		return false;
	FILE *file = nullptr;
	if (_wfopen_s(&file, g.config_path.c_str(), L"rb") != 0 || file == nullptr)
		return false;
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	std::string content(length > 0 ? static_cast<size_t>(length) : 0, '\0');
	const bool read_ok = content.empty() || fread(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	if (!read_ok || !set_capture_ini_value(content, "FileNaming", value))
		return false;
	if (_wfopen_s(&file, g.config_path.c_str(), L"wb") != 0 || file == nullptr)
		return false;
	const bool write_ok = fwrite(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	if (!write_ok)
		return false;
	g.cfg.file_naming = value;
	sync_file_naming_input();
	return true;
}

std::string trim_ascii(const std::string &value) {
	const size_t first = value.find_first_not_of(" \t");
	if (first == std::string::npos)
		return {};
	const size_t last = value.find_last_not_of(" \t");
	return value.substr(first, last - first + 1);
}

bool read_ini_setting(const std::wstring &path, const char *section, const char *key, std::string &value) {
	value.clear();
	FILE *file = nullptr;
	if (path.empty() || _wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
		return false;
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	std::string content(length > 0 ? static_cast<size_t>(length) : 0, '\0');
	const bool read_ok = content.empty() || fread(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	if (!read_ok)
		return false;
	if (content.size() >= 3 && static_cast<uint8_t>(content[0]) == 0xef &&
		static_cast<uint8_t>(content[1]) == 0xbb && static_cast<uint8_t>(content[2]) == 0xbf)
		content.erase(0, 3);
	std::string current_section;
	for (size_t start = 0; start <= content.size();) {
		const size_t end = content.find('\n', start);
		std::string line = content.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		line = trim_ascii(line);
		if (!line.empty() && line.front() != ';' && line.front() != '#') {
			if (line.front() == '[' && line.back() == ']')
				current_section = line.substr(1, line.size() - 2);
			else if (current_section == section) {
				const size_t equals = line.find('=');
				if (equals != std::string::npos && trim_ascii(line.substr(0, equals)) == key) {
					value = trim_ascii(line.substr(equals + 1));
					return true;
				}
			}
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return false;
}

bool is_auto_highlight_target_format(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

bool is_auto_highlight_shape(uint32_t vertex_count, uint32_t instance_count,
	uint32_t first_vertex, uint32_t first_instance, ID3D11RenderTargetView *rtv,
	ID3D11DepthStencilView *dsv, ID3D11BlendState *blend, uint32_t *width = nullptr,
	uint32_t *height = nullptr) {
	if (rtv == nullptr || dsv != nullptr || blend == nullptr ||
		(vertex_count != 3 && vertex_count != 4) || instance_count != 1 ||
		first_vertex != 0 || first_instance != 0)
		return false;
	ComPtr<ID3D11Resource> resource;
	rtv->GetResource(&resource);
	ComPtr<ID3D11Texture2D> texture;
	if (resource == nullptr || FAILED(resource.As(&texture)))
		return false;
	D3D11_TEXTURE2D_DESC target_desc = {};
	texture->GetDesc(&target_desc);
	if (target_desc.SampleDesc.Count != 1 || target_desc.Width < 640 ||
		target_desc.Height < 360 || !is_auto_highlight_target_format(target_desc.Format))
		return false;
	D3D11_BLEND_DESC blend_desc = {};
	blend->GetDesc(&blend_desc);
	const D3D11_RENDER_TARGET_BLEND_DESC &rt_blend = blend_desc.RenderTarget[0];
	if (!rt_blend.BlendEnable || rt_blend.SrcBlend != D3D11_BLEND_ONE ||
		rt_blend.DestBlend != D3D11_BLEND_INV_SRC_ALPHA ||
		rt_blend.BlendOp != D3D11_BLEND_OP_ADD || rt_blend.RenderTargetWriteMask != 0x07)
		return false;
	if (width != nullptr)
		*width = target_desc.Width;
	if (height != nullptr)
		*height = target_desc.Height;
	return true;
}

void record_nonindexed_candidate(const shader_hashes &hashes, uint32_t vertex_count,
	uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance,
	uint32_t width, uint32_t height) {
	for (nonindexed_candidate &candidate : g.nonindexed_candidates) {
		if (candidate.pixel == hashes.pixel && candidate.vertex == hashes.vertex &&
			candidate.vertex_count == vertex_count && candidate.instance_count == instance_count &&
			candidate.first_vertex == first_vertex && candidate.first_instance == first_instance &&
			candidate.width == width && candidate.height == height) {
			++candidate.draw_count;
			return;
		}
	}
	if (g.nonindexed_candidates.size() >= 256)
		return;
	nonindexed_candidate candidate;
	candidate.pixel = hashes.pixel;
	candidate.vertex = hashes.vertex;
	candidate.vertex_count = vertex_count;
	candidate.instance_count = instance_count;
	candidate.first_vertex = first_vertex;
	candidate.first_instance = first_instance;
	candidate.width = width;
	candidate.height = height;
	candidate.draw_count = 1;
	g.nonindexed_candidates.push_back(candidate);
	log_line("auto_highlight learned non-indexed candidate ps=%u vs=%u vertices=%u instances=%u target=%ux%u",
		hashes.pixel, hashes.vertex, vertex_count, instance_count, width, height);
}

bool has_learned_nonindexed_candidate(const shader_hashes &hashes, uint32_t vertex_count,
	uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
	for (const nonindexed_candidate &candidate : g.nonindexed_candidates) {
		if (candidate.pixel == hashes.pixel && candidate.vertex == hashes.vertex &&
			candidate.vertex_count == vertex_count && candidate.instance_count == instance_count &&
			candidate.first_vertex == first_vertex && candidate.first_instance == first_instance &&
			candidate.draw_count >= 2)
			return true;
	}
	return false;
}

bool read_reshade_setting(const char *section, const char *key, std::string &value) {
	std::array<char, 4096> buffer = {};
	size_t size = buffer.size();
	if (reshade::get_config_value(nullptr, section, key, buffer.data(), &size)) {
		value.assign(buffer.data(), strnlen_s(buffer.data(), buffer.size()));
		return true;
	}
	return read_ini_setting(g.reshade_ini_path, section, key, value);
}

bool utf8_to_wide(const std::string &value, std::wstring &result) {
	result.clear();
	if (value.empty())
		return true;
	if (value.size() > static_cast<size_t>(MAXINT))
		return false;

	const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (required <= 0)
		return false;
	result.resize(static_cast<size_t>(required));
	return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), required) == required;
}

bool wide_to_utf8(const std::wstring &value, std::string &result) {
	result.clear();
	if (value.empty())
		return true;
	if (value.size() > static_cast<size_t>(INT_MAX))
		return false;
	const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
	if (required <= 0)
		return false;
	result.resize(static_cast<size_t>(required));
	return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) == required;
}

void sync_output_path_input() {
	std::string output_path;
	if (!wide_to_utf8(g.output_dir, output_path))
		return;
	const size_t length = std::min(output_path.size(), g.output_path_input.size() - 1);
	memcpy(g.output_path_input.data(), output_path.data(), length);
	g.output_path_input[length] = '\0';
}

void sync_file_naming_input() {
	const size_t length = std::min(g.cfg.file_naming.size(), g.file_naming_input.size() - 1);
	memcpy(g.file_naming_input.data(), g.cfg.file_naming.data(), length);
	g.file_naming_input[length] = '\0';
}

bool read_utf8_capture_config(std::vector<std::pair<std::string, std::string>> &entries,
	std::string &error) {
	FILE *file = nullptr;
	if (_wfopen_s(&file, g.config_path.c_str(), L"rb") != 0 || file == nullptr) {
		error = "cannot open NS_AlphaCapture.ini";
		return false;
	}

	if (fseek(file, 0, SEEK_END) != 0) {
		fclose(file);
		error = "cannot read NS_AlphaCapture.ini";
		return false;
	}
	const long file_size = ftell(file);
	if (file_size < 0 || file_size > 64 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		error = "NS_AlphaCapture.ini has an invalid size";
		return false;
	}

	std::string bytes(static_cast<size_t>(file_size), '\0');
	if (!bytes.empty() && fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
		fclose(file);
		error = "cannot read NS_AlphaCapture.ini";
		return false;
	}
	fclose(file);

	if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xef &&
		static_cast<uint8_t>(bytes[1]) == 0xbb && static_cast<uint8_t>(bytes[2]) == 0xbf)
		bytes.erase(0, 3);
	if (!bytes.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
		static_cast<int>(bytes.size()), nullptr, 0) <= 0) {
		error = "NS_AlphaCapture.ini is not valid UTF-8";
		return false;
	}

	bool in_capture_section = false;
	for (size_t start = 0; start <= bytes.size();) {
		const size_t end = bytes.find('\n', start);
		std::string line = bytes.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		line = trim_ascii(line);

		if (!line.empty() && line.front() != ';' && line.front() != '#') {
			if (line.front() == '[' && line.back() == ']') {
				in_capture_section = line == "[Capture]";
			} else if (in_capture_section) {
				const size_t equals = line.find('=');
				if (equals != std::string::npos) {
					entries.emplace_back(trim_ascii(line.substr(0, equals)),
						trim_ascii(line.substr(equals + 1)));
				}
			}
		}

		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return true;
}

const std::string *find_config_value(
	const std::vector<std::pair<std::string, std::string>> &entries, const char *name) {
	for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
		if (entry->first == name)
			return &entry->second;
	}
	return nullptr;
}

uint32_t pack_toggle_key(uint32_t key, uint32_t modifiers) {
	return ((key & 0xFFu) << 24) |
		(((modifiers & ns_white_backing::modifier_alt) != 0 ? 1u : 0u) << 16) |
		(((modifiers & ns_white_backing::modifier_ctrl) != 0 ? 1u : 0u) << 8) |
		((modifiers & ns_white_backing::modifier_shift) != 0 ? 1u : 0u);
}

void unpack_toggle_key(uint32_t packed, uint32_t &key, uint32_t &modifiers) {
	key = (packed >> 24) & 0xFFu;
	modifiers = ns_white_backing::modifier_none;
	if (((packed >> 16) & 0xFFu) != 0) modifiers |= ns_white_backing::modifier_alt;
	if (((packed >> 8) & 0xFFu) != 0) modifiers |= ns_white_backing::modifier_ctrl;
	if ((packed & 0xFFu) != 0) modifiers |= ns_white_backing::modifier_shift;
}


void log_line(const char *format, ...) {
	FILE *file = nullptr;
	if (_wfopen_s(&file, g.log_path.c_str(), L"a") != 0 || file == nullptr)
		return;

	SYSTEMTIME now = {};
	GetLocalTime(&now);
	fprintf(file, "%02u:%02u:%02u.%03u ", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
	va_list args;
	va_start(args, format);
	vfprintf(file, format, args);
	va_end(args);
	fputc('\n', file);
	fclose(file);
}

bool build_paths() {
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&build_paths), &module) || module == nullptr)
		return false;

	wchar_t module_path[MAX_PATH] = {};
	if (GetModuleFileNameW(module, module_path, MAX_PATH) == 0)
		return false;

	std::wstring base(module_path);
	const size_t separator = base.find_last_of(L"\\/");
	base = separator == std::wstring::npos ? L"." : base.substr(0, separator);
	g.config_path = base + L"\\NS_AlphaCapture.ini";
	// Keep diagnostics beside the addon binary, separate from user capture files.
	g.log_path = base + L"\\NS_AlphaCapture.log";
	const size_t parent_separator = base.find_last_of(L"\\/");
	const std::wstring parent = parent_separator == std::wstring::npos ? L"" : base.substr(0, parent_separator);
	const std::array<std::wstring, 2> directories = { base, parent };
	const std::array<const wchar_t *, 2> ini_names = { L"ReShade.ini", L"GShade.ini" };
	for (const std::wstring &directory : directories) {
		if (directory.empty())
			continue;
		for (const wchar_t *ini_name : ini_names) {
			const std::wstring candidate = directory + L"\\" + ini_name;
			if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
				g.reshade_ini_path = candidate;
				break;
			}
		}
		if (!g.reshade_ini_path.empty())
			break;
	}

	PWSTR pictures_path = nullptr;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &pictures_path)) &&
		pictures_path != nullptr) {
		g.default_output_dir = pictures_path;
		CoTaskMemFree(pictures_path);
	}
	if (g.default_output_dir.empty()) {
		wchar_t profile_path[MAX_PATH] = {};
		const DWORD profile_length = GetEnvironmentVariableW(L"USERPROFILE", profile_path, MAX_PATH);
		if (profile_length > 0 && profile_length < MAX_PATH)
			g.default_output_dir = std::wstring(profile_path) + L"\\Pictures";
	}
	if (g.default_output_dir.empty())
		g.default_output_dir = base;
	g.output_dir = g.default_output_dir;
	return true;
}

void read_hotkey(const std::vector<std::pair<std::string, std::string>> &entries,
	const char *key_name, const char *modifiers_name,
	uint32_t &key, uint32_t &modifiers) {
	uint32_t parsed = 0;
	const std::string *key_value = find_config_value(entries, key_name);
	const std::string *modifiers_value = find_config_value(entries, modifiers_name);
	if (key_value != nullptr && ns_white_backing::try_parse_virtual_key(*key_value, parsed))
		key = parsed;
	if (modifiers_value != nullptr && ns_white_backing::try_parse_modifiers(*modifiers_value, parsed))
		modifiers = ns_white_backing::sanitize_modifiers(parsed);
}

bool load_config() {
	config fresh;
	std::vector<ns_alpha_rules::rule_group> fresh_rule_groups;
	std::vector<ns_alpha_rules::nonindexed_rule> fresh_nonindexed_rules;
	std::vector<std::pair<std::string, std::string>> entries;
	std::string error;
	if (!read_utf8_capture_config(entries, error)) {
		g.cfg = fresh;
		g.config_loaded = false;
		g.config_error = error;
		show_notification(false, capture_failure_message(error));
		return false;
	}

	read_hotkey(entries, "CaptureKey", "CaptureModifiers", fresh.capture_key, fresh.capture_modifiers);
	read_hotkey(entries, "ReloadKey", "ReloadModifiers", fresh.reload_key, fresh.reload_modifiers);
	const auto read_bool = [&entries](const char *name, bool &target) {
		if (const std::string *value = find_config_value(entries, name))
			target = *value == "1" || *value == "true" || *value == "TRUE";
	};
	read_bool("OutputBlack", fresh.output_black);
	read_bool("OutputWhite", fresh.output_white);
	read_bool("OutputTransparent", fresh.output_transparent);
	bool file_naming_from_addon = false;
	if (const std::string *file_naming_value = find_config_value(entries, "FileNaming")) {
		const std::string trimmed = trim_ascii(*file_naming_value);
		if (!trimmed.empty() && trimmed.find_first_of("\r\n") == std::string::npos) {
			fresh.file_naming = trimmed;
			file_naming_from_addon = true;
		}
	}
	if (!file_naming_from_addon) {
		std::string reshade_file_naming;
		if (read_reshade_setting("SCREENSHOT", "FileNaming", reshade_file_naming)) {
			const std::string trimmed = trim_ascii(reshade_file_naming);
			if (!trimmed.empty() && trimmed.find_first_of("\r\n") == std::string::npos)
				fresh.file_naming = trimmed;
		}
	}
	if (!fresh.output_black && !fresh.output_white && !fresh.output_transparent)
		fresh.output_transparent = true;
	if (const std::string *auto_match_value = find_config_value(entries, "AutoMatch"))
		fresh.auto_match = *auto_match_value == "1" || *auto_match_value == "true" ||
			*auto_match_value == "TRUE";
	if (const std::string *auto_highlight_value = find_config_value(entries, "AutoHighlight"))
		fresh.auto_highlight = *auto_highlight_value == "1" || *auto_highlight_value == "true" ||
			*auto_highlight_value == "TRUE";
	if (const std::string *lens_only_value = find_config_value(entries, "LensOnly"))
		fresh.lens_only = *lens_only_value == "1" || *lens_only_value == "true" ||
			*lens_only_value == "TRUE";
	if (const std::string *lens_capture_value = find_config_value(entries, "LensCapture"))
		fresh.lens_capture = *lens_capture_value == "1" || *lens_capture_value == "true" ||
			*lens_capture_value == "TRUE";
	{
		ns_alpha_rules::load_groups_result group_result =
			ns_alpha_rules::load_rule_groups_file(g.config_path);
		if (group_result.ok) {
			fresh_rule_groups = std::move(group_result.groups);
			if (group_result.migrated)
				log_line("shader groups migrated to format v2 backup_created=%u groups=%zu",
					group_result.backup_created ? 1u : 0u, fresh_rule_groups.size());
		} else if (!group_result.error.empty()) {
			log_line("shader group load failed: %s", group_result.error.c_str());
			show_notification(false, text("Shader group config error - check log",
				"着色器组配置错误 - 请查看日志"));
		}
	}
	{
		std::string nonindexed_error;
		if (!ns_alpha_rules::load_nonindexed_rules_file(g.config_path,
			fresh_nonindexed_rules, nonindexed_error)) {
			log_line("non-indexed rule load failed: %s", nonindexed_error.c_str());
		}
	}
	const auto read_u32 = [&entries](const char *name, uint32_t &target) {
		const std::string *value = find_config_value(entries, name);
		if (value == nullptr || value->empty())
			return true;
		char *end = nullptr;
		errno = 0;
		const unsigned long parsed = std::strtoul(value->c_str(), &end, 0);
		if (errno != 0 || end == nullptr || *end != '\0' || parsed > UINT32_MAX)
			return false;
		target = static_cast<uint32_t>(parsed);
		return true;
	};
	if (!read_u32("LensPixelShaderHash", fresh.lens_pixel_shader_hash) ||
		!read_u32("LensFirstIndex", fresh.lens_first_index) ||
		!read_u32("LensIndexCount", fresh.lens_index_count)) {
		g.cfg = fresh;
		g.config_loaded = false;
		g.config_error = "LensOnly configuration is invalid";
		show_notification(false, capture_failure_message(g.config_error));
		return false;
	}
	g.cfg = fresh;

	const std::string *output_value = find_config_value(entries, "OutputDirectory");
	std::wstring output_directory;
	if (output_value != nullptr && !output_value->empty()) {
		if (!utf8_to_wide(*output_value, output_directory)) {
			g.config_loaded = false;
			g.config_error = "OutputDirectory is not valid UTF-8";
			show_notification(false, capture_failure_message(g.config_error));
			return false;
		}
	} else {
		std::string screenshot_path;
		if (read_reshade_setting("SCREENSHOT", "SavePath", screenshot_path) &&
			!screenshot_path.empty() && !utf8_to_wide(screenshot_path, output_directory)) {
			g.config_loaded = false;
			g.config_error = "ReShade screenshot path is not valid UTF-8";
			show_notification(false, capture_failure_message(g.config_error));
			return false;
		}
		if (output_directory.empty())
			output_directory = g.default_output_dir;
	}
	if (output_directory.empty()) {
		g.config_loaded = false;
		g.config_error = "OutputDirectory is empty";
		show_notification(false, capture_failure_message(g.config_error));
		return false;
	}

	g.output_dir = output_directory;
	g.rule_groups = std::move(fresh_rule_groups);
	g.nonindexed_rules = std::move(fresh_nonindexed_rules);
	g.group_editor_index = -1;
	g.rule_editor_index = -1;
	ns_alpha_rules::preview_exit(g.preview);
	g.config_loaded = true;
	g.config_error.clear();
	sync_output_path_input();
	sync_file_naming_input();
	return true;
}

bool ensure_output_directory() {
	if (CreateDirectoryW(g.output_dir.c_str(), nullptr))
		return true;
	if (GetLastError() != ERROR_ALREADY_EXISTS)
		return false;
	const DWORD attributes = GetFileAttributesW(g.output_dir.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool save_output_directory(const std::string &configured_value) {
	const std::string value = trim_ascii(configured_value);
	std::wstring resolved;
	if (!value.empty()) {
		if (!utf8_to_wide(value, resolved)) {
			capture_failure(text("screenshot path is not valid UTF-8", "截图路径不是有效的 UTF-8"));
			return false;
		}
	} else {
		std::string reshade_path;
		if (read_reshade_setting("SCREENSHOT", "SavePath", reshade_path) &&
			!reshade_path.empty() && !utf8_to_wide(reshade_path, resolved)) {
			capture_failure(text("ReShade screenshot path is not valid UTF-8", "ReShade 截图路径不是有效的 UTF-8"));
			return false;
		}
		if (resolved.empty())
			resolved = g.default_output_dir;
	}

	const std::wstring previous_output = g.output_dir;
	g.output_dir = resolved;
	if (!ensure_output_directory()) {
		g.output_dir = previous_output;
		capture_failure(text("cannot create screenshot directory", "无法创建截图目录"));
		return false;
	}

	FILE *file = nullptr;
	if (_wfopen_s(&file, g.config_path.c_str(), L"rb") != 0 || file == nullptr) {
		g.output_dir = previous_output;
		capture_failure(text("cannot open addon configuration", "无法打开 addon 配置"));
		return false;
	}
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	std::string content(length > 0 ? static_cast<size_t>(length) : 0, '\0');
	const bool read_ok = content.empty() || fread(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	if (!read_ok || !replace_ini_value(content, "OutputDirectory", value) ||
		_wfopen_s(&file, g.config_path.c_str(), L"wb") != 0 || file == nullptr) {
		g.output_dir = previous_output;
		capture_failure(text("cannot save screenshot path", "无法保存截图路径"));
		return false;
	}
	const bool write_ok = fwrite(content.data(), 1, content.size(), file) == content.size();
	fclose(file);
	if (!write_ok) {
		g.output_dir = previous_output;
		capture_failure(text("cannot save screenshot path", "无法保存截图路径"));
		return false;
	}
	sync_output_path_input();
	show_notification(true, text("NS Alpha Capture - Screenshot path saved", "NS Alpha Capture - 截图路径已保存"));
	return true;
}

bool capture_failure(const std::string &detail) {
	log_line("capture failed: %s", detail.c_str());
	show_notification(false, capture_failure_message(detail));
	return false;
}

float half_to_float(uint16_t value) {
	const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
	uint32_t exponent = (value >> 10) & 0x1fu;
	uint32_t mantissa = value & 0x03ffu;
	uint32_t bits = 0;
	if (exponent == 0) {
		if (mantissa == 0) {
			bits = sign;
		} else {
			int32_t normalized_exponent = -14;
			while ((mantissa & 0x0400u) == 0) {
				mantissa <<= 1;
				--normalized_exponent;
			}
			mantissa &= 0x03ffu;
			bits = sign | (static_cast<uint32_t>(normalized_exponent + 127) << 23) |
				(mantissa << 13);
		}
	} else if (exponent == 0x1fu) {
		bits = sign | 0x7f800000u | (mantissa << 13);
	} else {
		bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
	}
	float result = 0.0f;
	memcpy(&result, &bits, sizeof(result));
	return result;
}

bool read_effect_texture_rgba(effect_runtime *runtime, const char *effect_name,
	const char *texture_name,
	rgba_image &image, std::string &error) {
	if (runtime == nullptr) {
		error = "ReShade runtime is unavailable";
		return false;
	}
	const effect_texture_variable variable =
		runtime->find_texture_variable(effect_name, texture_name);
	if (variable == 0) {
		error = std::string("texture variable not found: ") + texture_name;
		return false;
	}

	resource_view srv = { 0 };
	resource_view srv_srgb = { 0 };
	runtime->get_texture_binding(variable, &srv, &srv_srgb);
	if (srv == 0)
		srv = srv_srgb;
	if (srv == 0) {
		error = std::string("texture is not bound: ") + texture_name;
		return false;
	}

	ID3D11ShaderResourceView *native_view =
		reinterpret_cast<ID3D11ShaderResourceView *>(srv.handle);
	ComPtr<ID3D11Resource> resource;
	native_view->GetResource(&resource);
	ComPtr<ID3D11Texture2D> source;
	if (resource == nullptr || FAILED(resource.As(&source))) {
		error = std::string("bound texture is not Texture2D: ") + texture_name;
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	source->GetDesc(&desc);
	const bool rgba8_format = desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
		desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	const bool rgba16f_format = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
	if ((!rgba8_format && !rgba16f_format) || desc.Width == 0 || desc.Height == 0 ||
		desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) {
		char layout_error[256] = {};
		sprintf_s(layout_error,
			"unsupported texture layout for %s: format=%u size=%ux%u mips=%u array=%u samples=%u",
			texture_name, static_cast<unsigned>(desc.Format), desc.Width, desc.Height,
			desc.MipLevels, desc.ArraySize, desc.SampleDesc.Count);
		error = layout_error;
		return false;
	}

	ComPtr<ID3D11Device> device;
	source->GetDevice(&device);
	ComPtr<ID3D11DeviceContext> context;
	if (device != nullptr)
		device->GetImmediateContext(&context);
	if (device == nullptr || context == nullptr) {
		error = std::string("D3D11 device is unavailable for ") + texture_name;
		return false;
	}

	D3D11_TEXTURE2D_DESC staging_desc = desc;
	staging_desc.Usage = D3D11_USAGE_STAGING;
	staging_desc.BindFlags = 0;
	staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	staging_desc.MiscFlags = 0;
	ComPtr<ID3D11Texture2D> staging;
	HRESULT result = device->CreateTexture2D(&staging_desc, nullptr, &staging);
	if (FAILED(result)) {
		error = std::string("staging texture creation failed for ") + texture_name;
		return false;
	}

	context->CopyResource(staging.Get(), source.Get());
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(result)) {
		error = std::string("staging texture map failed for ") + texture_name;
		return false;
	}

	const size_t source_pixel_size = rgba16f_format ? 8 : 4;
	const size_t source_row_size = static_cast<size_t>(desc.Width) * source_pixel_size;
	if (mapped.RowPitch < source_row_size) {
		context->Unmap(staging.Get(), 0);
		error = std::string("mapped row pitch is too small for ") + texture_name;
		return false;
	}
	image.width = desc.Width;
	image.height = desc.Height;
	const size_t output_row_size = static_cast<size_t>(desc.Width) * 4;
	image.pixels.resize(output_row_size * desc.Height);
	const auto *source_row = static_cast<const uint8_t *>(mapped.pData);
	for (UINT y = 0; y < desc.Height; ++y) {
		uint8_t *output = image.pixels.data() + output_row_size * y;
		const uint8_t *row_data = source_row + mapped.RowPitch * y;
		if (rgba8_format) {
			memcpy(output, row_data, output_row_size);
			continue;
		}
		for (UINT x = 0; x < desc.Width; ++x) {
			const auto *half = reinterpret_cast<const uint16_t *>(row_data + x * source_pixel_size);
			for (UINT channel = 0; channel < 4; ++channel) {
				const float value = std::clamp(half_to_float(half[channel]), 0.0f, 1.0f);
				output[x * 4 + channel] = static_cast<uint8_t>(
					std::lround(value * 255.0f));
			}
		}
	}
	context->Unmap(staging.Get(), 0);
	return true;
}

bool make_lens_composite_outputs(const rgba_image &source, rgba_image &black,
	rgba_image &white, rgba_image &alpha, rgba_image &rgba) {
	if (source.width == 0 || source.height == 0 ||
		source.pixels.size() != static_cast<size_t>(source.width) * source.height * 4)
		return false;

	black.width = white.width = alpha.width = rgba.width = source.width;
	black.height = white.height = alpha.height = rgba.height = source.height;
	black.pixels.resize(source.pixels.size());
	white.pixels.resize(source.pixels.size());
	alpha.pixels.resize(source.pixels.size());
	rgba.pixels.resize(source.pixels.size());
	bool has_content = false;
	for (size_t offset = 0; offset < source.pixels.size(); offset += 4) {
		const uint8_t source_alpha = source.pixels[offset + 3];
		if (source_alpha != 0 || source.pixels[offset + 0] != 0 ||
			source.pixels[offset + 1] != 0 || source.pixels[offset + 2] != 0)
			has_content = true;
		for (size_t channel = 0; channel < 3; ++channel) {
			const uint8_t source_rgb = source.pixels[offset + channel];
			black.pixels[offset + channel] = source_rgb;
			white.pixels[offset + channel] = static_cast<uint8_t>(std::min(
				255u, static_cast<unsigned>(source_rgb) + 255u - source_alpha));
			alpha.pixels[offset + channel] = source_alpha;
			rgba.pixels[offset + channel] = source_alpha > 0 ? static_cast<uint8_t>(
				std::min(255u, static_cast<unsigned>(source_rgb) * 255u / source_alpha)) : 0;
		}
		black.pixels[offset + 3] = 255;
		white.pixels[offset + 3] = 255;
		alpha.pixels[offset + 3] = 255;
		rgba.pixels[offset + 3] = source_alpha;
	}
	return has_content;
}

std::vector<uint8_t> rgba_to_bgra(const std::vector<uint8_t> &rgba) {
	std::vector<uint8_t> bgra(rgba.size());
	for (size_t offset = 0; offset + 3 < rgba.size(); offset += 4) {
		bgra[offset + 0] = rgba[offset + 2];
		bgra[offset + 1] = rgba[offset + 1];
		bgra[offset + 2] = rgba[offset + 0];
		bgra[offset + 3] = rgba[offset + 3];
	}
	return bgra;
}

std::string wic_error(const char *stage, HRESULT result) {
	char message[128] = {};
	sprintf_s(message, "WIC %s failed (HRESULT=0x%08X)", stage,
		static_cast<unsigned int>(result));
	return message;
}

bool save_texture_png(const std::wstring &path, const rgba_image &image, std::string &error) {
	const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	const bool should_uninitialize = SUCCEEDED(com_result);
	if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
		error = wic_error("CoInitializeEx", com_result);
		return false;
	}

	ComPtr<IWICImagingFactory> factory;
	ComPtr<IWICStream> stream;
	ComPtr<IWICBitmapEncoder> encoder;
	ComPtr<IWICBitmapFrameEncode> frame;
	ComPtr<IPropertyBag2> properties;

	const char *stage = "CreateFactory";
	HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
		IID_PPV_ARGS(&factory));
	if (SUCCEEDED(result)) {
		stage = "CreateStream";
		result = factory->CreateStream(&stream);
	}
	if (SUCCEEDED(result)) {
		stage = "InitializeStream";
		result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
	}
	if (SUCCEEDED(result)) {
		stage = "CreateEncoder";
		result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
	}
	if (SUCCEEDED(result)) {
		stage = "InitializeEncoder";
		result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
	}
	if (SUCCEEDED(result)) {
		stage = "CreateFrame";
		result = encoder->CreateNewFrame(&frame, &properties);
	}
	if (SUCCEEDED(result)) {
		stage = "InitializeFrame";
		result = frame->Initialize(properties.Get());
	}
	if (SUCCEEDED(result)) {
		stage = "SetSize";
		result = frame->SetSize(image.width, image.height);
	}

	WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
	if (SUCCEEDED(result)) {
		stage = "SetPixelFormat";
		result = frame->SetPixelFormat(&pixel_format);
	}
	if (SUCCEEDED(result) && !IsEqualGUID(pixel_format, GUID_WICPixelFormat32bppBGRA))
		result = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;

	const UINT stride = image.width * 4;
	const std::vector<uint8_t> bgra = rgba_to_bgra(image.pixels);
	if (bgra.size() > MAXUINT) {
		stage = "ValidateBuffer";
		result = E_INVALIDARG;
	}
	if (SUCCEEDED(result)) {
		stage = "WritePixels";
		result = frame->WritePixels(image.height, stride,
			static_cast<UINT>(bgra.size()), const_cast<BYTE *>(bgra.data()));
	}
	if (SUCCEEDED(result)) {
		stage = "FrameCommit";
		result = frame->Commit();
	}
	if (SUCCEEDED(result)) {
		stage = "EncoderCommit";
		result = encoder->Commit();
	}

	properties.Reset();
	frame.Reset();
	encoder.Reset();
	stream.Reset();
	factory.Reset();
	if (should_uninitialize)
		CoUninitialize();
	if (FAILED(result)) {
		error = wic_error(stage, result);
		DeleteFileW(path.c_str());
		return false;
	}
	return true;
}

std::string current_preset_name(effect_runtime *runtime) {
	if (runtime == nullptr)
		return {};
	char path[1024] = {};
	runtime->get_current_preset_path(path);
	std::string value = path;
	const size_t separator = value.find_last_of("/\\");
	if (separator != std::string::npos)
		value.erase(0, separator + 1);
	const size_t extension = value.find_last_of('.');
	if (extension != std::string::npos)
		value.erase(extension);
	return value;
}

std::string current_app_name() {
	std::array<wchar_t, 32768> module_path = {};
	const DWORD length = GetModuleFileNameW(nullptr, module_path.data(),
		static_cast<DWORD>(module_path.size()));
	if (length == 0 || length >= module_path.size())
		return {};
	std::wstring value(module_path.data(), length);
	const size_t separator = value.find_last_of(L"\\/");
	if (separator != std::wstring::npos)
		value.erase(0, separator + 1);
	const size_t extension = value.find_last_of(L'.');
	if (extension != std::wstring::npos)
		value.erase(extension);
	std::string result;
	return wide_to_utf8(value, result) ? result : std::string();
}

std::string expand_reshade_macro_string(const std::string &input,
	const std::vector<std::pair<std::string, std::string>> &macros) {
	std::string result;
	for (size_t offset = 0; offset < input.size();) {
		const size_t macro_begin = input.find('%', offset);
		if (macro_begin == std::string::npos) {
			result += input.substr(offset);
			break;
		}
		const size_t macro_end = input.find('%', macro_begin + 1);
		if (macro_end == std::string::npos) {
			result += input.substr(offset);
			break;
		}
		result += input.substr(offset, macro_begin - offset);
		if (macro_end == macro_begin + 1) {
			result += '%';
			offset = macro_end + 1;
			continue;
		}

		std::string_view replacing(input.data() + macro_begin + 1,
			macro_end - macro_begin - 1);
		const size_t colon = replacing.find(':');
		const std::string name(replacing.substr(0, colon));
		std::string value;
		for (const auto &macro : macros) {
			if (_stricmp(name.c_str(), macro.first.c_str()) == 0) {
				value = macro.second;
				break;
			}
		}

		if (colon == std::string_view::npos) {
			result += value;
		} else {
			const std::string_view parameter = replacing.substr(colon + 1);
			const size_t insert = parameter.find('$');
			if (insert == std::string_view::npos) {
				result += parameter;
			} else {
				result += parameter.substr(0, insert);
				result += value;
				result += parameter.substr(insert + 1);
			}
		}
		offset = macro_end + 1;
	}
	return result;
}

std::string expand_file_naming(const std::string &pattern, const SYSTEMTIME &now,
	const std::string &preset_name, const std::string &app_name, uint64_t count) {
	char date[16] = {};
	char time[16] = {};
	char year[8] = {};
	char month[4] = {};
	char day[4] = {};
	char hour[4] = {};
	char minute[4] = {};
	char second[4] = {};
	char milliseconds[8] = {};
	sprintf_s(date, "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
	sprintf_s(time, "%02u-%02u-%02u", now.wHour, now.wMinute, now.wSecond);
	sprintf_s(year, "%04u", now.wYear);
	sprintf_s(month, "%02u", now.wMonth);
	sprintf_s(day, "%02u", now.wDay);
	sprintf_s(hour, "%02u", now.wHour);
	sprintf_s(minute, "%02u", now.wMinute);
	sprintf_s(second, "%02u", now.wSecond);
	sprintf_s(milliseconds, "%03u", now.wMilliseconds);
	std::string expanded = pattern.empty() ? default_file_naming : pattern;
	return expand_reshade_macro_string(expanded, {
		{ "AppName", app_name }, { "PresetName", preset_name },
		{ "Count", std::to_string(count) },
		{ "Date", date }, { "DateYear", year }, { "Year", year },
		{ "DateMonth", month }, { "Month", month },
		{ "DateDay", day }, { "Day", day },
		{ "Time", time }, { "TimeHour", hour }, { "Hour", hour },
		{ "TimeMinute", minute }, { "Minute", minute },
		{ "TimeSecond", second }, { "Second", second },
		{ "TimeMillisecond", milliseconds }, { "Millisecond", milliseconds },
		{ "TimeMS", milliseconds },
	});
}

std::wstring sanitize_capture_file_name(const std::wstring &value) {
	std::wstring sanitized;
	sanitized.reserve(value.size());
	for (const wchar_t character : value) {
		const bool invalid = character < 0x20 || character == L'<' || character == L'>' ||
			character == L':' || character == L'"' || character == L'/' || character == L'\\' ||
			character == L'|' || character == L'?' || character == L'*';
		sanitized.push_back(invalid ? L'_' : character);
	}
	while (!sanitized.empty() && (sanitized.back() == L'.' || sanitized.back() == L' '))
		sanitized.pop_back();
	if (sanitized.empty() || sanitized == L"." || sanitized == L"..")
		sanitized = L"capture";
	if (sanitized.size() > 180)
		sanitized.resize(180);
	return sanitized;
}

std::wstring make_capture_prefix(effect_runtime *runtime) {
	SYSTEMTIME now = {};
	GetLocalTime(&now);
	++g.capture_serial;
	std::wstring file_name;
	if (!utf8_to_wide(expand_file_naming(g.cfg.file_naming, now,
		current_preset_name(runtime), current_app_name(), g.capture_serial), file_name))
		file_name = L"capture";
	return g.output_dir + L"\\" + sanitize_capture_file_name(file_name);
}

bool make_replay_blend_states(ID3D11Device *device, replay_resources &resources,
	std::string &error) {
	D3D11_BLEND_DESC alpha_desc = {};
	alpha_desc.RenderTarget[0].BlendEnable = TRUE;
	alpha_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	alpha_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	alpha_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	alpha_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	alpha_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	alpha_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	alpha_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	HRESULT result = device->CreateBlendState(&alpha_desc, &resources.alpha_blend);
	if (FAILED(result)) {
		error = "private alpha blend state creation failed";
		return false;
	}

	D3D11_BLEND_DESC additive_desc = alpha_desc;
	additive_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	additive_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
	additive_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
	additive_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	result = device->CreateBlendState(&additive_desc, &resources.additive_blend);
	if (FAILED(result)) {
		error = "private additive blend state creation failed";
		return false;
	}

	D3D11_BLEND_DESC opaque_desc = {};
	opaque_desc.RenderTarget[0].BlendEnable = FALSE;
	opaque_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
		D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	result = device->CreateBlendState(&opaque_desc, &resources.opaque_blend);
	if (FAILED(result)) {
		error = "private opaque blend state creation failed";
		return false;
	}
	return true;
}

bool ensure_replay_resources(ID3D11DeviceContext *context,
	ID3D11RenderTargetView *original_rtv, ID3D11DepthStencilView *original_dsv,
	std::string &error, bool allow_null_dsv = false) {
	if (context == nullptr || original_rtv == nullptr || (!allow_null_dsv && original_dsv == nullptr)) {
		error = "draw has no D3D11 context, render target, or depth target";
		return false;
	}

	ComPtr<ID3D11Resource> original_resource;
	original_rtv->GetResource(&original_resource);
	ComPtr<ID3D11Texture2D> original_texture;
	if (original_resource == nullptr || FAILED(original_resource.As(&original_texture))) {
		error = "target render target is not a Texture2D";
		return false;
	}

	D3D11_TEXTURE2D_DESC original_desc = {};
	original_texture->GetDesc(&original_desc);
	if (original_desc.Width == 0 || original_desc.Height == 0 ||
		original_desc.MipLevels != 1 || original_desc.ArraySize != 1 ||
		original_desc.SampleDesc.Count != 1) {
		error = "target RT is not a single-sample Texture2D";
		return false;
	}

	D3D11_TEXTURE2D_DESC depth_desc = {};
	depth_desc.SampleDesc.Count = 1;
	if (original_dsv != nullptr) {
		ComPtr<ID3D11Resource> depth_resource;
		original_dsv->GetResource(&depth_resource);
		ComPtr<ID3D11Texture2D> original_depth_surface;
		if (depth_resource == nullptr || FAILED(depth_resource.As(&original_depth_surface))) {
			error = "target depth resource is not a Texture2D";
			return false;
		}
		original_depth_surface->GetDesc(&depth_desc);
		if (depth_desc.Width != original_desc.Width || depth_desc.Height != original_desc.Height ||
			depth_desc.SampleDesc.Count != 1) {
			error = "target depth dimensions or sample count differ from the color RT";
			return false;
		}
	}
	ComPtr<ID3D11Device> device;
	context->GetDevice(&device);
	if (device == nullptr) {
		error = "draw context has no D3D11 device";
		return false;
	}

	if (g.replay.black_texture != nullptr && g.replay.white_texture != nullptr &&
		g.replay.scene_black_texture != nullptr && g.replay.scene_white_texture != nullptr &&
		g.replay.device.Get() == device.Get() &&
		g.replay.width == original_desc.Width && g.replay.height == original_desc.Height &&
		g.replay.sample_count == depth_desc.SampleDesc.Count &&
		g.replay.sample_quality == depth_desc.SampleDesc.Quality)
		return true;

	UINT format_support = 0;
	if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R32G32B32A32_FLOAT,
		&format_support)) || (format_support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) == 0) {
		error = "R32G32B32A32_FLOAT render target is unsupported";
		return false;
	}

	replay_resources fresh;
	fresh.device = device;
	fresh.width = original_desc.Width;
	fresh.height = original_desc.Height;
	fresh.sample_count = original_desc.SampleDesc.Count;
	fresh.sample_quality = original_desc.SampleDesc.Quality;
	D3D11_TEXTURE2D_DESC capture_desc = {};
	capture_desc.Width = original_desc.Width;
	capture_desc.Height = original_desc.Height;
	capture_desc.MipLevels = 1;
	capture_desc.ArraySize = 1;
	capture_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	capture_desc.SampleDesc.Count = 1;
	capture_desc.Usage = D3D11_USAGE_DEFAULT;
	capture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	HRESULT result = device->CreateTexture2D(&capture_desc, nullptr, &fresh.black_texture);
	if (FAILED(result)) { error = "private black RGBA32F texture creation failed"; return false; }
	result = device->CreateTexture2D(&capture_desc, nullptr, &fresh.white_texture);
	if (FAILED(result)) { error = "private white RGBA32F texture creation failed"; return false; }
	D3D11_TEXTURE2D_DESC scene_desc = capture_desc;
	scene_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	result = device->CreateTexture2D(&scene_desc, nullptr, &fresh.scene_black_texture);
	if (FAILED(result)) { error = "private black scene SRV texture creation failed"; return false; }
	result = device->CreateTexture2D(&scene_desc, nullptr, &fresh.scene_white_texture);
	if (FAILED(result)) { error = "private white scene SRV texture creation failed"; return false; }
	D3D11_RENDER_TARGET_VIEW_DESC view_desc = {};
	view_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	result = device->CreateRenderTargetView(fresh.black_texture.Get(), &view_desc, &fresh.black_rtv);
	if (FAILED(result)) { error = "private black RGBA32F RTV creation failed"; return false; }
	result = device->CreateRenderTargetView(fresh.white_texture.Get(), &view_desc, &fresh.white_rtv);
	if (FAILED(result)) { error = "private white RGBA32F RTV creation failed"; return false; }
	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	srv_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv_desc.Texture2D.MipLevels = 1;
	result = device->CreateShaderResourceView(fresh.black_texture.Get(), &srv_desc, &fresh.black_srv);
	if (FAILED(result)) { error = "private black RGBA32F SRV creation failed"; return false; }
	result = device->CreateShaderResourceView(fresh.white_texture.Get(), &srv_desc, &fresh.white_srv);
	if (FAILED(result)) { error = "private white RGBA32F SRV creation failed"; return false; }
	result = device->CreateShaderResourceView(fresh.scene_black_texture.Get(), &srv_desc, &fresh.scene_black_srv);
	if (FAILED(result)) { error = "private black scene SRV creation failed"; return false; }
	result = device->CreateShaderResourceView(fresh.scene_white_texture.Get(), &srv_desc, &fresh.scene_white_srv);
	if (FAILED(result)) { error = "private white scene SRV creation failed"; return false; }
	if (!make_replay_blend_states(device.Get(), fresh, error))
		return false;

	g.replay = std::move(fresh);
	g.replay_frame_started = false;
	g.replay_frame_target_resource = 0;
	log_line("private replay RTs ready: %ux%u format=R32G32B32A32_FLOAT dual_background=1",
		g.replay.width, g.replay.height);
	return true;
}

bool make_replay_depth_state(ID3D11Device *device,
	ID3D11DepthStencilState *original_depth_state,
	ComPtr<ID3D11DepthStencilState> &replay_depth_state, bool relax_equal,
	std::string &error) {
	if (device == nullptr) {
		error = "target draw has no D3D11 device";
		return false;
	}

	D3D11_DEPTH_STENCIL_DESC description = {};
	if (original_depth_state != nullptr) {
		original_depth_state->GetDesc(&description);
	} else {
		description.DepthEnable = TRUE;
		description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		description.DepthFunc = D3D11_COMPARISON_LESS;
	}
	// Replay must test against the game's depth/stencil values without modifying them.
	description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	if (relax_equal && description.DepthFunc == D3D11_COMPARISON_GREATER)
		description.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
	else if (relax_equal && description.DepthFunc == D3D11_COMPARISON_LESS)
		description.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	if (description.StencilEnable)
		description.StencilWriteMask = 0;

	HRESULT result = device->CreateDepthStencilState(&description, &replay_depth_state);
	if (FAILED(result)) {
		error = "replay depth-stencil state creation failed";
		return false;
	}
	return true;
}

bool classify_original_blend(ID3D11BlendState *original,
	D3D11_BLEND_DESC &description, bool &additive, std::string &error) {
	if (original == nullptr) {
		error = "target draw has no blend state";
		return false;
	}
	original->GetDesc(&description);
	const D3D11_RENDER_TARGET_BLEND_DESC &rt = description.RenderTarget[0];
	if (!rt.BlendEnable) {
		error = "target draw blend is disabled";
		return false;
	}
	if (rt.SrcBlend == D3D11_BLEND_SRC_ALPHA &&
		rt.DestBlend == D3D11_BLEND_INV_SRC_ALPHA) {
		additive = false;
		return true;
	}
	if (rt.SrcBlend == D3D11_BLEND_ONE && rt.DestBlend == D3D11_BLEND_ONE) {
		additive = true;
		return true;
	}
	error = "target draw uses an unsupported RGB blend pair";
	return false;
}

UINT replay_restore_rtv_count(const std::array<ID3D11RenderTargetView *,
	D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> &rtvs) {
	UINT count = static_cast<UINT>(rtvs.size());
	while (count > 1 && rtvs[count - 1] == nullptr)
		--count;
	return count;
}

uint64_t render_target_resource_id(ID3D11RenderTargetView *rtv) {
	if (rtv == nullptr)
		return 0;
	ComPtr<ID3D11Resource> resource;
	rtv->GetResource(&resource);
	return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(resource.Get()));
}

void copy_scene_color_substitutes(ID3D11DeviceContext *context) {
	if (context == nullptr || g.replay.scene_black_texture == nullptr ||
		g.replay.scene_white_texture == nullptr)
		return;
	context->CopyResource(g.replay.scene_black_texture.Get(), g.replay.black_texture.Get());
	context->CopyResource(g.replay.scene_white_texture.Get(), g.replay.white_texture.Get());
}

void initialize_replay_targets(ID3D11DeviceContext *context, uint64_t original_resource_id) {
	if (g.replay_frame_started && g.replay_frame_target_resource == original_resource_id)
		return;
	const float black_clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float white_clear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	context->ClearRenderTargetView(g.replay.black_rtv.Get(), black_clear);
	context->ClearRenderTargetView(g.replay.white_rtv.Get(), white_clear);
	copy_scene_color_substitutes(context);
	g.replay_frame_started = true;
	g.replay_frame_target_resource = original_resource_id;
	g.replay_frame_has_draws = false;
	g.replay_draw_count = 0;
	g.replay_nonindexed_draw_count = 0;
	g.replay_clear_count = 0;
}

bool mirror_scene_draw(command_list *cmd_list, uint32_t index_count,
	uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
	uint32_t first_instance) {
	ID3D11DeviceContext *context = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
	if (context == nullptr)
		return false;
	std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> original_rtvs = {};
	ID3D11DepthStencilView *original_dsv = nullptr;
	context->OMGetRenderTargets(static_cast<UINT>(original_rtvs.size()), original_rtvs.data(), &original_dsv);
	const uint64_t target_id = render_target_resource_id(original_rtvs[0]);
	if (original_rtvs[0] == nullptr || original_dsv == nullptr ||
		target_id == 0 || !current_render_target_is_learned(context)) {
		for (ID3D11RenderTargetView *rtv : original_rtvs) if (rtv != nullptr) rtv->Release();
		if (original_dsv != nullptr) original_dsv->Release();
		return false;
	}
	ID3D11BlendState *original_blend = nullptr;
	FLOAT original_factor[4] = {};
	UINT original_sample_mask = 0;
	context->OMGetBlendState(&original_blend, original_factor, &original_sample_mask);
	if (original_blend != nullptr) {
		D3D11_BLEND_DESC blend_desc = {};
		original_blend->GetDesc(&blend_desc);
		if (blend_desc.RenderTarget[0].RenderTargetWriteMask == 0) {
			original_blend->Release();
			for (ID3D11RenderTargetView *rtv : original_rtvs) if (rtv != nullptr) rtv->Release();
			original_dsv->Release();
			return false;
		}
	}
	ID3D11DepthStencilState *original_depth_state = nullptr;
	UINT original_stencil_ref = 0;
	context->OMGetDepthStencilState(&original_depth_state, &original_stencil_ref);
	std::string error;
	ComPtr<ID3D11DepthStencilState> replay_depth_state;
	if (!ensure_replay_resources(context, original_rtvs[0], original_dsv, error) ||
		!make_replay_depth_state(g.replay.device.Get(), original_depth_state,
			replay_depth_state, false, error)) {
		log_line("scene mirror skipped: %s", error.c_str());
		if (original_blend != nullptr) original_blend->Release();
		if (original_depth_state != nullptr) original_depth_state->Release();
		for (ID3D11RenderTargetView *rtv : original_rtvs) if (rtv != nullptr) rtv->Release();
		original_dsv->Release();
		return false;
	}
	initialize_replay_targets(context, target_id);
	const UINT restore_count = replay_restore_rtv_count(original_rtvs);
	context->OMSetBlendState(original_blend, original_factor, original_sample_mask);
	context->OMSetDepthStencilState(replay_depth_state.Get(), original_stencil_ref);
	++g_replay_depth;
	ID3D11RenderTargetView *replay_rtv = g.replay.black_rtv.Get();
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, &replay_rtv, original_dsv,
		1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	context->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
	replay_rtv = g.replay.white_rtv.Get();
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, &replay_rtv, original_dsv,
		1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	context->DrawIndexedInstanced(index_count, instance_count, first_index, vertex_offset, first_instance);
	--g_replay_depth;
	context->OMSetRenderTargetsAndUnorderedAccessViews(restore_count, original_rtvs.data(), original_dsv,
		restore_count, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	context->OMSetDepthStencilState(original_depth_state, original_stencil_ref);
	context->OMSetBlendState(original_blend, original_factor, original_sample_mask);
	if (original_blend != nullptr) original_blend->Release();
	if (original_depth_state != nullptr) original_depth_state->Release();
	for (ID3D11RenderTargetView *rtv : original_rtvs) if (rtv != nullptr) rtv->Release();
	original_dsv->Release();
	g.replay_frame_has_draws = true;
	++g.replay_draw_count;
	return true;
}

struct saved_lens_srv {
	UINT slot = 0;
	ID3D11ShaderResourceView *view = nullptr;
};

bool is_lens_scene_color_format(DXGI_FORMAT format) {
	switch (format) {
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return true;
	default:
		return false;
	}
}

bool lens_scene_srv_matches(ID3D11ShaderResourceView *view, UINT width, UINT height) {
	if (view == nullptr)
		return false;
	ComPtr<ID3D11Resource> resource;
	view->GetResource(&resource);
	ComPtr<ID3D11Texture2D> texture;
	if (resource == nullptr || FAILED(resource.As(&texture)))
		return false;
	D3D11_TEXTURE2D_DESC description = {};
	texture->GetDesc(&description);
	return description.Width == width && description.Height == height &&
		description.ArraySize == 1 && description.SampleDesc.Count == 1 &&
		is_lens_scene_color_format(description.Format);
}

void bind_lens_scene_srvs(ID3D11DeviceContext *context, ID3D11ShaderResourceView *replacement,
	std::vector<saved_lens_srv> &saved, UINT width, UINT height, bool capture_original) {
	if (context == nullptr || replacement == nullptr)
		return;
	for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
		if (capture_original) {
			ID3D11ShaderResourceView *current = nullptr;
			context->PSGetShaderResources(slot, 1, &current);
			if (!lens_scene_srv_matches(current, width, height)) {
				if (current != nullptr) current->Release();
				continue;
			}
			saved.push_back({ slot, current });
		}
		for (const saved_lens_srv &entry : saved) {
			if (entry.slot != slot)
				continue;
			context->PSSetShaderResources(slot, 1, &replacement);
			break;
		}
	}
}

void restore_lens_scene_srvs(ID3D11DeviceContext *context, std::vector<saved_lens_srv> &saved) {
	if (context == nullptr)
		return;
	for (const saved_lens_srv &entry : saved) {
		context->PSSetShaderResources(entry.slot, 1, &entry.view);
		if (entry.view != nullptr)
			entry.view->Release();
	}
	saved.clear();
}

bool replay_color_draw(command_list *cmd_list, uint32_t index_count,
	uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
	uint32_t first_instance, uint32_t pixel_hash, bool lens_only) {
	ID3D11DeviceContext *context = reinterpret_cast<ID3D11DeviceContext *>(
		cmd_list->get_native());
	if (context == nullptr)
		return false;
	const D3D11_DEVICE_CONTEXT_TYPE context_type = context->GetType();

	std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> original_rtvs = {};
	ID3D11DepthStencilView *original_dsv = nullptr;
	context->OMGetRenderTargets(static_cast<UINT>(original_rtvs.size()),
		original_rtvs.data(), &original_dsv);
	if (original_rtvs[0] == nullptr || original_dsv == nullptr) {
		for (ID3D11RenderTargetView *rtv : original_rtvs)
			if (rtv != nullptr)
				rtv->Release();
		if (original_dsv != nullptr)
			original_dsv->Release();
		return false;
	}

	ID3D11BlendState *original_blend = nullptr;
	FLOAT original_factor[4] = {};
	UINT original_sample_mask = 0;
	context->OMGetBlendState(&original_blend, original_factor, &original_sample_mask);
	ID3D11DepthStencilState *original_depth_state = nullptr;
	UINT original_stencil_ref = 0;
	context->OMGetDepthStencilState(&original_depth_state, &original_stencil_ref);
	D3D11_BLEND_DESC original_blend_desc = {};
	bool additive = false;
	std::string error;
	ComPtr<ID3D11DepthStencilState> replay_depth_state;
	bool blend_supported = false;
	if (lens_only) {
		if (original_blend == nullptr) {
			error = "lens target draw has no blend state";
		} else {
			original_blend->GetDesc(&original_blend_desc);
			blend_supported = !original_blend_desc.RenderTarget[0].BlendEnable &&
				original_blend_desc.RenderTarget[0].RenderTargetWriteMask == 0x07;
			if (!blend_supported)
				error = "lens target draw blend state is not opaque RGB";
		}
	} else {
		blend_supported = classify_original_blend(original_blend, original_blend_desc, additive, error);
	}
	if (!blend_supported ||
		!ensure_replay_resources(context, original_rtvs[0], original_dsv, error) ||
		!make_replay_depth_state(g.replay.device.Get(), original_depth_state,
			replay_depth_state, true, error)) {
		log_line("color replay skipped ps=%u: %s", pixel_hash, error.c_str());
		if (original_blend != nullptr)
			original_blend->Release();
		if (original_depth_state != nullptr)
			original_depth_state->Release();
		for (ID3D11RenderTargetView *rtv : original_rtvs)
			if (rtv != nullptr)
				rtv->Release();
		if (original_dsv != nullptr)
			original_dsv->Release();
		return false;
	}

	const UINT restore_count = replay_restore_rtv_count(original_rtvs);
	const uint64_t original_resource_id = render_target_resource_id(original_rtvs[0]);
	initialize_replay_targets(context, original_resource_id);

	ID3D11BlendState *replay_blend = lens_only ? g.replay.opaque_blend.Get() :
		(additive ? g.replay.additive_blend.Get() : g.replay.alpha_blend.Get());
	if (replay_blend == nullptr) {
		log_line("color replay skipped ps=%u: replay blend state is unavailable", pixel_hash);
		if (original_blend != nullptr) original_blend->Release();
		if (original_depth_state != nullptr) original_depth_state->Release();
		for (ID3D11RenderTargetView *rtv : original_rtvs) if (rtv != nullptr) rtv->Release();
		if (original_dsv != nullptr) original_dsv->Release();
		return false;
	}
	context->OMSetBlendState(replay_blend, original_factor, original_sample_mask);
	context->OMSetDepthStencilState(replay_depth_state.Get(), original_stencil_ref);
	std::vector<saved_lens_srv> lens_scene_srvs;
	ID3D11RenderTargetView *replay_rtv = g.replay.black_rtv.Get();
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, &replay_rtv, original_dsv,
		1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	if (lens_only) {
		// The lens shader samples the scene while writing the replay RT. Snapshot the
		// already-composited private backgrounds before binding the replacement SRVs.
		copy_scene_color_substitutes(context);
		bind_lens_scene_srvs(context, g.replay.scene_black_srv.Get(), lens_scene_srvs,
			g.replay.width, g.replay.height, true);
	}
	if (lens_only)
		log_line("lens scene SRVs substituted slots=%u", static_cast<unsigned>(lens_scene_srvs.size()));
	if (g.replay_draw_count < 8) {
		ID3D11RenderTargetView *bound_rtv = nullptr;
		ID3D11DepthStencilView *bound_dsv = nullptr;
		context->OMGetRenderTargets(1, &bound_rtv, &bound_dsv);
		ID3D11DepthStencilState *depth_state = nullptr;
		UINT stencil_ref = 0;
		context->OMGetDepthStencilState(&depth_state, &stencil_ref);
		D3D11_DEPTH_STENCIL_DESC bound_depth_desc = {};
		if (depth_state != nullptr)
			depth_state->GetDesc(&bound_depth_desc);
		log_line("replay draw ps=%u ctx=%u rtv=%p bound=%p original_dsv=%p bound_dsv=%p depth=%u func=%u write=%u stencil=%u",
			pixel_hash, static_cast<unsigned>(context_type), replay_rtv, bound_rtv,
			original_dsv, bound_dsv, bound_depth_desc.DepthEnable ? 1u : 0u,
			static_cast<unsigned>(bound_depth_desc.DepthFunc), bound_depth_desc.DepthWriteMask,
			bound_depth_desc.StencilEnable ? 1u : 0u);
		if (bound_rtv != nullptr)
			bound_rtv->Release();
		if (bound_dsv != nullptr)
			bound_dsv->Release();
		if (depth_state != nullptr)
			depth_state->Release();
	}
	++g_replay_depth;
	context->DrawIndexedInstanced(index_count, instance_count, first_index,
		vertex_offset, first_instance);
	replay_rtv = g.replay.white_rtv.Get();
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, &replay_rtv, original_dsv,
		1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	if (lens_only)
		bind_lens_scene_srvs(context, g.replay.scene_white_srv.Get(), lens_scene_srvs,
			g.replay.width, g.replay.height, false);
	context->DrawIndexedInstanced(index_count, instance_count, first_index,
		vertex_offset, first_instance);
	if (lens_only)
		restore_lens_scene_srvs(context, lens_scene_srvs);
	context->OMSetRenderTargetsAndUnorderedAccessViews(restore_count,
		original_rtvs.data(), original_dsv, restore_count,
		D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	context->OMSetBlendState(replay_blend, original_factor, original_sample_mask);
	context->OMSetDepthStencilState(replay_depth_state.Get(), original_stencil_ref);
	context->DrawIndexedInstanced(index_count, instance_count, first_index,
		vertex_offset, first_instance);
	--g_replay_depth;
	context->OMSetDepthStencilState(original_depth_state, original_stencil_ref);
	context->OMSetBlendState(original_blend, original_factor, original_sample_mask);

	if (original_blend != nullptr)
		original_blend->Release();
	if (original_depth_state != nullptr)
		original_depth_state->Release();
	for (ID3D11RenderTargetView *rtv : original_rtvs)
		if (rtv != nullptr)
			rtv->Release();
	if (original_dsv != nullptr)
		original_dsv->Release();

	g.replay_frame_has_draws = true;
	++g.replay_draw_count;
	return true;
}

bool replay_nonindexed_composite_draw(command_list *cmd_list, uint32_t vertex_count,
	uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance,
	uint32_t pixel_hash, uint32_t vertex_hash) {
	ID3D11DeviceContext *context = cmd_list == nullptr ? nullptr :
		reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
	if (context == nullptr)
		return false;
	std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> original_rtvs = {};
	ID3D11DepthStencilView *original_dsv = nullptr;
	context->OMGetRenderTargets(static_cast<UINT>(original_rtvs.size()),
		original_rtvs.data(), &original_dsv);
	if (original_rtvs[0] == nullptr || original_dsv != nullptr) {
		for (ID3D11RenderTargetView *rtv : original_rtvs)
			if (rtv != nullptr) rtv->Release();
		if (original_dsv != nullptr) original_dsv->Release();
		return false;
	}
	ID3D11BlendState *original_blend = nullptr;
	FLOAT original_factor[4] = {};
	UINT original_sample_mask = 0;
	context->OMGetBlendState(&original_blend, original_factor, &original_sample_mask);
	D3D11_BLEND_DESC original_blend_desc = {};
	if (original_blend == nullptr) {
		log_line("non-indexed replay skipped ps=%u: blend state is null", pixel_hash);
		for (ID3D11RenderTargetView *rtv : original_rtvs)
			if (rtv != nullptr) rtv->Release();
		return false;
	}
	original_blend->GetDesc(&original_blend_desc);
	const D3D11_RENDER_TARGET_BLEND_DESC &blend = original_blend_desc.RenderTarget[0];
	if (!blend.BlendEnable || blend.SrcBlend != D3D11_BLEND_ONE ||
		blend.DestBlend != D3D11_BLEND_INV_SRC_ALPHA ||
		blend.BlendOp != D3D11_BLEND_OP_ADD || blend.RenderTargetWriteMask != 0x07) {
		log_line("non-indexed replay skipped ps=%u: blend mismatch", pixel_hash);
		original_blend->Release();
		for (ID3D11RenderTargetView *rtv : original_rtvs)
			if (rtv != nullptr) rtv->Release();
		return false;
	}
	ComPtr<ID3D11Resource> target_resource;
	original_rtvs[0]->GetResource(&target_resource);
	ComPtr<ID3D11Texture2D> target_texture;
	D3D11_TEXTURE2D_DESC target_desc = {};
	if (target_resource != nullptr && SUCCEEDED(target_resource.As(&target_texture)))
		target_texture->GetDesc(&target_desc);
	const bool reuse_captured_scene = g.replay_frame_started;
	if (reuse_captured_scene && target_desc.Width != 0 && target_desc.Height != 0 &&
		(g.replay.width != target_desc.Width || g.replay.height != target_desc.Height))
		log_line("non-indexed composite target=%ux%u reusing captured scene=%ux%u without reset",
			target_desc.Width, target_desc.Height, g.replay.width, g.replay.height);
	ID3D11DepthStencilState *original_depth_state = nullptr;
	UINT original_stencil_ref = 0;
	context->OMGetDepthStencilState(&original_depth_state, &original_stencil_ref);
	std::string error;
	ComPtr<ID3D11DepthStencilState> replay_depth_state;
	if ((!reuse_captured_scene &&
		 !ensure_replay_resources(context, original_rtvs[0], nullptr, error, true)) ||
		!make_replay_depth_state(g.replay.device.Get(), original_depth_state,
			replay_depth_state, false, error)) {
		log_line("non-indexed replay skipped ps=%u: %s", pixel_hash, error.c_str());
		if (original_depth_state != nullptr) original_depth_state->Release();
		original_blend->Release();
		for (ID3D11RenderTargetView *rtv : original_rtvs)
			if (rtv != nullptr) rtv->Release();
		return false;
	}
	const UINT restore_count = replay_restore_rtv_count(original_rtvs);
	const uint64_t original_resource_id = render_target_resource_id(original_rtvs[0]);
	if (!reuse_captured_scene)
		initialize_replay_targets(context, original_resource_id);
	context->OMSetBlendState(original_blend, original_factor, original_sample_mask);
	context->OMSetDepthStencilState(replay_depth_state.Get(), original_stencil_ref);
	++g_replay_depth;
	ID3D11RenderTargetView *replay_rtv = g.replay.black_rtv.Get();
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, &replay_rtv, nullptr,
		1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	context->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
	replay_rtv = g.replay.white_rtv.Get();
	context->OMSetRenderTargetsAndUnorderedAccessViews(1, &replay_rtv, nullptr,
		1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	context->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
	context->OMSetRenderTargetsAndUnorderedAccessViews(restore_count,
		original_rtvs.data(), nullptr, restore_count,
		D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
	context->OMSetBlendState(original_blend, original_factor, original_sample_mask);
	context->OMSetDepthStencilState(original_depth_state, original_stencil_ref);
	context->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
	--g_replay_depth;
	context->OMSetDepthStencilState(original_depth_state, original_stencil_ref);
	context->OMSetBlendState(original_blend, original_factor, original_sample_mask);
	if (original_depth_state != nullptr) original_depth_state->Release();
	original_blend->Release();
	for (ID3D11RenderTargetView *rtv : original_rtvs)
		if (rtv != nullptr) rtv->Release();
	g.replay_frame_has_draws = true;
	++g.replay_draw_count;
	++g.replay_nonindexed_draw_count;
	log_line("non-indexed composite replayed ps=%u vs=%u vertices=%u instances=%u target=%ux%u dsv=null blend=one_inv_src_alpha",
		pixel_hash, vertex_hash, vertex_count, instance_count,
		g.replay.width, g.replay.height);
	return true;
}

bool hotkey_conflicts(uint32_t key, uint32_t modifiers, int ignored_group,
	bool ignore_capture, bool ignore_reload) {
	if (key == 0)
		return false;
	const uint32_t packed = pack_toggle_key(key, modifiers);
	if (!ignore_capture && packed == pack_toggle_key(g.cfg.capture_key, g.cfg.capture_modifiers))
		return true;
	if (!ignore_reload && packed == pack_toggle_key(g.cfg.reload_key, g.cfg.reload_modifiers))
		return true;
	for (size_t index = 0; index < g.rule_groups.size(); ++index) {
		if (static_cast<int>(index) != ignored_group &&
			g.rule_groups[index].toggle_key_packed != 0 &&
			g.rule_groups[index].toggle_key_packed == packed)
			return true;
	}
	return false;
}

bool read_replay_rgba32f(ID3D11Texture2D *source, rgba32f_image &image, std::string &error) {
	if (source == nullptr || g.replay.device == nullptr) {
		error = "no color replay frame is available";
		return false;
	}

	D3D11_TEXTURE2D_DESC source_desc = {};
	source->GetDesc(&source_desc);
	D3D11_TEXTURE2D_DESC staging_desc = source_desc;
	staging_desc.Usage = D3D11_USAGE_STAGING;
	staging_desc.BindFlags = 0;
	staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	staging_desc.MiscFlags = 0;
	ComPtr<ID3D11Texture2D> staging;
	HRESULT result = g.replay.device->CreateTexture2D(&staging_desc, nullptr, &staging);
	if (FAILED(result)) {
		error = "RGBA32F staging texture creation failed";
		return false;
	}

	ComPtr<ID3D11DeviceContext> context;
	g.replay.device->GetImmediateContext(&context);
	if (context == nullptr) {
		error = "D3D11 immediate context unavailable for color replay";
		return false;
	}
	context->CopyResource(staging.Get(), source);
	context->Flush();
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	result = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(result)) {
		error = "RGBA32F staging texture map failed";
		return false;
	}

	const size_t row_size = static_cast<size_t>(source_desc.Width) * 4 * sizeof(float);
	if (mapped.RowPitch < row_size) {
		context->Unmap(staging.Get(), 0);
		error = "RGBA32F mapped row pitch is too small";
		return false;
	}
	image.width = source_desc.Width;
	image.height = source_desc.Height;
	image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
	const auto *source_row = static_cast<const uint8_t *>(mapped.pData);
	for (UINT y = 0; y < image.height; ++y)
		memcpy(image.pixels.data() + static_cast<size_t>(y) * image.width * 4,
			source_row + static_cast<size_t>(y) * mapped.RowPitch, row_size);
	context->Unmap(staging.Get(), 0);
	return true;
}

bool save_rgba32f_binary(const std::wstring &path, const rgba32f_image &image,
	std::string &error) {
	FILE *file = nullptr;
	if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
		error = "cannot open RGBA32F output";
		return false;
	}
	const uint32_t header[4] = { 0x32414252u, image.width, image.height, 4u };
	const bool ok = fwrite(header, sizeof(header), 1, file) == 1 &&
		(image.pixels.empty() || fwrite(image.pixels.data(), sizeof(float), image.pixels.size(), file) == image.pixels.size());
	fclose(file);
	if (!ok) {
		DeleteFileW(path.c_str());
		error = "cannot write RGBA32F output";
		return false;
	}
	return true;
}

float clamp_unit(float value) {
	return std::max(0.0f, std::min(1.0f, value));
}

rgba_image make_final_rgba_image(const rgba32f_image &source) {
	rgba_image image;
	image.width = source.width;
	image.height = source.height;
	image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
	for (size_t index = 0; index < source.pixels.size(); index += 4) {
		const float alpha = clamp_unit(source.pixels[index + 3]);
		const float divisor = alpha > 0.000001f ? alpha : 1.0f;
		for (size_t channel = 0; channel < 3; ++channel) {
			const float straight = alpha > 0.000001f ?
				source.pixels[index + channel] / divisor : 0.0f;
			image.pixels[index + channel] = static_cast<uint8_t>(
				std::lround(clamp_unit(straight) * 255.0f));
		}
		image.pixels[index + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
	}
	return image;
}

rgba_image make_alpha_image(const rgba32f_image &source) {
	rgba_image image;
	image.width = source.width;
	image.height = source.height;
	image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
	for (size_t index = 0; index < source.pixels.size(); index += 4) {
		const uint8_t alpha = static_cast<uint8_t>(std::lround(
			clamp_unit(source.pixels[index + 3]) * 255.0f));
		image.pixels[index + 0] = alpha;
		image.pixels[index + 1] = alpha;
		image.pixels[index + 2] = alpha;
		image.pixels[index + 3] = 255;
	}
	return image;
}

rgba_image make_display_image(const rgba32f_image &source) {
	rgba_image image;
	image.width = source.width;
	image.height = source.height;
	image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
	for (size_t index = 0; index < source.pixels.size(); index += 4) {
		for (size_t channel = 0; channel < 3; ++channel)
			image.pixels[index + channel] = static_cast<uint8_t>(
				std::lround(clamp_unit(source.pixels[index + channel]) * 255.0f));
		image.pixels[index + 3] = 255;
	}
	return image;
}

rgba_image reconstruct_black_white_rgba(const rgba32f_image &black,
	const rgba32f_image &white) {
	rgba_image image;
	image.width = black.width;
	image.height = black.height;
	image.pixels.resize(static_cast<size_t>(image.width) * image.height * 4);
	for (size_t index = 0; index < black.pixels.size(); index += 4) {
		const float uncovered = clamp_unit(((white.pixels[index + 0] - black.pixels[index + 0]) +
			(white.pixels[index + 1] - black.pixels[index + 1]) +
			(white.pixels[index + 2] - black.pixels[index + 2])) / 3.0f);
		float alpha = 1.0f - uncovered;
		// Pure additive light has no conventional alpha. Preserve it in the RGBA preview
		// while the exact black/white pair remains available for lossless compositing.
		alpha = std::max(alpha, std::max({ black.pixels[index + 0],
			black.pixels[index + 1], black.pixels[index + 2] }));
		alpha = clamp_unit(alpha);
		for (size_t channel = 0; channel < 3; ++channel) {
			const float straight = alpha > 0.000001f ? black.pixels[index + channel] / alpha : 0.0f;
			image.pixels[index + channel] = static_cast<uint8_t>(
				std::lround(clamp_unit(straight) * 255.0f));
		}
		image.pixels[index + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
	}
	return image;
}

bool capture_lens_effect_outputs(effect_runtime *runtime, const std::wstring &prefix,
	std::string &error) {
	rgba_image composite;
	rgba_image black;
	rgba_image white;
	rgba_image alpha;
	rgba_image rgba;
	if (!read_effect_texture_rgba(runtime, "NS_AlphaBase.fx", "NS_LENS_Composite",
		composite, error))
		return false;
	if (!make_lens_composite_outputs(composite, black, white, alpha, rgba)) {
		error = "lens composite texture is empty";
		return false;
	}

	const std::wstring black_path = prefix + L"_lens_black.png";
	const std::wstring white_path = prefix + L"_lens_white.png";
	const std::wstring alpha_path = prefix + L"_lens_alpha.png";
	const std::wstring rgba_path = prefix + L"_lens_rgba.png";
	bool success = save_texture_png(black_path, black, error);
	if (success)
		success = save_texture_png(white_path, white, error);
	if (success)
		success = save_texture_png(alpha_path, alpha, error);
	if (success)
		success = save_texture_png(rgba_path, rgba, error);
	if (!success) {
		DeleteFileW(black_path.c_str());
		DeleteFileW(white_path.c_str());
		DeleteFileW(alpha_path.c_str());
		DeleteFileW(rgba_path.c_str());
		return false;
	}

	log_line("lens composite outputs saved: %ux%u id=%06llu", black.width, black.height,
		static_cast<unsigned long long>(g.capture_serial));
	return true;
}

bool capture_replay_outputs(effect_runtime *runtime) {
	if (!g.config_loaded)
		return capture_failure(g.config_error.empty() ? "configuration is unavailable" : g.config_error);
	if (!g.replay_frame_has_draws)
		return capture_failure(text("no target color draw was replayed in the current frame",
			"当前帧没有重放到目标颜色绘制"));
	if (!ensure_output_directory()) {
		char directory_error[96] = {};
		sprintf_s(directory_error, "cannot create output directory (win32=%lu)", GetLastError());
		return capture_failure(directory_error);
	}

	rgba32f_image black_raw;
	rgba32f_image white_raw;
	std::string error;
	if (!read_replay_rgba32f(g.replay.black_texture.Get(), black_raw, error) ||
		!read_replay_rgba32f(g.replay.white_texture.Get(), white_raw, error))
		return capture_failure(error);
	if (black_raw.width != white_raw.width || black_raw.height != white_raw.height)
		return capture_failure("black/white replay target dimensions differ");
	const rgba_image rgba = reconstruct_black_white_rgba(black_raw, white_raw);
	uint64_t black_nonzero_pixels = 0;
	uint64_t white_nonwhite_pixels = 0;
	uint64_t alpha_nontrivial_pixels = 0;
	for (size_t index = 0; index < black_raw.pixels.size(); index += 4) {
		const float uncovered = clamp_unit(((white_raw.pixels[index + 0] - black_raw.pixels[index + 0]) +
			(white_raw.pixels[index + 1] - black_raw.pixels[index + 1]) +
			(white_raw.pixels[index + 2] - black_raw.pixels[index + 2])) / 3.0f);
		const float alpha = 1.0f - uncovered;
		const float black_luma = std::max({ black_raw.pixels[index + 0],
			black_raw.pixels[index + 1], black_raw.pixels[index + 2] });
		const float white_delta = std::max({
			std::abs(white_raw.pixels[index + 0] - 1.0f),
			std::abs(white_raw.pixels[index + 1] - 1.0f),
			std::abs(white_raw.pixels[index + 2] - 1.0f) });
		if (black_luma > 0.0001f)
			++black_nonzero_pixels;
		if (white_delta > 0.0001f)
			++white_nonwhite_pixels;
		if (alpha > 0.0001f && alpha < 0.9999f)
			++alpha_nontrivial_pixels;
	}
	const rgba_image black_rgba = make_display_image(black_raw);
	const rgba_image white_rgba = make_display_image(white_raw);
	const rgba_image &final_rgba = rgba;
	const std::wstring prefix = make_capture_prefix(runtime);
	const std::wstring black_path = prefix + L"_Black.png";
	const std::wstring white_path = prefix + L"_White.png";
	const std::wstring final_path = prefix + L"_Final.png";
	bool success = true;
	if (g.cfg.output_black)
		success = save_texture_png(black_path, black_rgba, error);
	if (success && g.cfg.output_white)
		success = save_texture_png(white_path, white_rgba, error);
	if (success && g.cfg.output_transparent)
		success = save_texture_png(final_path, final_rgba, error);
	if (success) {
		log_line("capture pixels: black_nonzero=%llu white_nonwhite=%llu alpha_nontrivial=%llu",
			static_cast<unsigned long long>(black_nonzero_pixels),
			static_cast<unsigned long long>(white_nonwhite_pixels),
			static_cast<unsigned long long>(alpha_nontrivial_pixels));
		log_line("selected images saved: black=%u white=%u transparent=%u size=%ux%u indexed_draws=%u nonindexed_draws=%u clears=%u original_dsv_read_only=1 id=%06llu",
			g.cfg.output_black ? 1u : 0u, g.cfg.output_white ? 1u : 0u,
			g.cfg.output_transparent ? 1u : 0u,
			black_raw.width, black_raw.height, g.replay_draw_count,
			g.replay_nonindexed_draw_count, g.replay_clear_count,
			static_cast<unsigned long long>(g.capture_serial));
		char success_message[160] = {};
		sprintf_s(success_message, "%s #%06llu",
			text("NS Alpha Capture - Selected images saved", "NS Alpha Capture - 已保存所选图片"),
			static_cast<unsigned long long>(g.capture_serial));
		show_notification(true, success_message);
	} else {
		if (g.cfg.output_black)
			DeleteFileW(black_path.c_str());
		if (g.cfg.output_white)
			DeleteFileW(white_path.c_str());
		if (g.cfg.output_transparent)
			DeleteFileW(final_path.c_str());
		capture_failure("color replay output failed: " + error);
	}
	return success;
}

void on_init_pipeline(device *, pipeline_layout, uint32_t subobject_count,
	const pipeline_subobject *subobjects, pipeline pipeline_handle) {
	shader_hashes discovered;
	for (uint32_t index = 0; index < subobject_count; ++index) {
		const pipeline_subobject &subobject = subobjects[index];
		if (subobject.type != pipeline_subobject_type::pixel_shader &&
			subobject.type != pipeline_subobject_type::vertex_shader)
			continue;
		const auto shader = static_cast<const shader_desc *>(subobject.data);
		if (shader == nullptr || shader->code == nullptr || shader->code_size == 0)
			continue;
		const uint32_t hash = compute_crc32(
			static_cast<const uint8_t *>(shader->code), shader->code_size);
		if (subobject.type == pipeline_subobject_type::pixel_shader)
			discovered.pixel = hash;
		else
			discovered.vertex = hash;
		if (subobject.type == pipeline_subobject_type::pixel_shader &&
			shader_contains_discard(shader->code, shader->code_size)) {
			std::lock_guard<std::mutex> lock(g.pipeline_mutex);
			g.discard_shader_hashes.insert(hash);
		}
	}

	if (discovered.pixel == 0 && discovered.vertex == 0)
		return;
	std::lock_guard<std::mutex> lock(g.pipeline_mutex);
	shader_hashes &stored = g.pipeline_hashes[pipeline_handle.handle];
	if (discovered.pixel != 0)
		stored.pixel = discovered.pixel;
	if (discovered.vertex != 0)
		stored.vertex = discovered.vertex;
}

void on_destroy_pipeline(device *, pipeline pipeline_handle) {
	std::lock_guard<std::mutex> lock(g.pipeline_mutex);
	g.pipeline_hashes.erase(pipeline_handle.handle);
}

void on_bind_pipeline(command_list *cmd_list, pipeline_stage stages, pipeline pipeline_handle) {
	if (cmd_list == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g.pipeline_mutex);
	command_state &command = g.command_states[command_key(cmd_list)];
	if ((stages & pipeline_stage::pixel_shader) == pipeline_stage::pixel_shader)
		command.pixel_pipeline = pipeline_handle.handle;
	if ((stages & pipeline_stage::vertex_shader) == pipeline_stage::vertex_shader)
		command.vertex_pipeline = pipeline_handle.handle;
}

void on_destroy_command_list(command_list *cmd_list) {
	if (cmd_list == nullptr)
		return;
	std::lock_guard<std::mutex> lock(g.pipeline_mutex);
	g.command_states.erase(command_key(cmd_list));
}

shader_hashes current_shader_hashes(command_list *cmd_list) {
	std::lock_guard<std::mutex> lock(g.pipeline_mutex);
	const auto command_it = g.command_states.find(command_key(cmd_list));
	if (command_it == g.command_states.end())
		return {};
	shader_hashes result;
	const auto pixel_it = g.pipeline_hashes.find(command_it->second.pixel_pipeline);
	if (pixel_it != g.pipeline_hashes.end())
		result.pixel = pixel_it->second.pixel;
	const auto vertex_it = g.pipeline_hashes.find(command_it->second.vertex_pipeline);
	if (vertex_it != g.pipeline_hashes.end())
		result.vertex = vertex_it->second.vertex;
	return result;
}

using disassemble_fn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob **);

bool shader_contains_discard(const void *code, size_t code_size) {
	if (code == nullptr || code_size == 0)
		return false;
	static HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
	if (compiler == nullptr)
		return false;
	static const auto disassemble = reinterpret_cast<disassemble_fn>(
		GetProcAddress(compiler, "D3DDisassemble"));
	ID3DBlob *assembly = nullptr;
	const bool success = disassemble != nullptr && SUCCEEDED(disassemble(
		code, code_size, D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS,
		nullptr, &assembly)) && assembly != nullptr;
	bool has_discard = false;
	if (success) {
		const char *text = static_cast<const char *>(assembly->GetBufferPointer());
		const size_t length = assembly->GetBufferSize();
		const std::string source(text, length);
		has_discard = source.find("discard_nz") != std::string::npos ||
			source.find("discard_z") != std::string::npos;
		assembly->Release();
	}
	return has_discard;
}

bool query_mesh_signature(ID3D11DeviceContext *context, const draw_arguments &arguments,
	mesh_signature &mesh) {
	if (context == nullptr)
		return false;
	ID3D11Buffer *index_buffer = nullptr;
	DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
	UINT index_offset = 0;
	context->IAGetIndexBuffer(&index_buffer, &index_format, &index_offset);
	ID3D11Buffer *vertex_buffer = nullptr;
	UINT vertex_stride = 0;
	UINT vertex_offset = 0;
	context->IAGetVertexBuffers(0, 1, &vertex_buffer, &vertex_stride, &vertex_offset);
	mesh.index_buffer = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(index_buffer));
	mesh.index_offset = index_offset;
	mesh.index_format = static_cast<uint32_t>(index_format);
	mesh.vertex_buffer0 = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vertex_buffer));
	mesh.vertex_offset0 = vertex_offset;
	mesh.vertex_stride0 = vertex_stride;
	mesh.first_index = arguments.first_index;
	mesh.index_count = arguments.index_count;
	mesh.vertex_offset = arguments.vertex_offset;
	if (index_buffer != nullptr)
		index_buffer->Release();
	if (vertex_buffer != nullptr)
		vertex_buffer->Release();
	return mesh.index_buffer != 0 && mesh.vertex_buffer0 != 0;
}

bool query_bound_a8_beacon(ID3D11DeviceContext *context) {
	if (context == nullptr)
		return false;
	constexpr UINT kProbeResourceSlots = 16;
	ID3D11ShaderResourceView *views[kProbeResourceSlots] = {};
	context->PSGetShaderResources(0, kProbeResourceSlots, views);
	bool found = false;
	for (ID3D11ShaderResourceView *view : views) {
		if (view == nullptr)
			continue;
		ID3D11Resource *resource = nullptr;
		view->GetResource(&resource);
		ID3D11Texture2D *texture = nullptr;
		if (resource != nullptr)
			resource->QueryInterface(__uuidof(ID3D11Texture2D),
				reinterpret_cast<void **>(&texture));
		if (texture != nullptr) {
			D3D11_TEXTURE2D_DESC desc = {};
			texture->GetDesc(&desc);
			if (desc.Width == 4 && desc.Height == 4 && desc.ArraySize == 1 &&
				desc.MipLevels == 1 && desc.SampleDesc.Count == 1 &&
				desc.Format == DXGI_FORMAT_A8_UNORM)
				found = true;
			texture->Release();
		}
		if (resource != nullptr)
			resource->Release();
		view->Release();
	}
	return found;
}

bool query_runtime_dither_state(ID3D11DeviceContext *context, const draw_arguments &arguments,
	mesh_signature &mesh) {
	if (context == nullptr || !query_bound_a8_beacon(context) ||
		!query_mesh_signature(context, arguments, mesh))
		return false;
	ID3D11RenderTargetView *rtv = nullptr;
	ID3D11DepthStencilView *dsv = nullptr;
	context->OMGetRenderTargets(1, &rtv, &dsv);
	ID3D11BlendState *blend = nullptr;
	FLOAT factor[4] = {};
	UINT sample_mask = 0;
	context->OMGetBlendState(&blend, factor, &sample_mask);
	ID3D11DepthStencilState *depth = nullptr;
	UINT stencil_ref = 0;
	context->OMGetDepthStencilState(&depth, &stencil_ref);
	D3D11_RENDER_TARGET_BLEND_DESC blend_desc = {};
	if (blend != nullptr) {
		D3D11_BLEND_DESC description = {};
		blend->GetDesc(&description);
		blend_desc = description.RenderTarget[0];
	}
	D3D11_DEPTH_STENCIL_DESC depth_desc = {};
	if (depth != nullptr)
		depth->GetDesc(&depth_desc);
	const bool result = dsv != nullptr && rtv != nullptr && blend_desc.RenderTargetWriteMask == 0 &&
		depth_desc.DepthEnable && depth_desc.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO;
	if (rtv != nullptr)
		rtv->Release();
	if (dsv != nullptr)
		dsv->Release();
	if (blend != nullptr)
		blend->Release();
	if (depth != nullptr)
		depth->Release();
	return result;
}

uint64_t current_render_target_resource_id(ID3D11DeviceContext *context) {
	if (context == nullptr)
		return 0;
	ID3D11RenderTargetView *rtv = nullptr;
	context->OMGetRenderTargets(1, &rtv, nullptr);
	const uint64_t result = render_target_resource_id(rtv);
	if (rtv != nullptr)
		rtv->Release();
	return result;
}

bool current_render_target_is_learned(ID3D11DeviceContext *context) {
	if (context == nullptr || g.learned_scene_targets.empty())
		return false;
	ID3D11RenderTargetView *rtv = nullptr;
	context->OMGetRenderTargets(1, &rtv, nullptr);
	ComPtr<ID3D11Resource> resource;
	if (rtv != nullptr)
		rtv->GetResource(&resource);
	if (rtv != nullptr)
		rtv->Release();
	if (resource == nullptr)
		return false;
	for (const ComPtr<ID3D11Resource> &learned : g.learned_scene_targets) {
		if (learned.Get() == resource.Get())
			return true;
	}
	return false;
}

void remember_current_render_target(ID3D11DeviceContext *context) {
	if (context == nullptr)
		return;
	ID3D11RenderTargetView *rtv = nullptr;
	context->OMGetRenderTargets(1, &rtv, nullptr);
	ComPtr<ID3D11Resource> resource;
	if (rtv != nullptr)
		rtv->GetResource(&resource);
	if (rtv != nullptr)
		rtv->Release();
	if (resource == nullptr)
		return;
	for (const ComPtr<ID3D11Resource> &learned : g.learned_scene_targets) {
		if (learned.Get() == resource.Get())
			return;
	}
	if (g.learned_scene_targets.size() < 64)
		g.learned_scene_targets.push_back(std::move(resource));
}

void record_shader_candidate(ID3D11DeviceContext *context, const shader_hashes &hashes,
	const draw_arguments &arguments) {
	if (!g.shader_selector_active || context == nullptr)
		return;
	const uint64_t render_target = current_render_target_resource_id(context);
	if (render_target == 0)
		return;
	for (shader_candidate &candidate : g.shader_candidates) {
		if (candidate.render_target == render_target && candidate.rule.pixel == hashes.pixel &&
			candidate.rule.vertex == hashes.vertex &&
			candidate.rule.first_index == arguments.first_index &&
			candidate.rule.index_count == arguments.index_count &&
			candidate.rule.vertex_offset == arguments.vertex_offset) {
			++candidate.draw_count;
			return;
		}
	}
	if (g.shader_candidates.size() >= 256)
		return;
	shader_candidate candidate;
	candidate.rule.pixel = hashes.pixel;
	candidate.rule.vertex = hashes.vertex;
	candidate.rule.first_index = arguments.first_index;
	candidate.rule.index_count = arguments.index_count;
	candidate.rule.vertex_offset = arguments.vertex_offset;
	candidate.draw_count = 1;
	candidate.render_target = render_target;
	g.shader_candidates.push_back(std::move(candidate));
}

bool query_color_replay_state(ID3D11DeviceContext *context, const mesh_signature &mesh,
	bool &additive) {
	if (context == nullptr)
		return false;
	ID3D11RenderTargetView *rtv = nullptr;
	ID3D11DepthStencilView *dsv = nullptr;
	context->OMGetRenderTargets(1, &rtv, &dsv);
	ID3D11BlendState *blend = nullptr;
	FLOAT factor[4] = {};
	UINT sample_mask = 0;
	context->OMGetBlendState(&blend, factor, &sample_mask);
	D3D11_RENDER_TARGET_BLEND_DESC blend_desc = {};
	if (blend != nullptr) {
		D3D11_BLEND_DESC description = {};
		blend->GetDesc(&description);
		blend_desc = description.RenderTarget[0];
	}
	D3D11_TEXTURE2D_DESC rtv_desc = {};
	ID3D11Resource *resource = nullptr;
	ID3D11Texture2D *texture = nullptr;
	if (rtv != nullptr)
		rtv->GetResource(&resource);
	if (resource != nullptr)
		resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
	if (texture != nullptr)
		texture->GetDesc(&rtv_desc);
	const bool ordinary = blend_desc.BlendEnable &&
		blend_desc.SrcBlend == D3D11_BLEND_SRC_ALPHA &&
		blend_desc.DestBlend == D3D11_BLEND_INV_SRC_ALPHA;
	const bool additive_blend = blend_desc.BlendEnable &&
		blend_desc.SrcBlend == D3D11_BLEND_ONE &&
		blend_desc.DestBlend == D3D11_BLEND_ONE;
	const bool result = mesh.index_buffer != 0 && rtv != nullptr && dsv != nullptr &&
		rtv_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
		rtv_desc.SampleDesc.Count == 1 && blend_desc.RenderTargetWriteMask == 0x07 &&
		(ordinary || additive_blend);
	additive = additive_blend;
	if (texture != nullptr)
		texture->Release();
	if (resource != nullptr)
		resource->Release();
	if (rtv != nullptr)
		rtv->Release();
	if (dsv != nullptr)
		dsv->Release();
	if (blend != nullptr)
		blend->Release();
	return result;
}

bool query_lens_replay_state(ID3D11DeviceContext *context, const mesh_signature &mesh) {
	if (context == nullptr || mesh.index_buffer == 0)
		return false;
	ID3D11RenderTargetView *rtv = nullptr;
	ID3D11DepthStencilView *dsv = nullptr;
	context->OMGetRenderTargets(1, &rtv, &dsv);
	ID3D11BlendState *blend = nullptr;
	FLOAT factor[4] = {};
	UINT sample_mask = 0;
	context->OMGetBlendState(&blend, factor, &sample_mask);
	D3D11_RENDER_TARGET_BLEND_DESC blend_desc = {};
	if (blend != nullptr) {
		D3D11_BLEND_DESC description = {};
		blend->GetDesc(&description);
		blend_desc = description.RenderTarget[0];
	}
	D3D11_TEXTURE2D_DESC rtv_desc = {};
	ID3D11Resource *resource = nullptr;
	ID3D11Texture2D *texture = nullptr;
	if (rtv != nullptr)
		rtv->GetResource(&resource);
	if (resource != nullptr)
		resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture));
	if (texture != nullptr)
		texture->GetDesc(&rtv_desc);
	const bool result = rtv != nullptr && dsv != nullptr && texture != nullptr &&
		rtv_desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
		rtv_desc.SampleDesc.Count == 1 && blend_desc.RenderTargetWriteMask == 0x07 &&
		!blend_desc.BlendEnable;
	if (texture != nullptr) texture->Release();
	if (resource != nullptr) resource->Release();
	if (rtv != nullptr) rtv->Release();
	if (dsv != nullptr) dsv->Release();
	if (blend != nullptr) blend->Release();
	return result;
}

bool learned_mesh(const mesh_signature &mesh) {
	std::lock_guard<std::mutex> lock(g.pipeline_mutex);
	return g.learned_meshes.count(mesh) != 0;
}

bool is_lens_target(const shader_hashes &hashes, const mesh_signature &mesh) {
	return g.cfg.lens_capture && hashes.pixel == g.cfg.lens_pixel_shader_hash &&
		mesh.first_index == g.cfg.lens_first_index &&
		mesh.index_count == g.cfg.lens_index_count && mesh.vertex_offset == 0;
}

bool on_draw_indexed(command_list *cmd_list, uint32_t index_count, uint32_t instance_count,
	uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
	if (cmd_list == nullptr || g_replay_depth != 0)
		return false;
	const shader_hashes hashes = current_shader_hashes(cmd_list);
	const draw_arguments arguments = { 0, index_count, instance_count, 0, first_index,
		vertex_offset, first_instance };
	ID3D11DeviceContext *context = reinterpret_cast<ID3D11DeviceContext *>(
		cmd_list->get_native());
	record_shader_candidate(context, hashes, arguments);
	if (!g.replay_capture_active && g.preview.active &&
		ns_alpha_rules::preview_hides_draw(g.preview, g.rule_groups, hashes.pixel,
			hashes.vertex, first_index, index_count, vertex_offset))
		return true;
	if (g.replay_capture_active && g.cfg.lens_capture) {
		mesh_signature lens_mesh;
		if (query_mesh_signature(context, arguments, lens_mesh) &&
			is_lens_target(hashes, lens_mesh) && query_lens_replay_state(context, lens_mesh)) {
			return replay_color_draw(cmd_list, index_count, instance_count, first_index,
				vertex_offset, first_instance, hashes.pixel, true);
		}
	}
	const bool configured = is_configured_shader_rule(hashes, index_count, first_index,
		vertex_offset);
	if (configured) {
		mesh_signature configured_mesh;
		bool configured_additive = false;
		if (query_mesh_signature(context, arguments, configured_mesh) &&
			query_color_replay_state(context, configured_mesh, configured_additive)) {
			remember_current_render_target(context);
			if (g.replay_capture_active)
				return replay_color_draw(cmd_list, index_count, instance_count, first_index,
					vertex_offset, first_instance, hashes.pixel, false);
		}
	}
	if (g.cfg.auto_match) {
		bool discard_shader = false;
		{
			std::lock_guard<std::mutex> lock(g.pipeline_mutex);
			discard_shader = g.discard_shader_hashes.count(hashes.pixel) != 0;
		}
		if (discard_shader) {
			mesh_signature mesh;
			if (query_runtime_dither_state(context, arguments, mesh)) {
				const uint64_t scene_target = current_render_target_resource_id(context);
				if (scene_target != 0)
					remember_current_render_target(context);
				bool inserted = false;
				{
					std::lock_guard<std::mutex> lock(g.pipeline_mutex);
					if (g.learned_meshes.size() < 4096)
						inserted = g.learned_meshes.insert(mesh).second;
				}
				if (inserted)
					log_line("auto_match learned dither mesh ib=0x%llX first=%u count=%u vb0=0x%llX target=0x%llX",
						static_cast<unsigned long long>(mesh.index_buffer), mesh.first_index,
						mesh.index_count, static_cast<unsigned long long>(mesh.vertex_buffer0),
						static_cast<unsigned long long>(scene_target));
					if (g.replay_capture_active) {
						bool additive = false;
						if (query_color_replay_state(context, mesh, additive)) {
							return replay_color_draw(cmd_list, index_count, instance_count, first_index,
								vertex_offset, first_instance, hashes.pixel, false);
						}
						if (mirror_scene_draw(cmd_list, index_count, instance_count, first_index,
							vertex_offset, first_instance))
							return true;
					}
					return false;
			}
		}
		if (!g.replay_capture_active)
			return false;
		mesh_signature mesh;
		bool additive = false;
		if (query_mesh_signature(context, arguments, mesh) && learned_mesh(mesh) &&
			query_color_replay_state(context, mesh, additive)) {
			return replay_color_draw(cmd_list, index_count, instance_count, first_index,
				vertex_offset, first_instance, hashes.pixel, false);
		}
		mirror_scene_draw(cmd_list, index_count, instance_count, first_index,
			vertex_offset, first_instance);
		return false;
	}
	if (!g.replay_capture_active)
		return false;
	if (g.cfg.lens_only) {
		if (hashes.pixel != g.cfg.lens_pixel_shader_hash ||
			first_index != g.cfg.lens_first_index || index_count != g.cfg.lens_index_count ||
			vertex_offset != 0)
			return false;
	}
	if (is_target_depth_draw(hashes.pixel, hashes.vertex,
		index_count, first_index, vertex_offset)) {
		return false;
	}
	if (!g.cfg.lens_only && !is_configured_shader_rule(hashes, index_count, first_index,
		vertex_offset))
		return false;

	return replay_color_draw(cmd_list, index_count, instance_count, first_index,
		vertex_offset, first_instance, hashes.pixel, false);
}

bool on_draw(command_list *cmd_list, uint32_t vertex_count, uint32_t instance_count,
	uint32_t first_vertex, uint32_t first_instance) {
	if (cmd_list == nullptr || g_replay_depth != 0)
		return false;
	ID3D11DeviceContext *context = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
	if (context == nullptr)
		return false;
	ID3D11RenderTargetView *rtv = nullptr;
	ID3D11DepthStencilView *dsv = nullptr;
	context->OMGetRenderTargets(1, &rtv, &dsv);
	ID3D11BlendState *blend = nullptr;
	FLOAT factor[4] = {};
	UINT sample_mask = 0;
	context->OMGetBlendState(&blend, factor, &sample_mask);
	const shader_hashes hashes = current_shader_hashes(cmd_list);
	uint32_t target_width = 0;
	uint32_t target_height = 0;
	const bool auto_shape = g.cfg.auto_highlight &&
		is_auto_highlight_shape(vertex_count, instance_count, first_vertex, first_instance,
			rtv, dsv, blend, &target_width, &target_height);
	if (auto_shape)
		record_nonindexed_candidate(hashes, vertex_count, instance_count, first_vertex,
			first_instance, target_width, target_height);
	if (!g.replay_capture_active) {
		if (blend != nullptr) blend->Release();
		if (rtv != nullptr) rtv->Release();
		if (dsv != nullptr) dsv->Release();
		return false;
	}
	const bool configured = is_configured_nonindexed_rule(hashes, vertex_count, instance_count,
		first_vertex, first_instance, rtv, dsv, blend);
	const bool auto_learned = auto_shape && has_learned_nonindexed_candidate(hashes,
		vertex_count, instance_count, first_vertex, first_instance);
	if (configured || auto_learned) {
		const bool replayed = replay_nonindexed_composite_draw(cmd_list, vertex_count,
			instance_count, first_vertex, first_instance, hashes.pixel, hashes.vertex);
		if (blend != nullptr) blend->Release();
		if (rtv != nullptr) rtv->Release();
		if (dsv != nullptr) dsv->Release();
		return replayed;
	}
	if (current_render_target_is_learned(context))
		++g.replay_nonindexed_draw_count;
	if (blend != nullptr) blend->Release();
	if (rtv != nullptr) rtv->Release();
	if (dsv != nullptr) dsv->Release();
	return false;
}

bool on_clear_render_target_view(command_list *cmd_list, resource_view rtv,
	const float[4], uint32_t, const rect *) {
	if (cmd_list == nullptr || g_replay_depth != 0 || !g.replay_capture_active ||
		!g.replay_frame_started || rtv.handle == 0)
		return false;
	ID3D11RenderTargetView *native_rtv = reinterpret_cast<ID3D11RenderTargetView *>(
		static_cast<uintptr_t>(rtv.handle));
	if (render_target_resource_id(native_rtv) != g.replay_frame_target_resource)
		return false;
	ID3D11DeviceContext *context = reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native());
	if (context == nullptr || g.replay.black_rtv == nullptr || g.replay.white_rtv == nullptr)
		return false;
	const float black_clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	const float white_clear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	++g_replay_depth;
	context->ClearRenderTargetView(g.replay.black_rtv.Get(), black_clear);
	context->ClearRenderTargetView(g.replay.white_rtv.Get(), white_clear);
	--g_replay_depth;
	++g.replay_clear_count;
	log_line("scene RT clear mirrored: count=%u", g.replay_clear_count);
	return false;
}

void reset_replay_frame_state() {
	g.replay_frame_started = false;
	g.replay_frame_has_draws = false;
	g.replay_frame_target_resource = 0;
	g.replay_draw_count = 0;
	g.replay_nonindexed_draw_count = 0;
	g.replay_clear_count = 0;
}

resource_view native_srv(ID3D11ShaderResourceView *view) {
	return { static_cast<uint64_t>(reinterpret_cast<uintptr_t>(view)) };
}

void update_replay_texture_bindings(effect_runtime *runtime) {
	if (runtime == nullptr || g.replay.black_srv == nullptr || g.replay.white_srv == nullptr)
		return;
	const resource_view black = native_srv(g.replay.black_srv.Get());
	const resource_view white = native_srv(g.replay.white_srv.Get());
	runtime->update_texture_bindings("NS_ALPHA_CAPTURE_BLACK", black, black);
	runtime->update_texture_bindings("NS_ALPHA_CAPTURE_WHITE", white, white);
}

void on_reshade_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view) {
	update_replay_texture_bindings(runtime);
}

void on_reshade_reloaded_effects(effect_runtime *runtime) {
	update_replay_texture_bindings(runtime);
}

bool imgui_key_to_virtual_key(ImGuiKey key, uint32_t &virtual_key) {
	if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
		virtual_key = 'A' + static_cast<uint32_t>(key - ImGuiKey_A);
		return true;
	}
	if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
		virtual_key = '0' + static_cast<uint32_t>(key - ImGuiKey_0);
		return true;
	}
	if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
		virtual_key = VK_F1 + static_cast<uint32_t>(key - ImGuiKey_F1);
		return true;
	}
	switch (key) {
	case ImGuiKey_Tab: virtual_key = VK_TAB; return true;
	case ImGuiKey_LeftArrow: virtual_key = VK_LEFT; return true;
	case ImGuiKey_RightArrow: virtual_key = VK_RIGHT; return true;
	case ImGuiKey_UpArrow: virtual_key = VK_UP; return true;
	case ImGuiKey_DownArrow: virtual_key = VK_DOWN; return true;
	case ImGuiKey_PageUp: virtual_key = VK_PRIOR; return true;
	case ImGuiKey_PageDown: virtual_key = VK_NEXT; return true;
	case ImGuiKey_Home: virtual_key = VK_HOME; return true;
	case ImGuiKey_End: virtual_key = VK_END; return true;
	case ImGuiKey_Insert: virtual_key = VK_INSERT; return true;
	case ImGuiKey_Delete: virtual_key = VK_DELETE; return true;
	case ImGuiKey_Backspace: virtual_key = VK_BACK; return true;
	case ImGuiKey_Space: virtual_key = VK_SPACE; return true;
	case ImGuiKey_Enter: virtual_key = VK_RETURN; return true;
	case ImGuiKey_Escape: virtual_key = VK_ESCAPE; return true;
	default: return false;
	}
}

uint32_t current_imgui_modifiers() {
	uint32_t modifiers = ns_white_backing::modifier_none;
	if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl))
		modifiers |= ns_white_backing::modifier_ctrl;
	if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift))
		modifiers |= ns_white_backing::modifier_shift;
	if (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt))
		modifiers |= ns_white_backing::modifier_alt;
	return modifiers;
}

uint32_t current_runtime_modifiers(effect_runtime *runtime) {
	if (runtime == nullptr)
		return ns_white_backing::modifier_none;
	uint32_t modifiers = ns_white_backing::modifier_none;
	const auto is_down = [runtime](uint32_t key) { return runtime->is_key_down(key); };
	if (is_down(VK_CONTROL) || is_down(VK_LCONTROL) || is_down(VK_RCONTROL))
		modifiers |= ns_white_backing::modifier_ctrl;
	if (is_down(VK_SHIFT) || is_down(VK_LSHIFT) || is_down(VK_RSHIFT))
		modifiers |= ns_white_backing::modifier_shift;
	if (is_down(VK_MENU) || is_down(VK_LMENU) || is_down(VK_RMENU))
		modifiers |= ns_white_backing::modifier_alt;
	return modifiers;
}

bool runtime_hotkey_pressed(effect_runtime *runtime, uint32_t key, uint32_t modifiers) {
	if (runtime == nullptr || key == 0 || !runtime->is_key_pressed(key))
		return false;
	return current_runtime_modifiers(runtime) == modifiers;
}

void finish_hotkey_capture(uint32_t key, uint32_t modifiers) {
	modifiers = ns_white_backing::sanitize_modifiers(modifiers);
	if (g.hotkey_capture_target >= 100) {
		const size_t group_index = static_cast<size_t>(g.hotkey_capture_target - 100);
		const uint32_t packed = pack_toggle_key(key, modifiers);
		if (hotkey_conflicts(key, modifiers, static_cast<int>(group_index), false, false)) {
			show_notification(false, text("Shortcut is already in use", "快捷键已被使用"));
			g.hotkey_capture_target = 0;
			return;
		}
		if (g.group_editor_index == static_cast<int>(group_index)) {
			g.group_editor_work.toggle_key_packed = packed;
			show_notification(true, std::string(text("Toggle key staged: ", "组快捷键已暂存：")) +
				format_hotkey(key, modifiers));
		} else if (group_index < g.rule_groups.size()) {
			g.rule_groups[group_index].toggle_key_packed = packed;
			if (save_rule_groups())
				show_notification(true, std::string(text("Toggle key saved: ", "组快捷键已保存：")) +
					format_hotkey(key, modifiers));
		}
		g.hotkey_capture_target = 0;
		return;
	}
	const bool capture = g.hotkey_capture_target == 1;
	const char *key_name_value = capture ? "CaptureKey" : "ReloadKey";
	const char *modifiers_name = capture ? "CaptureModifiers" : "ReloadModifiers";
	if (hotkey_conflicts(key, modifiers, -1, capture, !capture)) {
		show_notification(false, text("Shortcut is already in use", "快捷键已被使用"));
		g.hotkey_capture_target = 0;
		return;
	}
	if (!save_hotkey(key_name_value, modifiers_name, key, modifiers)) {
		show_notification(false, capture_failure_message(text("could not save shortcut", "无法保存快捷键")));
		g.hotkey_capture_target = 0;
		return;
	}
	if (capture) {
		g.cfg.capture_key = key;
		g.cfg.capture_modifiers = modifiers;
	} else {
		g.cfg.reload_key = key;
		g.cfg.reload_modifiers = modifiers;
	}
	show_notification(true, std::string(text("NS Alpha Capture - Shortcut saved: ", "NS Alpha Capture - 快捷键已保存：")) +
		format_hotkey(key, modifiers));
	g.hotkey_capture_target = 0;
}

void process_hotkey_capture(effect_runtime *runtime) {
	if (g.hotkey_capture_target == 0 || runtime == nullptr)
		return;
	ImGui::SetNextFrameWantCaptureKeyboard(true);
	g.hotkey_modifier_latch |= current_imgui_modifiers() | current_runtime_modifiers(runtime);
	if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
		g.hotkey_capture_target = 0;
		g.hotkey_modifier_latch = ns_white_backing::modifier_none;
		g.hotkey_suppress_key = VK_ESCAPE;
		return;
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
		ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
		finish_hotkey_capture(0, ns_white_backing::modifier_none);
		g.hotkey_modifier_latch = ns_white_backing::modifier_none;
		g.hotkey_suppress_key = VK_BACK;
		return;
	}
	for (int value = ImGuiKey_NamedKey_BEGIN; value < ImGuiKey_NamedKey_END; ++value) {
		const ImGuiKey imgui_key = static_cast<ImGuiKey>(value);
		uint32_t virtual_key = 0;
		if (!imgui_key_to_virtual_key(imgui_key, virtual_key) || virtual_key == VK_ESCAPE ||
			virtual_key == VK_BACK || virtual_key == VK_DELETE ||
			!ImGui::IsKeyPressed(imgui_key, false))
			continue;
		finish_hotkey_capture(virtual_key,
			g.hotkey_modifier_latch | current_imgui_modifiers() | current_runtime_modifiers(runtime));
		g.hotkey_modifier_latch = ns_white_backing::modifier_none;
		g.hotkey_suppress_key = virtual_key;
		return;
	}
}


std::string format_hash_hex(uint32_t hash) {
	char buffer[16] = {};
	_snprintf_s(buffer, _TRUNCATE, "0x%08X", hash);
	return buffer;
}

void close_group_editors() {
	g.group_editor_index = -1;
	g.rule_editor_index = -1;
	g.rule_editor_error.clear();
}

void open_group_editor(size_t index) {
	if (index >= g.rule_groups.size())
		return;
	g.rule_editor_index = -1;
	g.rule_editor_error.clear();
	g.group_editor_work = g.rule_groups[index];
	g.group_editor_index = static_cast<int>(index);
	memset(g.group_name_input.data(), 0, g.group_name_input.size());
	strncpy_s(g.group_name_input.data(), g.group_name_input.size(),
		g.group_editor_work.name.c_str(), _TRUNCATE);
}

void open_rule_editor(size_t index) {
	if (index >= g.rule_groups.size())
		return;
	g.group_editor_index = -1;
	g.rule_editor_work = g.rule_groups[index].rules;
	g.rule_editor_names.clear();
	for (const ns_alpha_rules::capture_rule &rule : g.rule_groups[index].rules) {
		std::array<char, 64> name = {};
		strncpy_s(name.data(), name.size(), rule.name.c_str(), _TRUNCATE);
		g.rule_editor_names.push_back(name);
	}
	g.rule_editor_error.clear();
	g.rule_editor_index = static_cast<int>(index);
}

bool validate_rule_editor(std::string &error) {
	for (size_t index = 0; index < g.rule_editor_work.size(); ++index) {
		ns_alpha_rules::capture_rule &rule = g.rule_editor_work[index];
		if (rule.enabled && !ns_alpha_rules::rule_can_be_enabled(rule)) {
			error = std::string(text("Rule ", "规则 ")) + std::to_string(index) +
				text(" is enabled without a complete PS+VS+geometry signature",
					" 在缺少完整 PS+VS+几何签名时启用，已阻止");
			return false;
		}
		for (size_t other = index + 1; other < g.rule_editor_work.size(); ++other) {
			if (ns_alpha_rules::rule_signature_equals(rule, g.rule_editor_work[other])) {
				error = std::string(text("Rules ", "规则 ")) + std::to_string(index) +
					text(" and ", " 和 ") + std::to_string(other) +
					text(" are duplicates", " 重复");
				return false;
			}
		}
	}
	return true;
}

void draw_group_editor_inline(size_t index) {
	if (g.group_editor_index != static_cast<int>(index) || index >= g.rule_groups.size())
		return;
	ImGui::Indent();
	ImGui::Text(text("Editing group %zu", "编辑分组 %zu"), index);
	ImGui::TextUnformatted(text("Name", "名称"));
	ImGui::SameLine(120.0f);
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	ImGui::InputText("##group_editor_name", g.group_name_input.data(),
		g.group_name_input.size());

	uint32_t key = 0;
	uint32_t modifiers = 0;
	unpack_toggle_key(g.group_editor_work.toggle_key_packed, key, modifiers);
	ImGui::TextUnformatted(text("Toggle key", "快捷键"));
	ImGui::SameLine(120.0f);
	const std::string key_label = (g.hotkey_capture_target == 100 + static_cast<int>(index) ?
		std::string(text("Press a new shortcut...", "请按下新的快捷键…")) :
		(key == 0 ? std::string(text("Press a key", "请按下一个键")) :
			format_hotkey(key, modifiers))) + "##group_editor_key";
	if (ImGui::Button(key_label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
		g.hotkey_capture_target = 100 + static_cast<int>(index);
		g.hotkey_modifier_latch = ns_white_backing::modifier_none;
	}

	ImGui::Checkbox(text("Active at startup", "启动时启用"),
		&g.group_editor_work.active_at_startup);

	if (ImGui::Button(text("Confirm", "确定"))) {
		const std::string new_name = trim_ascii(g.group_name_input.data());
		if (new_name.empty()) {
			show_notification(false, text("Group name cannot be empty", "组名称不能为空"));
		} else {
			ns_alpha_rules::rule_group &group = g.rule_groups[index];
			const ns_alpha_rules::rule_group previous_group = group;
			const bool runtime_active = group.active;
			const std::vector<ns_alpha_rules::capture_rule> rules = group.rules;
			group = g.group_editor_work;
			group.name = new_name;
			group.rules = rules;
			group.active = runtime_active;
			if (save_rule_groups()) {
				show_notification(true, text("Group saved", "分组已保存"));
				g.group_editor_index = -1;
			} else {
				group = previous_group;
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(text("Cancel", "取消")))
		g.group_editor_index = -1;
	ImGui::Unindent();
}

void draw_rule_editor_window() {
	if (g.rule_editor_index < 0 ||
		static_cast<size_t>(g.rule_editor_index) >= g.rule_groups.size()) {
		g.rule_editor_index = -1;
		return;
	}
	const size_t group_index = static_cast<size_t>(g.rule_editor_index);
	bool open = true;
	const std::string title = std::string(text("Rule editor - ", "规则编辑器 - ")) +
		g.rule_groups[group_index].name + "###rule_editor";
	if (!ImGui::Begin(title.c_str(), &open)) {
		ImGui::End();
		return;
	}
	if (!open) {
		g.rule_editor_index = -1;
		g.rule_editor_error.clear();
		ImGui::End();
		return;
	}
	ImGui::TextUnformatted(text(
		"An enabled rule requires a complete PS+VS+FirstIndex+IndexCount+VertexOffset signature.",
		"启用规则必须同时具备 PS、VS、FirstIndex、IndexCount、VertexOffset 完整签名。"));
	ImGui::BeginChild("##rule_table_scroll", ImVec2(0.0f, -96.0f), false,
		ImGuiWindowFlags_HorizontalScrollbar);
	if (ImGui::BeginTable("##rule_table", 9, ImGuiTableFlags_Resizable |
		ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit |
		ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
		ImGui::TableSetupColumn(text("On", "启用"), ImGuiTableColumnFlags_WidthFixed, 40.0f);
		ImGui::TableSetupColumn("PS", ImGuiTableColumnFlags_WidthFixed, 140.0f);
		ImGui::TableSetupColumn("VS", ImGuiTableColumnFlags_WidthFixed, 140.0f);
		ImGui::TableSetupColumn("FirstIndex", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("IndexCount", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn("VertexOffset", ImGuiTableColumnFlags_WidthFixed, 120.0f);
		ImGui::TableSetupColumn(text("Name", "名称"), ImGuiTableColumnFlags_WidthFixed, 320.0f);
		ImGui::TableSetupColumn(text("State", "状态"), ImGuiTableColumnFlags_WidthFixed, 180.0f);
		ImGui::TableSetupColumn(text("Actions", "操作"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableHeadersRow();
		for (size_t index = 0; index < g.rule_editor_work.size(); ++index) {
			ns_alpha_rules::capture_rule &rule = g.rule_editor_work[index];
			ImGui::PushID(static_cast<int>(index));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			bool enabled = rule.enabled;
			if (ImGui::Checkbox("##rule_enabled", &enabled)) {
				if (enabled && !ns_alpha_rules::rule_can_be_enabled(rule)) {
					g.rule_editor_error = text(
						"Rule needs complete PS+VS+geometry before it can be enabled",
						"规则需要完整 PS+VS+几何签名后才能启用");
				} else {
					rule.enabled = enabled;
				}
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputScalar("##ps", ImGuiDataType_U32, &rule.pixel, nullptr, nullptr,
				"%08X", ImGuiInputTextFlags_CharsHexadecimal);
			ImGui::TableSetColumnIndex(2);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputScalar("##vs", ImGuiDataType_U32, &rule.vertex, nullptr, nullptr,
				"%08X", ImGuiInputTextFlags_CharsHexadecimal);
			ImGui::TableSetColumnIndex(3);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputScalar("##first", ImGuiDataType_U32, &rule.first_index, nullptr,
				nullptr, "%u", ImGuiInputTextFlags_CharsDecimal);
			ImGui::TableSetColumnIndex(4);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputScalar("##count", ImGuiDataType_U32, &rule.index_count, nullptr,
				nullptr, "%u", ImGuiInputTextFlags_CharsDecimal);
			ImGui::TableSetColumnIndex(5);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputScalar("##offset", ImGuiDataType_S32, &rule.vertex_offset, nullptr,
				nullptr, "%d", ImGuiInputTextFlags_CharsDecimal);
			ImGui::TableSetColumnIndex(6);
			ImGui::SetNextItemWidth(-1.0f);
			ImGui::InputText("##rule_name", g.rule_editor_names[index].data(),
				g.rule_editor_names[index].size());
			ImGui::TableSetColumnIndex(7);
			if (!rule.enabled)
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
					text("Candidate, disabled", "候选，未启用"));
			else if (!ns_alpha_rules::rule_has_full_signature(rule))
				ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
					text("Incomplete", "签名不完整"));
			else
				ImGui::TextUnformatted(text("Enabled", "已启用"));
			ImGui::TableSetColumnIndex(8);
			if (ImGui::SmallButton(text("Copy", "复制"))) {
				g.rule_editor_work.insert(g.rule_editor_work.begin() +
					static_cast<ptrdiff_t>(index) + 1, rule);
				g.rule_editor_names.insert(g.rule_editor_names.begin() +
					static_cast<ptrdiff_t>(index) + 1, g.rule_editor_names[index]);
				ImGui::PopID();
				break;
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("X##delete_rule")) {
				g.rule_editor_work.erase(g.rule_editor_work.begin() +
					static_cast<ptrdiff_t>(index));
				g.rule_editor_names.erase(g.rule_editor_names.begin() +
					static_cast<ptrdiff_t>(index));
				ImGui::PopID();
				break;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", text("Delete rule", "删除规则"));
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::EndChild();
	if (ImGui::Button(text("Add rule", "新增规则"))) {
		g.rule_editor_work.push_back(ns_alpha_rules::capture_rule{});
		g.rule_editor_names.push_back({});
	}
	if (!g.rule_editor_error.empty())
		ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", g.rule_editor_error.c_str());
	if (ImGui::Button(text("Validate and save", "校验并保存"))) {
		std::string error;
		if (!validate_rule_editor(error)) {
			g.rule_editor_error = error;
		} else {
			for (size_t index = 0; index < g.rule_editor_work.size(); ++index)
				g.rule_editor_work[index].name = trim_ascii(g.rule_editor_names[index].data());
			g.rule_groups[group_index].rules = g.rule_editor_work;
			if (save_rule_groups()) {
				show_notification(true, text("Rules saved", "规则已保存"));
				g.rule_editor_index = -1;
				g.rule_editor_error.clear();
			} else {
				g.rule_editor_error = text("Could not write configuration", "无法写入配置");
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(text("Cancel", "取消"))) {
		g.rule_editor_index = -1;
		g.rule_editor_error.clear();
	}
	ImGui::End();
}

std::vector<uint32_t> collect_unique_candidate_hashes(bool pixel) {
	std::vector<uint32_t> hashes;
	for (const shader_candidate &candidate : g.shader_candidates) {
		const uint32_t hash = pixel ? candidate.rule.pixel : candidate.rule.vertex;
		if (hash != 0)
			hashes.push_back(hash);
	}
	std::sort(hashes.begin(), hashes.end());
	hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
	return hashes;
}

std::vector<size_t> sorted_candidate_indices() {
	std::vector<size_t> indices(g.shader_candidates.size());
	for (size_t index = 0; index < indices.size(); ++index)
		indices[index] = index;
	std::sort(indices.begin(), indices.end(), [](size_t left, size_t right) {
		const shader_candidate &a = g.shader_candidates[left];
		const shader_candidate &b = g.shader_candidates[right];
		if (a.draw_count != b.draw_count)
			return a.draw_count > b.draw_count;
		if (a.rule.pixel != b.rule.pixel)
			return a.rule.pixel < b.rule.pixel;
		if (a.rule.vertex != b.rule.vertex)
			return a.rule.vertex < b.rule.vertex;
		if (a.rule.first_index != b.rule.first_index)
			return a.rule.first_index < b.rule.first_index;
		if (a.rule.index_count != b.rule.index_count)
			return a.rule.index_count < b.rule.index_count;
		return a.rule.vertex_offset < b.rule.vertex_offset;
	});
	return indices;
}

void stop_preview() {
	ns_alpha_rules::preview_exit(g.preview);
}

void draw_hunting_window() {
	if (!g.hunting_open) {
		if (g.preview.active)
			stop_preview();
		return;
	}
	bool open = true;
	if (!ImGui::Begin(text("Shader hunting###hunting", "着色器猎取###hunting"), &open)) {
		ImGui::End();
		return;
	}
	if (!open) {
		g.hunting_open = false;
		stop_preview();
		ImGui::End();
		return;
	}
	ImGui::TextUnformatted(text(
		"Preview is diagnostic only: nothing is written to the configuration.",
		"预览仅用于诊断：不会写入配置。"));
	if (g.preview.active) {
		ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
			text("Diagnostic preview active (transient)", "诊断预览进行中（临时状态）"));
		ImGui::SameLine();
		if (ImGui::Button(text("Stop preview", "停止预览")))
			stop_preview();
	}

	if (g.rule_groups.empty()) {
		ImGui::TextUnformatted(text("No groups available", "没有可用分组"));
	} else {
		if (g.hunting_target_group < 0 ||
			static_cast<size_t>(g.hunting_target_group) >= g.rule_groups.size())
			g.hunting_target_group = 0;
		const std::string preview_name = g.rule_groups[
			static_cast<size_t>(g.hunting_target_group)].name;
		if (ImGui::BeginCombo(text("Target group", "目标分组"), preview_name.c_str())) {
			for (size_t index = 0; index < g.rule_groups.size(); ++index) {
				const bool selected = g.hunting_target_group == static_cast<int>(index);
				if (ImGui::Selectable(g.rule_groups[index].name.c_str(), selected))
					g.hunting_target_group = static_cast<int>(index);
			}
			ImGui::EndCombo();
		}
	}
	if (ImGui::Button(text("Scan next frame", "扫描下一帧"))) {
		g.shader_candidates.clear();
		g.hunting_cursor = 0;
		g.shader_selector_active = true;
		show_notification(true, text("Shader hunting armed", "已准备扫描着色器"));
	}
	if (g.shader_selector_active) {
		ImGui::SameLine();
		ImGui::TextUnformatted(text("Collecting...", "正在收集…"));
	}

	if (ImGui::BeginTabBar("##hunting_stage")) {
		const char *stage_names[] = {
			text("Pixel shaders", "像素着色器"),
			text("Vertex shaders", "顶点着色器"),
			text("Compute shaders", "计算着色器"),
		};
		for (int stage = 0; stage < 3; ++stage) {
			if (!ImGui::BeginTabItem(stage_names[stage]))
				continue;
			if (g.hunting_stage != stage)
				stop_preview();
			g.hunting_stage = stage;
			if (stage == 2) {
				ImGui::TextUnformatted(text(
					"No compute shader data is collected by this addon.",
					"此 addon 不采集计算着色器数据。"));
				ImGui::EndTabItem();
				continue;
			}
			const bool pixel_stage = stage == 0;
			const std::vector<uint32_t> hashes = collect_unique_candidate_hashes(pixel_stage);
			std::unordered_set<uint32_t> &marked =
				pixel_stage ? g.hunting_marked_pixel : g.hunting_marked_vertex;
			if (hashes.empty()) {
				ImGui::TextUnformatted(text(
					"No candidates yet - run a scan first.", "尚无候选 - 请先扫描。"));
				ImGui::EndTabItem();
				continue;
			}
			if (g.hunting_cursor >= hashes.size())
				g.hunting_cursor = hashes.size() - 1;
			const uint32_t current = hashes[g.hunting_cursor];
			if (ImGui::Button(text("First", "首项"))) g.hunting_cursor = 0;
			ImGui::SameLine();
			if (ImGui::Button(text("Prev", "上一项")) && g.hunting_cursor > 0)
				--g.hunting_cursor;
			ImGui::SameLine();
			if (ImGui::Button(text("Next", "下一项")) &&
				g.hunting_cursor + 1 < hashes.size())
				++g.hunting_cursor;
			ImGui::SameLine();
			if (ImGui::Button(text("Last", "末项")))
				g.hunting_cursor = hashes.size() - 1;
			uint32_t draws = 0;
			for (const shader_candidate &candidate : g.shader_candidates) {
				if ((pixel_stage ? candidate.rule.pixel : candidate.rule.vertex) == current)
					draws += candidate.draw_count;
			}
			ImGui::Text("%zu / %zu  %s  %s %u", g.hunting_cursor + 1, hashes.size(),
				format_hash_hex(current).c_str(), text("draws:", "draw 次数："), draws);
			if (marked.count(current) != 0) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "%s",
					text("(marked)", "（已标记）"));
			}
			if (marked.count(current) != 0) {
				if (ImGui::Button(text("Unmark", "取消标记")))
					marked.erase(current);
			} else if (ImGui::Button(text("Mark", "标记"))) {
				marked.insert(current);
			}
			if (pixel_stage) {
				ImGui::SameLine();
				if (g.preview.active && g.preview.kind == 1 &&
					g.preview.isolation_pixel == current) {
					if (ImGui::Button(text("Stop isolation", "停止隔离预览")))
						stop_preview();
				} else if (ImGui::Button(text("Isolate preview", "隔离预览"))) {
					if (!ns_alpha_rules::preview_enter_isolation(g.preview, current,
						g.replay_capture_active))
						show_notification(false, text(
							"Preview unavailable during capture", "捕获期间不能预览"));
				}
			}
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::Separator();
	ImGui::TextUnformatted(text("Observed draws", "观察到的 draw 候选"));
	const std::vector<size_t> order = sorted_candidate_indices();
	if (ImGui::BeginTable("##hunting_draws", 6, ImGuiTableFlags_SizingStretchProp |
		ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("PS", ImGuiTableColumnFlags_WidthFixed, 92.0f);
		ImGui::TableSetupColumn("VS", ImGuiTableColumnFlags_WidthFixed, 92.0f);
		ImGui::TableSetupColumn(text("Geometry", "几何"), ImGuiTableColumnFlags_WidthFixed, 130.0f);
		ImGui::TableSetupColumn(text("Draws", "次数"), ImGuiTableColumnFlags_WidthFixed, 48.0f);
		ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 90.0f);
		for (const size_t candidate_index : order) {
			const shader_candidate &candidate = g.shader_candidates[candidate_index];
			ImGui::PushID(static_cast<int>(candidate_index));
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(format_hash_hex(candidate.rule.pixel).c_str());
			if (g.hunting_marked_pixel.count(candidate.rule.pixel) != 0) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "*");
			}
			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(format_hash_hex(candidate.rule.vertex).c_str());
			if (g.hunting_marked_vertex.count(candidate.rule.vertex) != 0) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "*");
			}
			ImGui::TableSetColumnIndex(2);
			ImGui::Text("%u:%u:%d", candidate.rule.first_index, candidate.rule.index_count,
				candidate.rule.vertex_offset);
			ImGui::TableSetColumnIndex(3);
			ImGui::Text("%u", candidate.draw_count);
			ImGui::TableSetColumnIndex(4);
			if (ImGui::SmallButton(text("Add candidate", "加入候选"))) {
				if (g.hunting_target_group >= 0 &&
					static_cast<size_t>(g.hunting_target_group) < g.rule_groups.size()) {
					if (g.rule_editor_index == g.hunting_target_group) {
						show_notification(false, text(
							"Close the rule editor before adding candidates",
							"请先关闭规则编辑器再加入候选"));
						ImGui::PopID();
						break;
					}
					ns_alpha_rules::capture_rule rule;
					rule.enabled = false;
					rule.pixel = candidate.rule.pixel;
					rule.vertex = candidate.rule.vertex;
					rule.first_index = candidate.rule.first_index;
					rule.index_count = candidate.rule.index_count;
					rule.vertex_offset = candidate.rule.vertex_offset;
					auto &target_rules = g.rule_groups[static_cast<size_t>(g.hunting_target_group)].rules;
					const bool duplicate = std::any_of(target_rules.begin(), target_rules.end(),
						[&rule](const ns_alpha_rules::capture_rule &existing) {
							return ns_alpha_rules::rule_signature_equals(existing, rule);
						});
					if (duplicate) {
						show_notification(false, text("Candidate rule already exists", "候选规则已存在"));
						ImGui::PopID();
						break;
					}
					target_rules.push_back(rule);
					if (save_rule_groups())
						show_notification(true, text(
							"Candidate rule added (disabled)", "已加入候选规则（未启用）"));
				}
			}
			ImGui::TableSetColumnIndex(5);
			if (ImGui::SmallButton(text("Remove", "移除"))) {
				g.shader_candidates.erase(g.shader_candidates.begin() +
					static_cast<ptrdiff_t>(candidate_index));
				ImGui::PopID();
				break;
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::End();
}

void draw_group_list_settings() {
	ImGui::Separator();
	ImGui::TextUnformatted(text("Toggle groups", "着色器组"));
	if (ImGui::Button(text("New group", "新增分组"))) {
		ns_alpha_rules::rule_group group;
		group.name = text("New group", "新分组");
		g.rule_groups.push_back(group);
		open_group_editor(g.rule_groups.size() - 1);
	}
	ImGui::SameLine();
	if (ImGui::Button(text("Save all groups", "保存全部组"))) {
		if (save_rule_groups())
			show_notification(true, text("All groups saved", "全部组已保存"));
	}
	ImGui::SameLine();
	if (ImGui::Button(text("Shader hunting", "着色器猎取")))
		g.hunting_open = !g.hunting_open;
	if (g.rule_groups.empty())
		ImGui::TextUnformatted(text("No groups configured", "没有配置着色器组"));
	for (size_t index = 0; index < g.rule_groups.size(); ++index) {
		ns_alpha_rules::rule_group &group = g.rule_groups[index];
		ImGui::PushID(static_cast<int>(index));
		if (ImGui::SmallButton("X##delete_group")) {
			g.rule_groups.erase(g.rule_groups.begin() + static_cast<ptrdiff_t>(index));
			close_group_editors();
			if (g.hunting_target_group == static_cast<int>(index))
				g.hunting_target_group = -1;
			else if (g.hunting_target_group > static_cast<int>(index))
				--g.hunting_target_group;
			if (save_rule_groups())
				show_notification(true, text("Group deleted and config rewritten",
					"分组已删除并重写配置"));
			ImGui::PopID();
			break;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", text("Delete group", "删除分组"));
		ImGui::SameLine();
		bool active = group.active;
		if (ImGui::Checkbox("##group_active", &active))
			group.active = active;
		ImGui::SameLine();
		if (ImGui::SmallButton(text("Edit group", "编辑分组")))
			open_group_editor(index);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", text("Edit group", "编辑分组"));
		ImGui::SameLine();
		if (ImGui::SmallButton(text("Edit rules", "编辑规则")))
			open_rule_editor(index);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", text("Edit rules", "编辑规则"));
		ImGui::SameLine();
		uint32_t key = 0;
		uint32_t modifiers = 0;
		unpack_toggle_key(group.toggle_key_packed, key, modifiers);
		size_t enabled_rules = 0;
		for (const ns_alpha_rules::capture_rule &rule : group.rules) {
			if (rule.enabled)
				++enabled_rules;
		}
		ImGui::Text("%zu %s (%s) %s %zu/%zu", index, group.name.c_str(),
			key == 0 ? text("no key", "无快捷键") : format_hotkey(key, modifiers).c_str(),
			text("rules:", "规则："), enabled_rules, group.rules.size());
		draw_group_editor_inline(index);
		ImGui::PopID();
	}
}

void on_settings_overlay(effect_runtime *runtime) {
	update_locale(runtime);
	const std::array<const char *, 4> setting_labels = {
		text("Screenshot shortcut", "截图快捷键"),
		text("Reload shortcut", "重载快捷键"),
		text("Screenshot path", "截图路径"),
		text("Screenshot filename", "截图文件名"),
	};
	float setting_label_width = 0.0f;
	for (const char *label : setting_labels)
		setting_label_width = std::max(setting_label_width, ImGui::CalcTextSize(label).x);
	const auto draw_hotkey_row = [](const char *label, int target, uint32_t key, uint32_t modifiers) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(label);
		ImGui::TableSetColumnIndex(1);
		const std::string button_label = (g.hotkey_capture_target == target ?
			std::string(text("Press a new shortcut...", "请按下新的快捷键…")) : format_hotkey(key, modifiers)) +
			"##shortcut_" + std::to_string(target);
		if (ImGui::Button(button_label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) {
			g.hotkey_capture_target = target;
			g.hotkey_modifier_latch = ns_white_backing::modifier_none;
		}
	};
	// Match REST's compact control spacing while adding enough vertical cell padding
	// for ReShade font scaling to keep adjacent rows from overlapping.
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 3.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f, 5.0f));
	if (ImGui::BeginTable("##capture_settings", 2,
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("##setting_label", ImGuiTableColumnFlags_WidthFixed,
			setting_label_width);
		ImGui::TableSetupColumn("##setting_control", ImGuiTableColumnFlags_WidthStretch);
		draw_hotkey_row(text("Screenshot shortcut", "截图快捷键"), 1, g.cfg.capture_key, g.cfg.capture_modifiers);
		draw_hotkey_row(text("Reload shortcut", "重载快捷键"), 2, g.cfg.reload_key, g.cfg.reload_modifiers);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(text("Screenshot path", "截图路径"));
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputText("##screenshot_path", g.output_path_input.data(), g.output_path_input.size(),
			ImGuiInputTextFlags_None, nullptr, nullptr);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(1);
		if (ImGui::Button(text("Save path", "保存路径"), ImVec2(0.0f, 0.0f)))
			save_output_directory(g.output_path_input.data());
		ImGui::SameLine();
		if (ImGui::Button(text("Use ReShade/GShade path", "使用 ReShade/GShade 路径"), ImVec2(0.0f, 0.0f)))
			save_output_directory("");
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(text("Screenshot filename", "截图文件名"));
		ImGui::TableSetColumnIndex(1);
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		ImGui::InputText("##screenshot_filename", g.file_naming_input.data(),
			g.file_naming_input.size(), ImGuiInputTextFlags_None, nullptr, nullptr);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(1);
		if (ImGui::Button(text("Save filename", "保存文件名"), ImVec2(0.0f, 0.0f))) {
			if (save_file_naming(g.file_naming_input.data()))
				show_notification(true, text("Screenshot filename saved", "截图文件名已保存"));
			else
				show_notification(false, text("Invalid screenshot filename", "截图文件名无效"));
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar(2);
	draw_group_list_settings();
	draw_rule_editor_window();
	draw_hunting_window();
	bool output_black = g.cfg.output_black;
	bool output_white = g.cfg.output_white;
	bool output_transparent = g.cfg.output_transparent;
	bool output_changed = false;
	const float output_label_width = ImGui::CalcTextSize(text("Output", "输出")).x;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3.0f, 3.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f, 5.0f));
	if (ImGui::BeginTable("##output_settings", 2,
		ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
		ImGui::TableSetupColumn("##output_label", ImGuiTableColumnFlags_WidthFixed,
			output_label_width);
		ImGui::TableSetupColumn("##output_controls", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::TextUnformatted(text("Output", "输出"));
		ImGui::TableSetColumnIndex(1);
		output_changed |= ImGui::Checkbox(text("Black background", "黑底图"), &output_black);
		ImGui::SameLine();
		output_changed |= ImGui::Checkbox(text("White background", "白底图"), &output_white);
		ImGui::SameLine();
		output_changed |= ImGui::Checkbox(text("Transparent", "透明图"), &output_transparent);
		ImGui::EndTable();
	}
	ImGui::PopStyleVar(2);
	if (output_changed) {
		if (!output_black && !output_white && !output_transparent) {
			show_notification(false, capture_failure_message(
				text("select at least one output", "请至少选择一种输出")));
		} else if (!save_output_selection(output_black, output_white, output_transparent)) {
			show_notification(false, capture_failure_message(
				text("could not save output selection", "无法保存输出选项")));
		} else {
			g.cfg.output_black = output_black;
			g.cfg.output_white = output_white;
			g.cfg.output_transparent = output_transparent;
			show_notification(true, text("NS Alpha Capture - Output selection saved",
				"NS Alpha Capture - 输出选项已保存"));
		}
	}
	if (g.hotkey_capture_target != 0)
		ImGui::TextUnformatted(text("Esc cancels, Backspace clears", "Esc 取消，Backspace 清除"));
	process_hotkey_capture(runtime);
}

void on_destroy_device(device *) {
	g.replay_capture_active = false;
	reset_replay_frame_state();
	g.learned_scene_targets.clear();
	g.replay = {};
}

void on_reshade_present(effect_runtime *runtime) {
	update_locale(runtime);
	if (runtime != nullptr) {
		for (ns_alpha_rules::rule_group &group : g.rule_groups) {
			uint32_t toggle_key = 0;
			uint32_t toggle_modifiers = 0;
			unpack_toggle_key(group.toggle_key_packed, toggle_key, toggle_modifiers);
			if (!runtime_hotkey_pressed(runtime, toggle_key, toggle_modifiers))
				continue;
			group.active = !group.active;
			show_notification(true, group.active ?
				std::string(text("Group enabled: ", "已启用组：")) + group.name :
				std::string(text("Group disabled: ", "已禁用组：")) + group.name);
		}
	}
	if (g.replay_capture_active) {
		capture_replay_outputs(runtime);
		g.replay_capture_active = false;
		reset_replay_frame_state();
	}
	if (g.shader_selector_active) {
		g.shader_selector_active = false;
		log_line("shader selector scan complete candidates=%zu", g.shader_candidates.size());
		show_notification(true, std::string(text("Shader scan complete - choose a candidate in the addon settings",
			"着色器扫描完成 - 请在 addon 设置中选择候选项")));
	}
	if (g.hotkey_capture_target != 0)
		return;
	if (g.hotkey_suppress_key != 0) {
		if (runtime != nullptr && runtime->is_key_down(g.hotkey_suppress_key))
			return;
		g.hotkey_suppress_key = 0;
	}

	const bool capture_pressed = runtime_hotkey_pressed(runtime, g.cfg.capture_key,
		g.cfg.capture_modifiers);
	if (capture_pressed) {
		reset_replay_frame_state();
		g.replay_capture_active = true;
		log_line("capture armed: next frame capture active");
		show_notification(true, text("NS Alpha Capture - Capturing next color frame", "NS Alpha Capture - 正在捕获下一帧画面"));
	}

	const bool reload_pressed = runtime_hotkey_pressed(runtime, g.cfg.reload_key,
		g.cfg.reload_modifiers);
	if (reload_pressed) {
		const bool discarded_edits = g.group_editor_index >= 0 || g.rule_editor_index >= 0;
		if (load_config()) {
			if (ensure_output_directory()) {
				log_line("configuration reloaded");
				show_notification(true, discarded_edits ?
					text("Configuration reloaded; unsaved edits discarded",
						"配置已重新载入，未保存的编辑已丢弃") :
					text("NS Alpha Capture - Configuration reloaded", "NS Alpha Capture - 配置已重新载入"));
			} else {
				capture_failure("cannot create output directory after reload");
			}
		}
	}

}

void on_reshade_overlay(effect_runtime *) {
	if (g.notification_message.empty() || GetTickCount64() >= g.notification_expires_at)
		return;

	const ImVec2 display_size = ImGui::GetIO().DisplaySize;
	if (display_size.x <= 0.0f || display_size.y <= 0.0f)
		return;

	const ImVec2 text_size = ImGui::CalcTextSize(g.notification_message.c_str());
	const float banner_width = std::min(text_size.x + 36.0f, display_size.x - 40.0f);
	const float banner_height = text_size.y + 22.0f;
	const float left = (display_size.x - banner_width) * 0.5f;
	const float top = 22.0f;
	ImDrawList *draw_list = ImGui::GetForegroundDrawList();
	const ImU32 background = g.notification_success ? IM_COL32(24, 105, 72, 242) : IM_COL32(156, 48, 54, 242);
	draw_list->AddRectFilled(ImVec2(left, top),
		ImVec2(left + banner_width, top + banner_height), background, 5.0f);
	draw_list->AddText(ImVec2(left + 18.0f, top + 11.0f), IM_COL32(255, 255, 255, 255),
		g.notification_message.c_str());
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(module);
		if (!reshade::register_addon(module))
			return FALSE;
		update_locale(nullptr);
		if (!build_paths()) {
			reshade::unregister_addon(module);
			return FALSE;
		}
		if (load_config() && !ensure_output_directory())
			capture_failure("cannot create output directory during startup");
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::register_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::clear_render_target_view>(on_clear_render_target_view);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reshade_reloaded_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_reshade_present);
		reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
		reshade::register_overlay(nullptr, on_settings_overlay);
		log_line("=== NS_AlphaCapture loaded: color draw replay exporter auto_match=%u auto_highlight=%u ===",
			g.cfg.auto_match ? 1u : 0u, g.cfg.auto_highlight ? 1u : 0u);
	} else if (reason == DLL_PROCESS_DETACH) {
		reshade::unregister_overlay(nullptr, on_settings_overlay);
		reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
		reshade::unregister_event<reshade::addon_event::reshade_present>(on_reshade_present);
		reshade::unregister_event<reshade::addon_event::reshade_reloaded_effects>(on_reshade_reloaded_effects);
		reshade::unregister_event<reshade::addon_event::reshade_begin_effects>(on_reshade_begin_effects);
		reshade::unregister_event<reshade::addon_event::clear_render_target_view>(on_clear_render_target_view);
		reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::unregister_event<reshade::addon_event::draw>(on_draw);
		reshade::unregister_event<reshade::addon_event::destroy_command_list>(on_destroy_command_list);
		reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
		reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
		reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::unregister_addon(module);
	}
	return TRUE;
}
