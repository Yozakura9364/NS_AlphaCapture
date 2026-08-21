#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace ns_alpha_rules {

struct capture_rule {
	bool enabled = false;
	uint32_t pixel = 0;
	uint32_t vertex = 0;
	uint32_t first_index = 0;
	uint32_t index_count = 0;
	int32_t vertex_offset = 0;
	std::string name;
};

// Non-indexed composite draws are intentionally separate from indexed mesh rules.
// The numeric blend values mirror D3D11_BLEND (SRC_ALPHA=5, INV_SRC_ALPHA=6).
struct nonindexed_rule {
	bool enabled = false;
	uint32_t pixel = 0;
	uint32_t vertex = 0;
	uint32_t vertex_count = 0;
	uint32_t instance_count = 0;
	uint32_t first_vertex = 0;
	uint32_t first_instance = 0;
	uint32_t index_count = 0;
	uint32_t first_index = 0;
	int32_t vertex_offset = 0;
	uint32_t render_target_width = 0;
	uint32_t render_target_height = 0;
	bool require_null_dsv = true;
	bool blend_enable = true;
	uint32_t src_blend = 5;
	uint32_t dest_blend = 6;
	uint32_t write_mask = 0x07;
	std::string name;
};

inline bool nonindexed_rule_can_be_enabled(const nonindexed_rule &rule) {
	return rule.pixel != 0 && rule.vertex != 0 && rule.vertex_count != 0 &&
		rule.instance_count != 0 && rule.render_target_width != 0 &&
		rule.render_target_height != 0;
}

inline void nonindexed_rule_force_candidate_if_incomplete(nonindexed_rule &rule) {
	if (rule.enabled && !nonindexed_rule_can_be_enabled(rule))
		rule.enabled = false;
}

inline bool nonindexed_rule_matches(const nonindexed_rule &rule,
	uint32_t pixel, uint32_t vertex, uint32_t vertex_count, uint32_t instance_count,
	uint32_t first_vertex, uint32_t first_instance, uint32_t index_count,
	uint32_t first_index, int32_t vertex_offset, uint32_t render_target_width,
	uint32_t render_target_height, bool dsv_bound, bool blend_enable,
	uint32_t src_blend, uint32_t dest_blend, uint32_t write_mask) {
	if (!rule.enabled || !nonindexed_rule_can_be_enabled(rule))
		return false;
	return rule.pixel == pixel && rule.vertex == vertex &&
		rule.vertex_count == vertex_count && rule.instance_count == instance_count &&
		rule.first_vertex == first_vertex && rule.first_instance == first_instance &&
		rule.index_count == index_count && rule.first_index == first_index &&
		rule.vertex_offset == vertex_offset &&
		rule.render_target_width == render_target_width &&
		rule.render_target_height == render_target_height &&
		(!rule.require_null_dsv || !dsv_bound) && rule.blend_enable == blend_enable &&
		rule.src_blend == src_blend && rule.dest_blend == dest_blend &&
		rule.write_mask == write_mask;
}

struct rule_group {
	std::string name;
	uint32_t toggle_key_packed = 0;
	bool active_at_startup = false;
	bool active = false;
	std::vector<capture_rule> rules;
};

struct preview_state {
	bool active = false;
	int kind = 0;
	uint32_t isolation_pixel = 0;
	size_t group_index = 0;
};

inline std::string ini_trim(const std::string &value) {
	const size_t first = value.find_first_not_of(" \t");
	if (first == std::string::npos)
		return {};
	const size_t last = value.find_last_not_of(" \t");
	return value.substr(first, last - first + 1);
}

inline bool rule_has_full_signature(const capture_rule &rule) {
	return rule.pixel != 0 && rule.vertex != 0 && rule.index_count != 0;
}

inline bool rule_can_be_enabled(const capture_rule &rule) {
	return rule_has_full_signature(rule);
}

inline void rule_force_candidate_if_incomplete(capture_rule &rule) {
	if (rule.enabled && !rule_can_be_enabled(rule))
		rule.enabled = false;
}

