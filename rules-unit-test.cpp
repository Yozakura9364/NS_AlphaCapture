#include "shader_rules.hpp"

#include <cstdio>
#include <string>

using namespace ns_alpha_rules;

static int g_failures = 0;

#define CHECK(condition) \
	do { \
		if (!(condition)) { \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			++g_failures; \
		} \
	} while (0)

static capture_rule make_rule(uint32_t ps, uint32_t vs, uint32_t first, uint32_t count,
	int32_t offset) {
	capture_rule rule;
	rule.enabled = true;
	rule.pixel = ps;
	rule.vertex = vs;
	rule.first_index = first;
	rule.index_count = count;
	rule.vertex_offset = offset;
	return rule;
}

static void test_exact_match() {
	const capture_rule rule = make_rule(2184442637u, 2160856356u, 0u, 6492u, 0);
	CHECK(rule_matches(rule, 2184442637u, 2160856356u, 0u, 6492u, 0));
	CHECK(!rule_matches(rule, 2184442637u, 2160856356u, 0u, 6492u, 1));
}

static void test_same_pixel_different_vertex_no_match() {
	const capture_rule rule = make_rule(100u, 200u, 0u, 10u, 0);
	CHECK(!rule_matches(rule, 100u, 201u, 0u, 10u, 0));
}

static void test_same_shaders_different_geometry_no_match() {
	const capture_rule rule = make_rule(100u, 200u, 0u, 6492u, 0);
	CHECK(!rule_matches(rule, 100u, 200u, 11040u, 6492u, 0));
	CHECK(!rule_matches(rule, 100u, 200u, 0u, 5472u, 0));
}

static void test_no_cartesian_product() {
	rule_group group;
	group.active = true;
	group.rules.push_back(make_rule(111u, 222u, 0u, 10u, 0));
	group.rules.push_back(make_rule(333u, 444u, 5u, 20u, 0));
	CHECK(group_matches(group, 111u, 222u, 0u, 10u, 0));
	CHECK(group_matches(group, 333u, 444u, 5u, 20u, 0));
	CHECK(!group_matches(group, 111u, 444u, 0u, 10u, 0));
	CHECK(!group_matches(group, 333u, 222u, 5u, 20u, 0));
	CHECK(!group_matches(group, 111u, 222u, 5u, 10u, 0));
}

static void test_disabled_and_incomplete_rules_never_match() {
	capture_rule rule = make_rule(100u, 200u, 0u, 10u, 0);
	rule.enabled = false;
	CHECK(!rule_matches(rule, 100u, 200u, 0u, 10u, 0));

	capture_rule pixel_only;
	pixel_only.enabled = true;
	pixel_only.pixel = 100u;
	CHECK(!rule_can_be_enabled(pixel_only));
	CHECK(!rule_matches(pixel_only, 100u, 200u, 0u, 10u, 0));

	rule_force_candidate_if_incomplete(pixel_only);
	CHECK(!pixel_only.enabled);
}

static void test_group_active_gates_matching() {
	rule_group group;
	group.active = false;
	group.rules.push_back(make_rule(100u, 200u, 0u, 10u, 0));
	CHECK(!group_matches(group, 100u, 200u, 0u, 10u, 0));
	group.active = true;
	CHECK(group_matches(group, 100u, 200u, 0u, 10u, 0));
}

static void test_nonindexed_composite_exact_match() {
	nonindexed_rule rule;
	rule.enabled = true;
	rule.pixel = 1956256419u;
	rule.vertex = 2589759975u;
	rule.vertex_count = 4;
	rule.instance_count = 1;
	rule.render_target_width = 3840;
	rule.render_target_height = 2160;
	CHECK(nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 3840, 2160, false, true, 5, 6, 7));
	CHECK(!nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 1920, 1080, false, true, 5, 6, 7));
	CHECK(!nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 3840, 2160, true, true, 5, 6, 7));
	CHECK(!nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 3, 1, 0, 0,
		0, 0, 0, 3840, 2160, false, true, 5, 6, 7));
	rule.enabled = false;
	CHECK(!nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 3840, 2160, false, true, 5, 6, 7));
}

static void test_nonindexed_one_inv_src_alpha_exact_match() {
	nonindexed_rule rule;
	rule.enabled = true;
	rule.pixel = 1956256419u;
	rule.vertex = 2589759975u;
	rule.vertex_count = 4;
	rule.instance_count = 1;
	rule.render_target_width = 1920;
	rule.render_target_height = 1080;
	rule.src_blend = 2;
	rule.dest_blend = 6;
	rule.write_mask = 7;
	CHECK(nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 1920, 1080, false, true, 2, 6, 7));
	CHECK(!nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 1920, 1080, false, true, 5, 6, 7));
	CHECK(!nonindexed_rule_matches(rule, 1956256419u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 3840, 2160, false, true, 2, 6, 7));
}

static void test_nonindexed_intermediate_rules_parse_and_match() {
	ini_document document;
	document.set("NonIndexedRules", "AmountRules", "2");
	document.set("NonIndexedRules", "Rule0",
		"1|1519164267|2589759975|4|1|0|0|0|0|0|1920|1080|1|0|2|1|15");
	document.set("NonIndexedRules", "Rule1",
		"1|1581445177|2602839305|4|1|0|0|0|0|0|480|270|1|0|2|1|15");
	const std::vector<nonindexed_rule> rules = nonindexed_rules_from_document(document);
	CHECK(rules.size() == 2);
	CHECK(nonindexed_rule_matches(rules[0], 1519164267u, 2589759975u, 4, 1, 0, 0,
		0, 0, 0, 1920, 1080, false, false, 2, 1, 15));
	CHECK(nonindexed_rule_matches(rules[1], 1581445177u, 2602839305u, 4, 1, 0, 0,
		0, 0, 0, 480, 270, false, false, 2, 1, 15));
	CHECK(!nonindexed_rule_matches(rules[1], 1581445177u, 2602839305u, 4, 1, 0, 0,
		0, 0, 0, 1920, 1080, false, false, 2, 1, 15));
}

static const char *kLegacyV1 =
	"[Capture]\r\n"
	"OutputTransparent=1\r\n"
	"\r\n"
	"[General]\r\n"
	"AmountGroups=2\r\n"
	"\r\n"
	"[Group0_PixelShaders]\r\n"
	"ShaderHash0=2184442637\r\n"
	"AmountHashes=1\r\n"
	"\r\n"
	"[Group0_VertexShaders]\r\n"
	"ShaderHash0=2160856356\r\n"
	"AmountHashes=1\r\n"
	"\r\n"
	"[Group0]\r\n"
	"Name=Main\r\n"
	"ToggleKey=0\r\n"
	"IsActiveAtStartup=True\r\n"
	"\r\n"
	"[Group0_DrawRules]\r\n"
	"Rule0=2184442637|2160856356|0|6492|0\r\n"
	"AmountRules=1\r\n"
	"\r\n"
	"[Group1_PixelShaders]\r\n"
	"ShaderHash0=1956256419\r\n"
	"AmountHashes=1\r\n"
	"\r\n"
	"[Group1_VertexShaders]\r\n"
	"AmountHashes=0\r\n"
	"\r\n"
	"[Group1]\r\n"
	"Name=Candidate\r\n"
	"ToggleKey=0\r\n"
	"IsActiveAtStartup=True\r\n";

static void test_v1_migration_preserves_groups_and_disables_hash_only() {
	const ini_document v1 = ini_parse(kLegacyV1);
	const std::vector<rule_group> groups = groups_from_v1(v1);
	CHECK(groups.size() == 2);
	CHECK(groups[0].name == "Main");
	CHECK(groups[0].rules.size() == 1);
	CHECK(groups[0].rules[0].enabled);
	CHECK(groups[0].rules[0].pixel == 2184442637u);
	CHECK(groups[0].rules[0].vertex == 2160856356u);
	CHECK(groups[0].rules[0].index_count == 6492u);
	CHECK(groups[1].name == "Candidate");
	CHECK(groups[1].rules.size() == 1);
	CHECK(!groups[1].rules[0].enabled);
	CHECK(groups[1].rules[0].pixel == 1956256419u);
	CHECK(groups[1].rules[0].vertex == 0u);
	CHECK(groups[1].active_at_startup);
}

static void test_v1_migration_removes_old_sections() {
	const ini_document v1 = ini_parse(kLegacyV1);
	const ini_document v2 = migrate_v1_to_v2(v1);
	CHECK(v2.find("Group0_PixelShaders") == nullptr);
	CHECK(v2.find("Group0_VertexShaders") == nullptr);
	CHECK(v2.find("Group0_DrawRules") == nullptr);
	CHECK(v2.find("Group1_PixelShaders") == nullptr);
	CHECK(v2.find("Group1_VertexShaders") == nullptr);
	CHECK(v2.find("Group0_Rules") != nullptr);
	CHECK(v2.find("Group1_Rules") != nullptr);
	uint32_t version = 0;
	CHECK(ini_get_u32(v2, "General", "FormatVersion", version) && version == 2);
	CHECK(v2.find("Capture") != nullptr);
}