inline bool rule_signature_equals(const capture_rule &left, const capture_rule &right) {
	return left.pixel == right.pixel && left.vertex == right.vertex &&
		left.first_index == right.first_index && left.index_count == right.index_count &&
		left.vertex_offset == right.vertex_offset;
}

inline bool rule_matches(const capture_rule &rule, uint32_t pixel, uint32_t vertex,
	uint32_t first_index, uint32_t index_count, int32_t vertex_offset) {
	if (!rule.enabled || !rule_has_full_signature(rule))
		return false;
	return rule.pixel == pixel && rule.vertex == vertex &&
		rule.first_index == first_index && rule.index_count == index_count &&
		rule.vertex_offset == vertex_offset;
}

inline bool group_matches(const rule_group &group, uint32_t pixel, uint32_t vertex,
	uint32_t first_index, uint32_t index_count, int32_t vertex_offset) {
	if (!group.active)
		return false;
	for (const capture_rule &rule : group.rules) {
		if (rule_matches(rule, pixel, vertex, first_index, index_count, vertex_offset))
			return true;
	}
	return false;
}

inline bool groups_match(const std::vector<rule_group> &groups, uint32_t pixel,
	uint32_t vertex, uint32_t first_index, uint32_t index_count, int32_t vertex_offset) {
	for (const rule_group &group : groups) {
		if (group_matches(group, pixel, vertex, first_index, index_count, vertex_offset))
			return true;
	}
	return false;
}

inline bool preview_enter_isolation(preview_state &preview, uint32_t pixel,
	bool capture_active) {
	if (capture_active || pixel == 0)
		return false;
	preview.active = true;
	preview.kind = 1;
	preview.isolation_pixel = pixel;
	preview.group_index = 0;
	return true;
}

inline bool preview_enter_group(preview_state &preview, size_t group_index,
	size_t group_count, bool capture_active) {
	if (capture_active || group_index >= group_count)
		return false;
	preview.active = true;
	preview.kind = 2;
	preview.isolation_pixel = 0;
	preview.group_index = group_index;
	return true;
}

inline void preview_exit(preview_state &preview) {
	preview = preview_state{};
}

inline bool preview_hides_draw(const preview_state &preview,
	const std::vector<rule_group> &groups, uint32_t pixel, uint32_t vertex,
	uint32_t first_index, uint32_t index_count, int32_t vertex_offset) {
	if (!preview.active)
		return false;
	if (preview.kind == 1)
		return pixel == preview.isolation_pixel;
	if (preview.kind == 2) {
		if (preview.group_index >= groups.size())
			return false;
		const rule_group &group = groups[preview.group_index];
		for (const capture_rule &rule : group.rules) {
			if (!rule.enabled)
				continue;
			if (rule.pixel == pixel && rule.vertex == vertex &&
				rule.first_index == first_index && rule.index_count == index_count &&
				rule.vertex_offset == vertex_offset)
				return true;
		}
	}
	return false;
}

struct ini_section {
	std::string name;
	std::vector<std::pair<std::string, std::string>> entries;
};

struct ini_document {
	std::vector<ini_section> sections;

	ini_section *find(const std::string &name) {
		for (ini_section &section : sections) {
			if (section.name == name)
				return &section;
		}
		return nullptr;
	}
	const ini_section *find(const std::string &name) const {
		for (const ini_section &section : sections) {
			if (section.name == name)
				return &section;
		}
		return nullptr;
	}
	bool get(const std::string &section_name, const std::string &key,
		std::string &value) const {
		const ini_section *section = find(section_name);
		if (section == nullptr)
			return false;
		for (auto entry = section->entries.rbegin(); entry != section->entries.rend(); ++entry) {
			if (entry->first == key) {
				value = entry->second;
				return true;
			}
		}
		return false;
	}
	void set(const std::string &section_name, const std::string &key,
		const std::string &value) {
		ini_section *section = find(section_name);
		if (section == nullptr) {
			sections.push_back({ section_name, {} });
			section = &sections.back();
		}
		for (auto &entry : section->entries) {
			if (entry.first == key) {
				entry.second = value;
				return;
			}
		}
		section->entries.emplace_back(key, value);
	}
	bool remove_section(const std::string &name) {
		for (auto it = sections.begin(); it != sections.end(); ++it) {
			if (it->name == name) {
				sections.erase(it);
				return true;
			}
		}
		return false;
	}
};

inline ini_document ini_parse(const std::string &text) {
	ini_document document;
	std::string bytes = text;
	if (bytes.size() >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF &&
		static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF)
		bytes.erase(0, 3);
	ini_section *current = nullptr;
	for (size_t start = 0; start <= bytes.size();) {
		const size_t end = bytes.find('\n', start);
		std::string line = bytes.substr(start,
			end == std::string::npos ? std::string::npos : end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		line = ini_trim(line);
		if (!line.empty() && line.front() != ';' && line.front() != '#') {
			if (line.front() == '[' && line.back() == ']') {
				const std::string name = line.substr(1, line.size() - 2);
				current = document.find(name);
				if (current == nullptr) {
					document.sections.push_back({ name, {} });
					current = &document.sections.back();
				}
			} else if (current != nullptr) {
				const size_t equals = line.find('=');
				if (equals != std::string::npos)
					current->entries.emplace_back(ini_trim(line.substr(0, equals)),
						ini_trim(line.substr(equals + 1)));
			}
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}
	return document;
}

inline std::string ini_serialize(const ini_document &document) {
	std::string text;
	for (size_t index = 0; index < document.sections.size(); ++index) {
		const ini_section &section = document.sections[index];
		text += "[" + section.name + "]\r\n";
		for (const auto &entry : section.entries)
			text += entry.first + "=" + entry.second + "\r\n";
		if (index + 1 < document.sections.size())
			text += "\r\n";
	}
	return text;
}

inline bool ini_get_u32(const ini_document &document, const std::string &section,
	const std::string &key, uint32_t &value) {
	std::string text_value;
	if (!document.get(section, key, text_value) || text_value.empty())
		return false;
	char *end = nullptr;
	const unsigned long parsed = std::strtoul(text_value.c_str(), &end, 0);
	if (end == nullptr || *end != '\0' || parsed > UINT32_MAX)
		return false;
	value = static_cast<uint32_t>(parsed);
	return true;
}

inline bool ini_get_i32(const ini_document &document, const std::string &section,
	const std::string &key, int32_t &value) {
	std::string text_value;
	if (!document.get(section, key, text_value) || text_value.empty())
		return false;
	char *end = nullptr;
	const long parsed = std::strtol(text_value.c_str(), &end, 0);
	if (end == nullptr || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX)
		return false;
	value = static_cast<int32_t>(parsed);
	return true;
}

inline bool ini_get_bool(const ini_document &document, const std::string &section,
	const std::string &key, bool &value) {
	std::string text_value;
	if (!document.get(section, key, text_value))
		return false;
	value = text_value == "1" || text_value == "true" || text_value == "True" ||
		text_value == "TRUE";
	return true;
}

inline bool ini_is_v2(const ini_document &document) {
	uint32_t version = 0;
	return ini_get_u32(document, "General", "FormatVersion", version) && version >= 2;
}

inline bool ini_has_v1_groups(const ini_document &document) {
	uint32_t amount = 0;
	return ini_get_u32(document, "General", "AmountGroups", amount);
}

inline bool parse_v1_draw_rule(const std::string &value, capture_rule &rule) {
	std::vector<std::string> fields;
	for (size_t start = 0;;) {
		const size_t separator = value.find('|', start);
		if (separator == std::string::npos) {
			fields.push_back(value.substr(start));
			break;
		}
		fields.push_back(value.substr(start, separator - start));
		start = separator + 1;
	}
	if (fields.size() != 5)
		return false;
	bool concrete = true;
	const auto parse_field = [&fields, &concrete](size_t index, uint32_t &target) {
		const std::string field = ini_trim(fields[index]);
		if (field == "*") {
			concrete = false;
			target = 0;
			return;
		}
		char *end = nullptr;
		const unsigned long parsed = std::strtoul(field.c_str(), &end, 0);
		if (end == nullptr || *end != '\0' || parsed > UINT32_MAX) {
			concrete = false;
			target = 0;
			return;
		}
		target = static_cast<uint32_t>(parsed);
	};
	parse_field(0, rule.pixel);
	parse_field(1, rule.vertex);
	parse_field(2, rule.first_index);
	parse_field(3, rule.index_count);
	const std::string offset_field = ini_trim(fields[4]);
	if (offset_field == "*") {
		concrete = false;
		rule.vertex_offset = 0;
	} else {
		char *end = nullptr;
		const long parsed = std::strtol(offset_field.c_str(), &end, 0);
		if (end == nullptr || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) {
			concrete = false;
			rule.vertex_offset = 0;
		} else {
			rule.vertex_offset = static_cast<int32_t>(parsed);
		}
	}
	rule.enabled = concrete && rule_can_be_enabled(rule);
	return true;
}

inline std::vector<rule_group> groups_from_v1(const ini_document &document) {
	std::vector<rule_group> groups;
	uint32_t amount = 0;
	if (!ini_get_u32(document, "General", "AmountGroups", amount))
		return groups;
	for (uint32_t index = 0; index < amount && index < 256; ++index) {
		const std::string base = "Group" + std::to_string(index);
		rule_group group;
		document.get(base, "Name", group.name);
		if (group.name.empty())
			group.name = "Default";
		ini_get_u32(document, base, "ToggleKey", group.toggle_key_packed);
		ini_get_bool(document, base, "IsActiveAtStartup", group.active_at_startup);
		group.active = group.active_at_startup;

		const ini_section *draw_rules = document.find(base + "_DrawRules");
		if (draw_rules != nullptr) {
			uint32_t rule_amount = 0;
			ini_get_u32(document, base + "_DrawRules", "AmountRules", rule_amount);
			for (uint32_t rule_index = 0; rule_index < rule_amount && rule_index < 4096;
				++rule_index) {
				std::string value;
				if (!document.get(base + "_DrawRules",
					"Rule" + std::to_string(rule_index), value))
					continue;
				capture_rule rule;
				if (parse_v1_draw_rule(value, rule))
					group.rules.push_back(rule);
			}
		}

		const auto hash_covered = [](const ini_document &doc, const std::string &section,
			std::vector<capture_rule> &rules, bool pixel) {
			uint32_t hash_amount = 0;
			if (!ini_get_u32(doc, section, "AmountHashes", hash_amount))
				return;
			for (uint32_t hash_index = 0; hash_index < hash_amount && hash_index < 4096;
				++hash_index) {
				uint32_t hash = 0;
				if (!ini_get_u32(doc, section, "ShaderHash" + std::to_string(hash_index), hash) ||
					hash == 0)
					continue;
				bool covered = false;
				for (const capture_rule &rule : rules) {
					if ((pixel ? rule.pixel : rule.vertex) == hash) {
						covered = true;
						break;
					}
				}
				if (!covered) {
					capture_rule candidate;
					if (pixel) candidate.pixel = hash;
					else candidate.vertex = hash;
					candidate.enabled = false;
					rules.push_back(candidate);
				}
			}
		};
		hash_covered(document, base + "_PixelShaders", group.rules, true);
		hash_covered(document, base + "_VertexShaders", group.rules, false);

		groups.push_back(std::move(group));
	}
	return groups;
}

inline bool parse_v2_rule_line(const std::string &value, capture_rule &rule) {
	std::vector<std::string> fields;
	for (size_t start = 0;;) {
		const size_t separator = value.find('|', start);
		if (separator == std::string::npos) {
			fields.push_back(value.substr(start));
			break;
		}
		fields.push_back(value.substr(start, separator - start));
		start = separator + 1;
	}
	if (fields.size() != 6)
		return false;
	uint32_t enabled_value = 0;
	char *end = nullptr;
	const unsigned long enabled_parsed = std::strtoul(ini_trim(fields[0]).c_str(), &end, 0);
	if (end == nullptr || *end != '\0' || enabled_parsed > 1)
		return false;
	enabled_value = static_cast<uint32_t>(enabled_parsed);
	const auto parse_u32_field = [&fields](size_t index, uint32_t &target) {
		char *field_end = nullptr;
		const unsigned long parsed = std::strtoul(ini_trim(fields[index]).c_str(), &field_end, 0);
		if (field_end == nullptr || *field_end != '\0' || parsed > UINT32_MAX)
			return false;
		target = static_cast<uint32_t>(parsed);
		return true;
	};
	if (!parse_u32_field(1, rule.pixel) || !parse_u32_field(2, rule.vertex) ||
		!parse_u32_field(3, rule.first_index) || !parse_u32_field(4, rule.index_count))
		return false;
	char *offset_end = nullptr;
	const long offset_parsed = std::strtol(ini_trim(fields[5]).c_str(), &offset_end, 0);
	if (offset_end == nullptr || *offset_end != '\0' ||
		offset_parsed < INT32_MIN || offset_parsed > INT32_MAX)
		return false;
	rule.vertex_offset = static_cast<int32_t>(offset_parsed);
	rule.enabled = enabled_value != 0;
	rule_force_candidate_if_incomplete(rule);
	return true;
}

inline std::vector<rule_group> groups_from_v2(const ini_document &document) {
	std::vector<rule_group> groups;
	uint32_t amount = 0;
	if (!ini_get_u32(document, "General", "AmountGroups", amount))
		return groups;
	for (uint32_t index = 0; index < amount && index < 256; ++index) {
		const std::string base = "Group" + std::to_string(index);
		rule_group group;
		document.get(base, "Name", group.name);
		if (group.name.empty())
			group.name = "Default";
		ini_get_u32(document, base, "ToggleKey", group.toggle_key_packed);
		ini_get_bool(document, base, "IsActiveAtStartup", group.active_at_startup);
		group.active = group.active_at_startup;

		uint32_t rule_amount = 0;
		ini_get_u32(document, base + "_Rules", "AmountRules", rule_amount);
		for (uint32_t rule_index = 0; rule_index < rule_amount && rule_index < 4096;
			++rule_index) {
			std::string value;
			if (!document.get(base + "_Rules", "Rule" + std::to_string(rule_index), value))
				continue;
			capture_rule rule;
			if (!parse_v2_rule_line(value, rule))
				continue;
			document.get(base + "_Rules", "Rule" + std::to_string(rule_index) + "Name",
				rule.name);
			group.rules.push_back(std::move(rule));
		}
		groups.push_back(std::move(group));
	}
	return groups;
}

inline void append_groups_v2(ini_document &document,
	const std::vector<rule_group> &groups) {
	document.set("General", "FormatVersion", "2");
	document.set("General", "AmountGroups", std::to_string(groups.size()));
	for (size_t index = 0; index < groups.size(); ++index) {
		const rule_group &group = groups[index];
		const std::string base = "Group" + std::to_string(index);
		document.set(base, "Name", group.name);
		document.set(base, "ToggleKey", std::to_string(group.toggle_key_packed));
		document.set(base, "IsActiveAtStartup", group.active_at_startup ? "True" : "False");
		const std::string rules_section = base + "_Rules";
		document.set(rules_section, "AmountRules", std::to_string(group.rules.size()));
		for (size_t rule_index = 0; rule_index < group.rules.size(); ++rule_index) {
			const capture_rule &rule = group.rules[rule_index];
			const std::string key = "Rule" + std::to_string(rule_index);
			document.set(rules_section, key,
				std::string(rule.enabled ? "1" : "0") + "|" +
				std::to_string(rule.pixel) + "|" + std::to_string(rule.vertex) + "|" +
				std::to_string(rule.first_index) + "|" + std::to_string(rule.index_count) +
				"|" + std::to_string(rule.vertex_offset));
			if (!rule.name.empty())
				document.set(rules_section, key + "Name", rule.name);
		}
	}
}

inline void remove_group_sections(ini_document &document) {
	for (size_t index = 0;; ++index) {
		const std::string base = "Group" + std::to_string(index);
		const bool had_any = document.find(base) != nullptr ||
			document.find(base + "_PixelShaders") != nullptr ||
			document.find(base + "_VertexShaders") != nullptr ||
			document.find(base + "_DrawRules") != nullptr ||
			document.find(base + "_Rules") != nullptr;
		if (!had_any)
			break;
		document.remove_section(base);
		document.remove_section(base + "_PixelShaders");
		document.remove_section(base + "_VertexShaders");
		document.remove_section(base + "_DrawRules");
		document.remove_section(base + "_Rules");
	}
}

inline ini_document migrate_v1_to_v2(const ini_document &v1) {
	const std::vector<rule_group> groups = groups_from_v1(v1);
	ini_document v2 = v1;
	remove_group_sections(v2);
	append_groups_v2(v2, groups);
	return v2;
}

inline std::vector<rule_group> groups_from_document(const ini_document &document) {
	if (ini_is_v2(document))
		return groups_from_v2(document);
	return groups_from_v1(document);
}

inline bool parse_nonindexed_rule_line(const std::string &value, nonindexed_rule &rule) {
	std::vector<std::string> fields;
	for (size_t start = 0;;) {
		const size_t separator = value.find('|', start);
		if (separator == std::string::npos) {
			fields.push_back(value.substr(start));
			break;
		}
		fields.push_back(value.substr(start, separator - start));
		start = separator + 1;
	}
	if (fields.size() != 17)
		return false;
	const auto parse_u32 = [&fields](size_t index, uint32_t &target) {
		char *end = nullptr;
		const unsigned long parsed = std::strtoul(ini_trim(fields[index]).c_str(), &end, 0);
		if (end == nullptr || *end != '\0' || parsed > UINT32_MAX)
			return false;
		target = static_cast<uint32_t>(parsed);
		return true;
	};
	const auto parse_bool = [&fields, &parse_u32](size_t index, bool &target) {
		uint32_t value = 0;
		if (!parse_u32(index, value) || value > 1)
			return false;
		target = value != 0;
		return true;
	};
	if (!parse_bool(0, rule.enabled) || !parse_u32(1, rule.pixel) ||
		!parse_u32(2, rule.vertex) || !parse_u32(3, rule.vertex_count) ||
		!parse_u32(4, rule.instance_count) || !parse_u32(5, rule.first_vertex) ||
		!parse_u32(6, rule.first_instance) || !parse_u32(7, rule.index_count) ||
		!parse_u32(8, rule.first_index))
		return false;
	char *offset_end = nullptr;
	const long offset = std::strtol(ini_trim(fields[9]).c_str(), &offset_end, 0);
	if (offset_end == nullptr || *offset_end != '\0' || offset < INT32_MIN || offset > INT32_MAX)
		return false;
	rule.vertex_offset = static_cast<int32_t>(offset);
	if (!parse_u32(10, rule.render_target_width) || !parse_u32(11, rule.render_target_height) ||
		!parse_bool(12, rule.require_null_dsv) || !parse_bool(13, rule.blend_enable) ||
		!parse_u32(14, rule.src_blend) || !parse_u32(15, rule.dest_blend) ||
		!parse_u32(16, rule.write_mask))
		return false;
	nonindexed_rule_force_candidate_if_incomplete(rule);
	return true;
}

inline std::vector<nonindexed_rule> nonindexed_rules_from_document(const ini_document &document) {
	std::vector<nonindexed_rule> rules;
	uint32_t amount = 0;
	if (!ini_get_u32(document, "NonIndexedRules", "AmountRules", amount))
		return rules;
	for (uint32_t index = 0; index < amount && index < 256; ++index) {
		std::string value;
		if (!document.get("NonIndexedRules", "Rule" + std::to_string(index), value))
			continue;
		nonindexed_rule rule;
		if (!parse_nonindexed_rule_line(value, rule))
			continue;
		document.get("NonIndexedRules", "Rule" + std::to_string(index) + "Name", rule.name);
		rules.push_back(std::move(rule));
	}
	return rules;
}

inline void append_nonindexed_rules(ini_document &document,
	const std::vector<nonindexed_rule> &rules) {
	document.set("NonIndexedRules", "AmountRules", std::to_string(rules.size()));
	for (size_t index = 0; index < rules.size(); ++index) {
		const nonindexed_rule &rule = rules[index];
		const std::string key = "Rule" + std::to_string(index);
		document.set("NonIndexedRules", key,
			std::string(rule.enabled ? "1" : "0") + "|" +
			std::to_string(rule.pixel) + "|" + std::to_string(rule.vertex) + "|" +
			std::to_string(rule.vertex_count) + "|" + std::to_string(rule.instance_count) + "|" +
			std::to_string(rule.first_vertex) + "|" + std::to_string(rule.first_instance) + "|" +
			std::to_string(rule.index_count) + "|" + std::to_string(rule.first_index) + "|" +
			std::to_string(rule.vertex_offset) + "|" + std::to_string(rule.render_target_width) + "|" +
			std::to_string(rule.render_target_height) + "|" + (rule.require_null_dsv ? "1" : "0") + "|" +
			(rule.blend_enable ? "1" : "0") + "|" + std::to_string(rule.src_blend) + "|" +
			std::to_string(rule.dest_blend) + "|" + std::to_string(rule.write_mask));
		if (!rule.name.empty())
			document.set("NonIndexedRules", key + "Name", rule.name);
	}
}

inline std::string serialize_nonindexed_rules(const ini_document &existing,
	const std::vector<nonindexed_rule> &rules) {
	ini_document document = existing;
	document.remove_section("NonIndexedRules");
	append_nonindexed_rules(document, rules);
	return ini_serialize(document);
}

inline std::string serialize_groups_v2(const ini_document &existing,
	const std::vector<rule_group> &groups) {
	ini_document document = existing;
	remove_group_sections(document);
	append_groups_v2(document, groups);
	return ini_serialize(document);
}

#if defined(_WIN32)

inline bool rules_file_exists(const std::wstring &path) {
	return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline bool rules_read_file(const std::wstring &path, std::string &bytes) {
	FILE *file = nullptr;
	if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
		return false;
	fseek(file, 0, SEEK_END);
	const long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (length < 0 || length > 1024 * 1024) {
		fclose(file);
		return false;
	}
	bytes.assign(static_cast<size_t>(length), '\0');
	const bool ok = bytes.empty() ||
		fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
	fclose(file);
	return ok;
}

inline bool rules_write_file(const std::wstring &path, const std::string &bytes) {
	FILE *file = nullptr;
	if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr)
		return false;
	const bool ok = bytes.empty() ||
		fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
	fclose(file);
	return ok;
}

struct load_groups_result {
	bool ok = false;
	bool migrated = false;
	bool backup_created = false;
	std::vector<rule_group> groups;
	std::string error;
};

inline load_groups_result load_rule_groups_file(const std::wstring &path) {
	load_groups_result result;
	std::string bytes;
	if (!rules_read_file(path, bytes)) {
		result.error = "cannot read config file";
		return result;
	}
	const ini_document document = ini_parse(bytes);
	if (ini_is_v2(document) || !ini_has_v1_groups(document)) {
		result.groups = groups_from_document(document);
		result.ok = true;
		return result;
	}
	result.groups = groups_from_v1(document);
	const ini_document migrated = migrate_v1_to_v2(document);
	const std::wstring backup_path = path + L".formatv1.bak";
	if (!rules_file_exists(backup_path)) {
		if (!rules_write_file(backup_path, bytes)) {
			result.error = "cannot create format v1 backup";
			return result;
		}
		result.backup_created = true;
	}
	if (!rules_write_file(path, ini_serialize(migrated))) {
		result.error = "cannot write migrated config";
		return result;
	}
	result.ok = true;
	result.migrated = true;
	return result;
}

inline bool save_rule_groups_file(const std::wstring &path,
	const std::vector<rule_group> &groups, std::string &error) {
	std::string bytes;
	ini_document document;
	if (rules_read_file(path, bytes))
		document = ini_parse(bytes);
	if (!rules_write_file(path, serialize_groups_v2(document, groups))) {
		error = "cannot write config file";
		return false;
	}
	return true;
}

inline bool load_nonindexed_rules_file(const std::wstring &path,
	std::vector<nonindexed_rule> &rules, std::string &error) {
	std::string bytes;
	if (!rules_read_file(path, bytes)) {
		error = "cannot read config file";
		return false;
	}
	rules = nonindexed_rules_from_document(ini_parse(bytes));
	return true;
}

#endif // _WIN32

} // namespace ns_alpha_rules