static void test_v2_round_trip() {
	std::vector<rule_group> groups;
	rule_group first;
	first.name = "Rule | with separator, and = signs";
	first.toggle_key_packed = 0x79000101u;
	first.active_at_startup = true;
	first.rules.push_back(make_rule(111u, 222u, 0u, 6492u, 0));
	first.rules[0].name = "name | with = separators";
	capture_rule candidate;
	candidate.pixel = 555u;
	candidate.name = "candidate";
	first.rules.push_back(candidate);
	rule_group second;
	second.name = "Empty";
	groups.push_back(first);
	groups.push_back(second);

	ini_document document;
	const std::string text = serialize_groups_v2(document, groups);
	const ini_document parsed = ini_parse(text);
	CHECK(ini_is_v2(parsed));
	const std::vector<rule_group> loaded = groups_from_v2(parsed);
	CHECK(loaded.size() == 2);
	CHECK(loaded[0].name == first.name);
	CHECK(loaded[0].toggle_key_packed == first.toggle_key_packed);
	CHECK(loaded[0].active_at_startup);
	CHECK(loaded[0].rules.size() == 2);
	CHECK(loaded[0].rules[0].enabled);
	CHECK(rule_signature_equals(loaded[0].rules[0], first.rules[0]));
	CHECK(loaded[0].rules[0].name == "name | with = separators");
	CHECK(!loaded[0].rules[1].enabled);
	CHECK(loaded[0].rules[1].pixel == 555u);
	CHECK(loaded[0].rules[1].name == "candidate");
	CHECK(loaded[1].rules.empty());

	const std::string text2 = serialize_groups_v2(parsed, loaded);
	CHECK(text == text2);
}

static void test_v2_enabled_incomplete_rule_loads_as_candidate() {
	ini_document document;
	document.set("General", "FormatVersion", "2");
	document.set("General", "AmountGroups", "1");
	document.set("Group0", "Name", "Broken");
	document.set("Group0_Rules", "AmountRules", "1");
	document.set("Group0_Rules", "Rule0", "1|1956256419|0|0|0|0");
	const std::vector<rule_group> groups = groups_from_v2(document);
	CHECK(groups.size() == 1);
	CHECK(groups[0].rules.size() == 1);
	CHECK(!groups[0].rules[0].enabled);
}

static void test_delete_group_leaves_no_residue() {
	const ini_document v1 = ini_parse(kLegacyV1);
	std::vector<rule_group> groups = groups_from_v1(v1);
	groups.erase(groups.begin());
	const ini_document v2 = migrate_v1_to_v2(v1);
	const std::string text = serialize_groups_v2(v2, groups);
	const ini_document written = ini_parse(text);
	CHECK(written.find("Group1") == nullptr);
	CHECK(written.find("Group1_Rules") == nullptr);
	CHECK(written.find("Group1_PixelShaders") == nullptr);
	CHECK(written.find("Group0_PixelShaders") == nullptr);
	CHECK(written.find("Group0_DrawRules") == nullptr);
	uint32_t amount = 0;
	CHECK(ini_get_u32(written, "General", "AmountGroups", amount) && amount == 1);
	const std::vector<rule_group> loaded = groups_from_v2(written);
	CHECK(loaded.size() == 1);
	CHECK(loaded[0].name == "Candidate");
}

static void test_cancel_edit_keeps_saved_state() {
	const ini_document v1 = ini_parse(kLegacyV1);
	const std::vector<rule_group> saved = groups_from_v1(v1);
	std::vector<rule_group> working = saved;
	working[0].name = "Edited Name";
	working[0].rules.clear();
	working.erase(working.begin() + 1);
	CHECK(saved.size() == 2);
	CHECK(saved[0].name == "Main");
	CHECK(saved[0].rules.size() == 1);
	CHECK(saved[1].name == "Candidate");
}

static void test_preview_is_transient_and_restores() {
	preview_state preview;
	CHECK(!preview.active);
	CHECK(!preview_enter_isolation(preview, 100u, true));
	CHECK(!preview.active);
	CHECK(preview_enter_isolation(preview, 100u, false));
	CHECK(preview_hides_draw(preview, {}, 100u, 1u, 0u, 0u, 0));
	CHECK(!preview_hides_draw(preview, {}, 101u, 1u, 0u, 0u, 0));
	preview_exit(preview);
	CHECK(!preview.active);
	CHECK(preview.kind == 0);
	CHECK(preview.isolation_pixel == 0);
	CHECK(!preview_hides_draw(preview, {}, 100u, 1u, 0u, 0u, 0));

	std::vector<rule_group> groups;
	rule_group group;
	group.rules.push_back(make_rule(100u, 200u, 0u, 10u, 0));
	groups.push_back(group);
	CHECK(!preview_enter_group(preview, 5, groups.size(), false));
	CHECK(preview_enter_group(preview, 0, groups.size(), false));
	CHECK(preview_hides_draw(preview, groups, 100u, 200u, 0u, 10u, 0));
	CHECK(!preview_hides_draw(preview, groups, 100u, 201u, 0u, 10u, 0));
	preview_exit(preview);
	CHECK(!preview_hides_draw(preview, groups, 100u, 200u, 0u, 10u, 0));
}

#if defined(_WIN32)

static void test_file_migration_and_backup_once() {
	wchar_t temp_dir[MAX_PATH] = {};
	CHECK(GetTempPathW(MAX_PATH, temp_dir) != 0);
	std::wstring base = temp_dir;
	base += L"ns_alpha_rules_test.ini";
	std::wstring backup = base + L".formatv1.bak";
	DeleteFileW(base.c_str());
	DeleteFileW(backup.c_str());

	CHECK(rules_write_file(base, kLegacyV1));
	load_groups_result first = load_rule_groups_file(base);
	CHECK(first.ok);
	CHECK(first.migrated);
	CHECK(first.backup_created);
	CHECK(first.groups.size() == 2);

	std::string backup_bytes;
	CHECK(rules_read_file(backup, backup_bytes));
	CHECK(backup_bytes == kLegacyV1);

	const std::string tampered = std::string("tampered");
	CHECK(rules_write_file(backup, tampered));
	load_groups_result second = load_rule_groups_file(base);
	CHECK(second.ok);
	CHECK(!second.migrated);
	CHECK(!second.backup_created);
	std::string backup_after;
	CHECK(rules_read_file(backup, backup_after));
	CHECK(backup_after == tampered);

	std::string migrated_text;
	CHECK(rules_read_file(base, migrated_text));
	const ini_document migrated_doc = ini_parse(migrated_text);
	CHECK(migrated_doc.find("Group0_PixelShaders") == nullptr);
	CHECK(migrated_doc.find("Group0_DrawRules") == nullptr);
	CHECK(ini_is_v2(migrated_doc));

	first.groups.erase(first.groups.begin());
	std::string error;
	CHECK(save_rule_groups_file(base, first.groups, error));
	std::string saved_text;
	CHECK(rules_read_file(base, saved_text));
	const ini_document saved_doc = ini_parse(saved_text);
	uint32_t amount = 0;
	CHECK(ini_get_u32(saved_doc, "General", "AmountGroups", amount) && amount == 1);
	CHECK(saved_doc.find("Group1") == nullptr);
	CHECK(saved_doc.find("Group0_Rules") != nullptr);
	CHECK(saved_doc.find("Capture") != nullptr);

	DeleteFileW(base.c_str());
	DeleteFileW(backup.c_str());
}

#endif

int main() {
	test_exact_match();
	test_same_pixel_different_vertex_no_match();
	test_same_shaders_different_geometry_no_match();
	test_no_cartesian_product();
	test_disabled_and_incomplete_rules_never_match();
	test_group_active_gates_matching();
	test_nonindexed_composite_exact_match();
	test_nonindexed_one_inv_src_alpha_exact_match();
	test_nonindexed_intermediate_rules_parse_and_match();
	test_v1_migration_preserves_groups_and_disables_hash_only();
	test_v1_migration_removes_old_sections();
	test_v2_round_trip();
	test_v2_enabled_incomplete_rule_loads_as_candidate();
	test_delete_group_leaves_no_residue();
	test_cancel_edit_keeps_saved_state();
	test_preview_is_transient_and_restores();
#if defined(_WIN32)
	test_file_migration_and_backup_once();
#endif
	if (g_failures != 0) {
		std::printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("all rule unit tests passed\n");
	return 0;
}
