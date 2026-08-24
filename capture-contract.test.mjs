import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import test from 'node:test'

const read = (name) => readFileSync(new URL(name, import.meta.url), 'utf8')

test('addon exposes one capture hotkey and never switches black or white frame modes', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(config, /^CaptureKey=F10$/m)
  assert.match(config, /^CaptureModifiers=Ctrl\+Shift$/m)
  assert.doesNotMatch(config, /^(?:Mode|BlackKey|WhiteKey|OffKey)=/m)
  assert.doesNotMatch(source, /capture_mode::(?:black|white)/)
})

test('capture output and runtime log are routed outside the game directory', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(config, /^OutputDirectory=$/m)
  assert.match(source, /_wfopen_s\([^\n]*g\.config_path\.c_str\(\), L"rb"\)/)
  assert.match(source, /MultiByteToWideChar\(CP_UTF8, MB_ERR_INVALID_CHARS/)
  assert.doesNotMatch(source, /GetPrivateProfileString[AW]/)
  assert.match(source, /g\.log_path = base \+ L"\\\\NS_AlphaCapture\.log"/)
  assert.doesNotMatch(source, /g\.log_path = g\.output_dir \+ L"\\\\NS_AlphaCapture\.log"/)
})

test('output selection defaults to transparent only and controls only disk exports', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')
  const replayExport = source.match(/bool capture_replay_outputs\([^]*?\n\}/)?.[0] ?? ''

  assert.match(config, /^OutputBlack=0$/m)
  assert.match(config, /^OutputWhite=0$/m)
  assert.match(config, /^OutputTransparent=1$/m)
  assert.match(source, /bool output_black = false/)
  assert.match(source, /bool output_white = false/)
  assert.match(source, /bool output_transparent = true/)
  for (const key of ['OutputBlack', 'OutputWhite', 'OutputTransparent'])
    assert.match(source, new RegExp(key))
  for (const label of [
    'Output', '输出',
    'Black background', '黑底图',
    'White background', '白底图',
    'Transparent', '透明图',
  ]) assert.match(source, new RegExp(label))
  assert.match(source, /save_output_selection/)
  assert.match(replayExport, /g\.cfg\.output_black/)
  assert.match(replayExport, /g\.cfg\.output_white/)
  assert.match(replayExport, /g\.cfg\.output_transparent/)
  assert.doesNotMatch(replayExport, /_rgba\.png|_alpha\.png|_rgba32f\.bin|_lens_/)
})

test('capture filenames use a configurable ReShade-style token template', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(config, /^FileNaming=$/m)
  assert.match(source, /default_file_naming/)
  assert.match(source, /expand_file_naming/)
  assert.match(source, /expand_reshade_macro_string/)
  assert.match(source, /current_preset_name/)
  assert.match(source, /current_app_name/)
  assert.match(source, /_stricmp\(name\.c_str\(\), macro\.first\.c_str\(\)\)/)
  assert.match(source, /macro_end == macro_begin \+ 1/)
  assert.match(source, /sanitize_capture_file_name/)
  assert.match(source, /save_file_naming/)
  assert.match(source, /Screenshot filename|截图文件名/)
  assert.match(source, /sprintf_s\(date, "%04u-%02u-%02u"/)
  assert.match(source, /const std::wstring black_path = prefix \+ L"_Black\.png"/)
  assert.match(source, /const std::wstring white_path = prefix \+ L"_White\.png"/)
  assert.match(source, /const std::wstring final_path = prefix \+ L"_Final\.png"/)
  for (const token of ['AppName', 'PresetName', 'Count', 'Date', 'DateYear', 'DateMonth',
    'DateDay', 'Time', 'TimeHour', 'TimeMinute', 'TimeSecond', 'TimeMillisecond'])
    assert.match(source, new RegExp(`\\{ "${token}"`))
})

test('capture is armed for exactly the next frame instead of replaying every frame', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const drawCallback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''
  const presentCallback = source.match(/void on_reshade_present\([^]*?\n\}/)?.[0] ?? ''

  assert.match(drawCallback, /!g\.replay_capture_active/)
  assert.match(presentCallback, /g\.replay_capture_active\s*=\s*true/)
  assert.match(presentCallback, /capture_replay_outputs\(runtime\)/)
  assert.match(presentCallback, /g\.replay_capture_active\s*=\s*false/)
  assert.match(source, /Capturing next color frame/)
})

test('automatic matcher learns runtime dither meshes instead of requiring item hashes', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')
  const equality = source.match(/bool operator==\(const mesh_signature &other\) const \{[^]*?\n\t\}/)?.[0] ?? ''
  const hash = source.match(/struct mesh_signature_hash \{[^]*?\n\};/)?.[0] ?? ''

  assert.match(config, /^AutoMatch=1$/m)
  assert.match(source, /shader_contains_discard/)
  assert.match(source, /query_bound_a8_beacon/)
  assert.match(source, /query_runtime_dither_state/)
  assert.match(source, /learned_meshes/)
  assert.match(source, /query_color_replay_state/)
  assert.match(source, /D3D11_FORMAT_SUPPORT_RENDER_TARGET/)
  assert.doesNotMatch(equality, /index_buffer == other\.index_buffer/)
  assert.doesNotMatch(equality, /vertex_buffer0 == other\.vertex_buffer0/)
  assert.doesNotMatch(hash, /mesh\.index_buffer/)
  assert.doesNotMatch(hash, /mesh\.vertex_buffer0/)
})

test('lens-only mode is opt-in and filters replay to the confirmed lens draw', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(config, /^LensOnly=0$/m)
  assert.match(config, /^LensCapture=1$/m)
  assert.match(config, /^LensPixelShaderHash=3361469263$/m)
  assert.match(config, /^LensFirstIndex=6448$/m)
  assert.match(config, /^LensIndexCount=276$/m)
  assert.match(source, /bool lens_only = false/)
  assert.match(source, /bool lens_capture = true/)
  assert.match(source, /is_lens_target\(/)
  assert.match(source, /if \(g\.cfg\.lens_only\)/)
  assert.match(source, /g\.cfg\.lens_capture/)
  assert.match(source, /hashes\.pixel != g\.cfg\.lens_pixel_shader_hash/)
  assert.match(source, /first_index != g\.cfg\.lens_first_index/)
})

test('the additive VFX hash stays a disabled candidate without a full signature', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(config, /\[Group5_Rules\][\s\S]*Rule0=0\|1956256419\|0\|0\|0\|0/)
  assert.doesNotMatch(config, /Rule\d+=1\|1956256419\|0/)
  assert.doesNotMatch(config, /\[Group\d+_PixelShaders\]/)
  assert.doesNotMatch(config, /\[Group\d+_DrawRules\]/)
  assert.match(source, /is_configured_shader_rule\(/)
  assert.match(callback, /is_configured_shader_rule\(hashes/)
  assert.match(callback, /replay_color_draw\([^]*?hashes\.pixel, false\)/)
})

test('exact rules live in format v2 sections and drive selectable replay', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')
  const rules = read('./shader_rules.hpp')

  assert.match(config, /^FormatVersion=2$/m)
  assert.match(config, /^AmountGroups=6$/m)
  assert.match(config, /^\[Group0\]$/m)
  assert.match(config, /^Name=主体材质$/m)
  assert.match(config, /^\[Group5\]$/m)
  assert.match(config, /^\[Group0_Rules\]$/m)
  assert.match(config, /^Rule0=1\|2184442637\|2160856356\|0\|6492\|0$/m)
  assert.match(config, /^Rule0Name=/m)
  assert.match(rules, /struct capture_rule/)
  assert.match(rules, /struct rule_group/)
  assert.match(rules, /rule_has_full_signature/)
  assert.match(rules, /rule_matches\(/)
  assert.match(rules, /groups_match\(/)
  assert.match(rules, /ToggleKey/)
  assert.match(rules, /IsActiveAtStartup/)
  assert.match(source, /ns_alpha_rules::groups_match\(g\.rule_groups/)
  assert.match(source, /is_configured_shader_rule\(/)
})

test('rule editing validates before saving and cancel keeps saved state', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /validate_rule_editor\(/)
  assert.match(source, /rule_can_be_enabled/)
  assert.match(source, /rule_signature_equals/)
  assert.match(source, /rule_editor_work/)
  assert.match(source, /group_editor_work/)
  assert.match(source, /校验并保存|Validate and save/)
  assert.match(source, /text\("Cancel", "取消"\)/)
})

test('migration removes v1 sections and protects the one-time backup', () => {
  const rules = read('./shader_rules.hpp')

  assert.match(rules, /migrate_v1_to_v2/)
  assert.match(rules, /remove_group_sections/)
  assert.match(rules, /"_PixelShaders"/)
  assert.match(rules, /"_VertexShaders"/)
  assert.match(rules, /"_DrawRules"/)
  assert.match(rules, /formatv1\.bak/)
  assert.match(rules, /!rules_file_exists\(backup_path\)/)
  assert.match(rules, /Rule" \+ std::to_string\(rule_index\) \+ "Name"/)
})

test('hunting offers deterministic navigation and transient preview only', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const rules = read('./shader_rules.hpp')

  assert.match(source, /hunting_open/)
  assert.match(source, /shader_selector_active/)
  assert.match(source, /shader_candidates/)
  assert.match(source, /sorted_candidate_indices/)
  assert.match(source, /collect_unique_candidate_hashes/)
  assert.match(source, /std::sort\(hashes\.begin\(\), hashes\.end\(\)\)/)
  assert.match(source, /text\("First", "首项"\)/)
  assert.match(source, /text\("Last", "末项"\)/)
  assert.match(source, /text\("Mark", "标记"\)/)
  assert.match(source, /text\("Add candidate", "加入候选"\)/)
  assert.match(source, /rule\.enabled = false/)
  assert.match(source, /preview_enter_isolation\(g\.preview, current,\s*g\.replay_capture_active\)/)
  assert.match(rules, /preview_exit/)
  assert.match(rules, /preview = preview_state\{\}/)
  assert.match(rules, /if \(capture_active \|\| pixel == 0\)\s*return false/)
})

test('preview never interferes with the capture chain', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(callback, /!g\.replay_capture_active && g\.preview\.active/)
  assert.match(callback, /preview_hides_draw\(/)
})

test('shader groups use Shader Toggler key packing and full-width shortcut bars', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const config = read('./NS_AlphaCapture.ini')
  const configStruct = source.match(/struct config \{[^]*?\n\};/)?.[0] ?? ''

  assert.match(config, /^ToggleKey=0$/m)
  assert.doesNotMatch(configStruct, /rule_group/)
  assert.match(source, /load_rule_groups_file\(g\.config_path\)/)
  assert.match(source, /g\.rule_groups = std::move\(fresh_rule_groups\)/)
  assert.match(source, /pack_toggle_key\(/)
  assert.match(source, /unpack_toggle_key\(/)
  assert.match(source, /save_rule_groups_file/)
  assert.match(source, /Ctrl \+ /)
  assert.match(source, /Shift \+ /)
  assert.doesNotMatch(source, /Shader Toggler 风格的 INI 分节/)
})

test('lens-only replay accepts the confirmed opaque lens blend without relaxing ordinary filtering', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /bool query_lens_replay_state\(/)
  assert.match(source, /!blend_desc\.BlendEnable/)
  assert.match(source, /g\.replay\.opaque_blend/)
  assert.match(callback, /query_lens_replay_state\(/)
  assert.match(callback, /replay_color_draw\([^]*?true\)/)
  assert.match(replay, /bool lens_only/)
  assert.match(replay, /classify_original_blend\(/)
})

test('lens-only replay substitutes scene-color SRVs with separate black and white resources', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /scene_black_texture/)
  assert.match(source, /scene_white_texture/)
  assert.match(source, /scene_black_srv/)
  assert.match(source, /scene_white_srv/)
  assert.match(source, /PSGetShaderResources/)
  assert.match(source, /PSSetShaderResources/)
  assert.match(source, /copy_scene_color_substitutes/)
  assert.match(replay, /bind_lens_scene_srvs\([^]*?scene_black_srv/)
  assert.match(replay, /bind_lens_scene_srvs\([^]*?scene_white_srv/)
  assert.match(replay, /restore_lens_scene_srvs/)
})

test('ordinary capture isolates the confirmed lens draw without enabling lens-only mode', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(callback, /g\.replay_capture_active && g\.cfg\.lens_capture/)
  assert.match(callback, /is_lens_target\([^]*?query_lens_replay_state/)
  assert.match(callback, /replay_color_draw\([^]*?hashes\.pixel, true\)/)
  assert.match(replay, /if \(lens_only\)\s*\{[^}]*copy_scene_color_substitutes\(context\)/)
  assert.ok(
    replay.indexOf('copy_scene_color_substitutes(context)') <
      replay.indexOf('bind_lens_scene_srvs(context, g.replay.scene_black_srv.Get()'),
  )
})

test('addon exports its own synchronized RGBA reconstruction without requiring FX output', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /const rgba_image &final_rgba = rgba/)
  assert.match(source, /_Final\.png/)
  assert.match(source, /capture pixels: black_nonzero=/)
})

test('one capture keeps lens replay inside the selected core images without extra lens files', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replayExport = source.match(/bool capture_replay_outputs\([^]*?\n\}/)?.[0] ?? ''
  const presentCallback = source.match(/void on_reshade_present\([^]*?\n\}/)?.[0] ?? ''

  assert.match(replayExport, /const rgba_image &final_rgba = rgba/)
  assert.doesNotMatch(replayExport, /capture_lens_effect_outputs|_lens_/)
  assert.match(presentCallback, /capture_replay_outputs\(runtime\)/)
  assert.doesNotMatch(source, /set_technique_state/)
})

test('effect consumes addon-rendered black and white targets without synthesizing either background', () => {
  const effect = read('./NS_VFXCapture.fx')

  assert.match(effect, /texture NS_VFX_Black\s*:\s*NS_ALPHA_CAPTURE_BLACK/)
  assert.match(effect, /texture NS_VFX_White\s*:\s*NS_ALPHA_CAPTURE_WHITE/)
  assert.doesNotMatch(effect, /pass InitializeBlackAndWhiteTargets/)
  assert.doesNotMatch(effect, /pass CompositePreparedOnBlack/)
  assert.doesNotMatch(effect, /pass CompositePreparedOnWhite/)
  assert.match(effect, /tex2D\(NS_CAPTURE_BlackSampler/)
  assert.match(effect, /tex2D\(NS_CAPTURE_WhiteSampler/)
  assert.match(effect, /uncovered\s*=\s*dot\(saturate\(whiteRGB\s*-\s*blackRGB\)/)
  assert.match(effect, /alpha\s*=\s*saturate\(1\.0\s*-\s*uncovered\)/)
  assert.doesNotMatch(effect, /prepared\.rgb\s*\+\s*\(1\.0\s*-\s*alpha\)/)
})

test('addon conditionally saves synchronized black, white and final images', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /read_replay_rgba32f\(g\.replay\.black_texture\.Get\(\)/)
  assert.match(source, /read_replay_rgba32f\(g\.replay\.white_texture\.Get\(\)/)
  assert.match(source, /_black\.png/)
  assert.match(source, /_white\.png/)
  assert.match(source, /save_texture_png\(black_path, black_rgba/)
  assert.match(source, /save_texture_png\(white_path, white_rgba/)
  assert.match(source, /save_texture_png\(final_path, final_rgba/)
  assert.match(source, /g\.cfg\.output_black/)
  assert.match(source, /g\.cfg\.output_white/)
  assert.match(source, /g\.cfg\.output_transparent/)
  assert.match(source, /DeleteFileW\(black_path\.c_str\(\)\)/)
  assert.match(source, /DeleteFileW\(white_path\.c_str\(\)\)/)
})

test('capture success and failure are shown in a foreground top banner', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /addon_event::reshade_overlay/)
  assert.match(source, /notification_message/)
  assert.match(source, /notification_success/)
  assert.match(source, /GetForegroundDrawList\(\)/)
  assert.match(source, /AddRectFilled\(/)
  assert.match(source, /AddText\(/)
  assert.match(source, /Selected images saved/)
  assert.match(source, /Capture failed/)
})

test('addon settings expose editable capture and reload shortcuts', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /register_overlay\(nullptr, on_settings_overlay\)/)
  assert.match(source, /unregister_overlay\(nullptr, on_settings_overlay\)/)
  assert.match(source, /ImGui::Button\(/)
  assert.match(source, /ImGui::IsKeyPressed\(/)
  assert.match(source, /ImGui::SetNextFrameWantCaptureKeyboard\(true\)/)
  assert.match(source, /save_hotkey\(/)
  assert.match(source, /CaptureKey/)
  assert.match(source, /CaptureModifiers/)
  assert.match(source, /ReloadKey/)
  assert.match(source, /ReloadModifiers/)
  assert.match(source, /Screenshot shortcut", "截图快捷键/)
  assert.match(source, /Reload shortcut", "重载快捷键/)
  assert.doesNotMatch(source, /Capture shortcut|Reload configuration|捕获快捷键|重新载入配置/)
})

test('shortcut capture keeps modifiers across frames and runtime matching uses ReShade key state', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const runtimeModifiers = source.match(
    /uint32_t current_runtime_modifiers\(effect_runtime \*runtime\)\s*\{[^]*?\n\}/,
  )?.[0] ?? ''

  assert.match(source, /hotkey_modifier_latch/)
  assert.match(runtimeModifiers, /runtime->is_key_down/)
  assert.match(runtimeModifiers, /VK_CONTROL/)
  assert.match(runtimeModifiers, /VK_SHIFT/)
  assert.match(runtimeModifiers, /VK_MENU/)
  assert.match(source, /runtime->is_key_pressed\(key\)/)
  assert.match(source, /current_runtime_modifiers\(runtime\) == modifiers/)
  assert.match(source, /g\.hotkey_modifier_latch \|=/)
  assert.match(source, /process_hotkey_capture\(runtime\)/)
})

test('Win is disabled as a shortcut modifier while legacy INI values remain readable', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const imguiModifiers = source.match(/uint32_t current_imgui_modifiers\(\)\s*\{[^]*?\n\}/)?.[0] ?? ''
  const runtimeModifiers = source.match(
    /uint32_t current_runtime_modifiers\(effect_runtime \*runtime\)\s*\{[^]*?\n\}/,
  )?.[0] ?? ''
  const formatter = source.match(/std::string format_hotkey\([^]*?\n\}/)?.[0] ?? ''
  const serializer = source.match(/std::string hotkey_modifier_name\([^]*?\n\}/)?.[0] ?? ''

  assert.doesNotMatch(imguiModifiers, /Super/)
  assert.doesNotMatch(runtimeModifiers, /VK_[LR]WIN/)
  assert.doesNotMatch(formatter, /modifier_win|Win \+/)
  assert.doesNotMatch(serializer, /modifier_win|Win\+/)
  assert.match(source, /ns_white_backing::sanitize_modifiers\(parsed\)/)
  assert.match(source, /ns_white_backing::sanitize_modifiers\(modifiers\)/)
})

test('addon settings and foreground notifications follow ReShade zh-CN language', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /\[OVERLAY\].*Language|"OVERLAY", "Language"/s)
  assert.match(source, /language\.rfind\("zh-", 0\) == 0/)
  assert.match(source, /language\.rfind\("zh_", 0\) == 0/)
  assert.match(source, /language == "6"/)
  assert.match(source, /read_reshade_setting\("OVERLAY", "Language", language\)/)
  assert.match(source, /get_config_value\(nullptr, section, key/)
  assert.match(source, /GetThreadPreferredUILanguages/)
  assert.match(source, /MUI_LANGUAGE_NAME \| MUI_UI_FALLBACK/)
  assert.match(source, /Screenshot path|截图路径/)
  assert.match(source, /当前帧没有重放到目标颜色绘制/)
  for (const english of [
    'Screenshot shortcut',
    'Reload shortcut',
    'Press a new shortcut',
    'Shortcut saved',
    'Capturing next color frame',
    'Configuration reloaded',
    'Selected images saved',
    'Capture failed',
  ]) assert.match(source, new RegExp(english.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')))
  for (const chinese of [
    '截图快捷键',
    '重载快捷键',
    '请按下新的快捷键',
    '快捷键已保存',
    '正在捕获下一帧画面',
    '配置已重新载入',
    '已保存所选图片',
    '捕获失败',
  ]) assert.match(source, new RegExp(chinese))
})

test('reload defaults to Ctrl+Shift+F9 and first load inherits ReShade screenshot path', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(config, /^ReloadKey=F9$/m)
  assert.match(config, /^ReloadModifiers=Ctrl\+Shift$/m)
  assert.match(source, /SavePath/)
  assert.match(source, /FileNaming/)
  assert.match(source, /reshade_ini_path/)
  assert.match(source, /SCREENSHOT/)
  assert.match(config, /^FileNaming=$/m)
  assert.match(source, /get_config_value\(nullptr, section, key/)
})

test('shortcut and screenshot path controls fill the remaining settings width', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const settings = source.slice(
    source.indexOf('void on_settings_overlay'),
    source.indexOf('bool output_black', source.indexOf('void on_settings_overlay')),
  )

  assert.match(settings, /ImGui::BeginTable\(/)
  assert.match(settings, /ImGuiTableColumnFlags_WidthFixed/)
  assert.match(settings, /ImGuiTableColumnFlags_WidthStretch/)
  assert.match(settings, /ImGui::CalcTextSize/)
  assert.match(settings, /ImGuiStyleVar_CellPadding/)
  assert.doesNotMatch(settings, /WidthFixed, (?:160|90)\.0f/)
  assert.match(settings, /ImGui::GetContentRegionAvail\(\)\.x/)
  assert.doesNotMatch(settings, /SameLine\(220\.0f\)|SetNextItemWidth\(360\.0f\)|Indent\(220\.0f\)/)
})

test('rule editor columns are resizable, horizontally scrollable, and retain user widths', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const editor = source.slice(
    source.indexOf('void draw_rule_editor_window'),
    source.indexOf('void draw_hunting_window', source.indexOf('void draw_rule_editor_window')),
  )

  assert.match(editor, /ImGuiTableFlags_Resizable/)
  assert.match(editor, /ImGuiTableFlags_ScrollX/)
  assert.match(editor, /ImGuiTableFlags_SizingFixedFit/)
  assert.doesNotMatch(editor, /ImGuiTableFlags_NoSavedSettings/)
  assert.doesNotMatch(editor, /ImGuiTableColumnFlags_NoResize/)
})

test('settings UI keeps long labels and editor state from losing user changes', () => {
  const source = read('./NS_AlphaCapture.cpp')
  assert.match(source, /BeginChild\("##rule_table_scroll"/)
  assert.match(source, /SmallButton\("X##delete_rule"\)/)
  assert.match(source, /hotkey_conflicts\(/)
  assert.match(source, /g\.hunting_target_group > static_cast<int>\(index\)/)
  assert.match(source, /Close the rule editor before adding candidates/)
  assert.match(source, /unsaved edits discarded/)
  assert.match(source, /std::min\(text_size\.x \+ 36\.0f, display_size\.x - 40\.0f\)/)
})

test('native screenshot path supports ReShade and GShade with Pictures fallback', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /ReShade\.ini/)
  assert.match(source, /GShade\.ini/)
  assert.match(source, /FOLDERID_Pictures|CSIDL_MYPICTURES/)
  assert.match(source, /SCREENSHOT/)
  assert.match(source, /SavePath/)
  assert.match(source, /FileNaming/)
  assert.match(source, /get_config_value\(nullptr, section, key/)
})

test('stable build identifies the author and contains no lighting experiment chain', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const resource = read('./NS_AlphaCapture.rc')
  const build = read('./build.cmd')
  const config = read('./NS_AlphaCapture.ini')
  const readme = read('./README.md')

  assert.match(resource, /VALUE "ProductName", "NS Alpha Capture"/)
  assert.match(resource, /VALUE "CompanyName", "Nightingale Silence"/)
  assert.match(resource, /VALUE "FileDescription", "Transparent RGBA capture addon for FINAL FANTASY XIV"/)
  assert.match(resource, /FILEVERSION 0,3,2,0/)
  assert.match(resource, /PRODUCTVERSION 0,3,2,0/)
  assert.match(build, /rc \/nologo \/fo NS_AlphaCapture\.res NS_AlphaCapture\.rc/)
  assert.match(build, /NS_AlphaCapture\.cpp NS_AlphaCapture\.res/)
  assert.doesNotMatch(source, /Author: Nightingale Silence|作者：Nightingale Silence/)
  assert.match(readme, /作者：Nightingale Silence/)
  for (const experiment of ['SceneColorReplay', 'LightDrawProbe', 'Rule7']) {
    assert.doesNotMatch(source, new RegExp(experiment))
    assert.doesNotMatch(config, new RegExp(experiment))
  }
})

test('screenshot path is editable and persists to the addon configuration', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /ImGui::InputText\(/)
  assert.match(source, /save_output_directory\(/)
  assert.match(source, /replace_ini_value\([^]*?"OutputDirectory"/)
  for (const label of ['Save path', '保存路径', 'Use ReShade/GShade path', '使用 ReShade/GShade 路径'])
    assert.match(source, new RegExp(label))
  assert.match(source, /Screenshot path saved/)
  assert.match(source, /截图路径已保存/)
})

test('capture arming is observable in the runtime log', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /log_line\("capture armed: next frame capture active"\)/)
  assert.match(source, /capture pixels: black_nonzero=/)
})

test('color replay tracks shader hashes and targets only captured mesh signatures', () => {
  const config = read('./NS_AlphaCapture.ini')
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /compute_crc32\(/)
  assert.match(source, /addon_event::init_pipeline/)
  assert.match(source, /addon_event::bind_pipeline/)
  for (const rule of [
    '1|2184442637|2160856356|0|6492|0',
    '1|2184442637|2160856356|11040|5472|0',
    '1|3782231024|696698206|6496|2484|0',
    '1|3401395384|696698206|2920|576|0',
    '1|1120170840|696698206|2920|576|0',
  ]) assert.match(config, new RegExp(`Rule\\d+=${rule.replaceAll('|', '\\\\|')}`))
  assert.match(source, /is_configured_shader_rule\(/)
})

test('color replay uses the original DSV with compatible private black and white RGBA32F targets', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /original_desc\.SampleDesc\.Count\s*!=\s*1/)
  assert.match(source, /original_desc\.SampleDesc\.Count\s*!=\s*1/)
  assert.match(source, /depth_desc\.SampleDesc\.Count\s*!=\s*1/)
  assert.match(source, /capture_desc\.Format\s*=\s*DXGI_FORMAT_R32G32B32A32_FLOAT/)
  assert.match(source, /capture_desc\.SampleDesc\.Count\s*=\s*1/)
  assert.match(source, /D3D11_BIND_RENDER_TARGET/)
  assert.match(source, /D3D11_RENDER_TARGET_VIEW_DESC/)
  assert.match(source, /ClearRenderTargetView\(g\.replay\.black_rtv\.Get\(\), black_clear\)/)
  assert.match(source, /ClearRenderTargetView\(g\.replay\.white_rtv\.Get\(\), white_clear\)/)
  assert.match(source, /OMSetRenderTargetsAndUnorderedAccessViews\(1, &replay_rtv, original_dsv/)
  assert.doesNotMatch(source, /depth_texture/)
  assert.doesNotMatch(source, /depth_dsv/)
  assert.doesNotMatch(source, /ClearDepthStencilView/)
})

test('auto-match replays newly learned scene draws during the capture frame', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /remember_current_render_target\(context\)/)
  assert.match(source, /current_render_target_is_learned\(context\)/)
  assert.match(source, /if \(g\.replay_capture_active\) \{[^]*query_color_replay_state\(context, mesh, additive\)/s)
  assert.match(source, /target RT is not a single-sample Texture2D/)
  assert.doesNotMatch(source, /target RT is not single-sample R16G16B16A16_FLOAT Texture2D/)
})

test('configured color draws refresh a recreated scene target without requiring a gear change', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(callback, /const bool configured = is_configured_shader_rule\(/)
  assert.match(callback, /if \(configured\) \{[^]*query_mesh_signature\(context, arguments, configured_mesh\)/)
  assert.match(callback, /query_color_replay_state\(context, configured_mesh, configured_additive\)/)
  assert.match(callback, /remember_current_render_target\(context\)/)
  assert.match(callback, /if \(g\.replay_capture_active\)[^]*replay_color_draw\(/)
  assert.ok(callback.indexOf('if (configured)') < callback.indexOf('if (g.cfg.auto_match)'))
})

test('color replay preserves opaque occlusion with read-only depth and stencil', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /make_replay_depth_state/)
  assert.match(source, /DepthWriteMask\s*=\s*D3D11_DEPTH_WRITE_MASK_ZERO/)
  assert.match(source, /StencilWriteMask\s*=\s*0/)
  assert.match(source, /relax_equal && description\.DepthFunc == D3D11_COMPARISON_GREATER/)
  assert.match(source, /D3D11_COMPARISON_GREATER_EQUAL/)
  assert.match(source, /OMSetDepthStencilState\(replay_depth_state/)
})

test('capture mirrors ordinary indexed scene draws into both real backgrounds', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const mirror = source.match(/bool mirror_scene_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /learned_scene_targets/)
  assert.match(source, /current_render_target_is_learned/)
  assert.match(source, /current_render_target_resource_id/)
  assert.match(mirror, /g\.replay\.black_rtv/)
  assert.match(mirror, /g\.replay\.white_rtv/)
  assert.equal((mirror.match(/DrawIndexedInstanced\(/g) ?? []).length, 2)
  assert.match(mirror, /OMSetBlendState\(original_blend/)
  assert.match(mirror, /replay_depth_state, false/)
})

test('capture diagnoses non-indexed scene draws and mirrors scene RT clears', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /bool on_draw\(/)
  assert.match(source, /replay_nonindexed_draw_count/)
  assert.match(source, /bool on_clear_render_target_view\(/)
  assert.match(source, /scene RT clear mirrored/)
  assert.match(source, /addon_event::draw>/)
  assert.match(source, /addon_event::clear_render_target_view>/)
})

test('non-indexed final highlight composite preserves its verified one/inv-src-alpha blend', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const rules = read('./shader_rules.hpp')
  const config = read('./NS_AlphaCapture.ini')

  assert.match(rules, /struct nonindexed_rule/)
  assert.match(rules, /nonindexed_rule_matches/)
  assert.match(source, /replay_nonindexed_composite_draw/)
  assert.match(source, /context->DrawInstanced\(vertex_count, instance_count, first_vertex, first_instance\)/)
  assert.match(source, /original_dsv != nullptr/)
  assert.match(source, /ensure_replay_resources\(context, original_rtvs\[0\], nullptr, error, true\)/)
  assert.match(source, /is_configured_nonindexed_rule/)
  assert.match(source, /dsv != nullptr/)
  assert.match(source, /blend\.SrcBlend != D3D11_BLEND_ONE/)
  assert.match(source, /D3D11_BLEND_INV_SRC_ALPHA/)
  const replay = source.match(/bool replay_nonindexed_composite_draw\([^]*?\n\}/)?.[0] ?? ''
  assert.match(replay, /OMSetBlendState\(original_blend, original_factor, original_sample_mask\)/)
  assert.doesNotMatch(replay, /OMSetBlendState\(g\.replay\.alpha_blend/)
  assert.match(config, /\[NonIndexedRules\]/)
  assert.match(config, /Rule0=1\|1956256419\|2589759975\|4\|1\|0\|0\|0\|0\|0\|1920\|1080\|1\|1\|2\|6\|7/)
  assert.match(config, /Rule1=1\|1956256419\|2589759975\|4\|1\|0\|0\|0\|0\|0\|3840\|2160\|1\|1\|2\|6\|7/)
  assert.doesNotMatch(config, /Rule4=1\|507037697\|2589759975/)
})

test('final highlight candidates are learned from draw state across arbitrary resolutions', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const config = read('./NS_AlphaCapture.ini')

  assert.match(config, /^AutoHighlight=1$/m)
  assert.match(source, /bool auto_highlight = true/)
  assert.match(source, /is_auto_highlight_shape\(/)
  assert.match(source, /record_nonindexed_candidate\(/)
  assert.match(source, /has_learned_nonindexed_candidate\(/)
  assert.match(source, /vertex_count != 3 && vertex_count != 4/)
  assert.match(source, /D3D11_BLEND_ONE/)
  assert.match(source, /D3D11_BLEND_INV_SRC_ALPHA/)
  assert.match(source, /RenderTargetWriteMask != 0x07/)
  assert.match(source, /dsv != nullptr/)
  assert.match(source, /candidate\.draw_count >= 2/)
  assert.match(source, /if \(!g\.replay_capture_active\)/)
  assert.match(source, /auto_highlight learned non-indexed candidate/)
})

test('confirmed final highlight refreshes the scene target without a resolution gate', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw\([^]*?\n\}/)?.[0] ?? ''
  const remember = source.match(/void remember_confirmed_scene_target\([^]*?\n\}/)?.[0] ?? ''
  const learnedStart = source.indexOf('bool current_render_target_is_learned(ID3D11DeviceContext *context) {')
  const learnedEnd = source.indexOf('\n}', learnedStart)
  const learned = source.slice(learnedStart, learnedEnd + 2)

  assert.match(callback, /const bool auto_learned = auto_shape && has_learned_nonindexed_candidate/)
  assert.ok(callback.indexOf('remember_confirmed_scene_target(') <
    callback.indexOf('if (!g.replay_capture_active)'))
  assert.match(remember, /g\.confirmed_scene_targets/)
  assert.match(remember, /constexpr size_t max_recent_targets = 4/)
  assert.match(remember, /scene target refreshed from confirmed final highlight/)
  assert.doesNotMatch(remember, /Width|Height|get_screenshot_width_and_height/)
  assert.match(learned, /g\.confirmed_scene_targets/)
})

test('later composites reuse the captured scene across render-target size changes', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_nonindexed_composite_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(replay, /const bool reuse_captured_scene = g\.replay_frame_started/)
  assert.match(replay, /g\.replay\.width != target_desc\.Width/)
  assert.match(replay, /g\.replay\.height != target_desc\.Height/)
  assert.match(replay, /reusing captured scene=%ux%u without reset/)
  assert.match(replay, /\(!reuse_captured_scene &&\s*!ensure_replay_resources\(/)
  assert.match(replay, /if \(!reuse_captured_scene\)\s*initialize_replay_targets\(/)
  assert.match(replay, /context->DrawInstanced\(vertex_count, instance_count, first_vertex, first_instance\)/)
})

test('ordinary and additive replay blends preserve continuous alpha semantics', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /SrcBlend\s*=\s*D3D11_BLEND_SRC_ALPHA/)
  assert.match(source, /DestBlend\s*=\s*D3D11_BLEND_INV_SRC_ALPHA/)
  assert.match(source, /SrcBlendAlpha\s*=\s*D3D11_BLEND_ONE/)
  assert.match(source, /DestBlendAlpha\s*=\s*D3D11_BLEND_INV_SRC_ALPHA/)
  assert.match(source, /SrcBlend\s*=\s*D3D11_BLEND_ONE/)
  assert.match(source, /DestBlend\s*=\s*D3D11_BLEND_ONE/)
  assert.match(source, /SrcBlendAlpha\s*=\s*D3D11_BLEND_ZERO/)
  assert.match(source, /DestBlendAlpha\s*=\s*D3D11_BLEND_ONE/)
  assert.match(source, /RenderTargetWriteMask\s*=\s*D3D11_COLOR_WRITE_ENABLE_ALL/)
})

test('replay restores OM state and leaves non-target game draws untouched', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(replay, /OMGetRenderTargets\(/)
  assert.match(replay, /OMGetBlendState\(/)
  assert.match(replay, /OMSetRenderTargetsAndUnorderedAccessViews\(1, &replay_rtv, original_dsv/)
  assert.match(replay, /D3D11_KEEP_UNORDERED_ACCESS_VIEWS/)
  assert.match(replay, /DrawIndexedInstanced\(/)
  assert.match(replay, /g_replay_depth/)
  assert.match(replay, /OMSetDepthStencilState\(replay_depth_state/)
  assert.match(replay, /OMSetBlendState\(original_blend/)
  assert.match(replay, /OMSetRenderTargetsAndUnorderedAccessViews\(restore_count/)
  assert.doesNotMatch(source, /bool replay_depth_draw\(/)
  assert.match(callback, /return false/)
})

test('capture frame replaces the original transparent draw with the corrected continuous draw', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(replay, /OMSetRenderTargetsAndUnorderedAccessViews\(restore_count,[^]*?original_rtvs\.data\(\), original_dsv/)
  assert.match(replay, /OMSetBlendState\(replay_blend/)
  assert.match(replay, /OMSetDepthStencilState\(replay_depth_state/)
  assert.equal((replay.match(/DrawIndexedInstanced\(/g) ?? []).length, 3)
  assert.match(replay, /return true/)
  assert.match(callback, /return replay_color_draw\(/)
})

test('RGBA32F readback stays internal and only selected PNG images are exported', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replayExport = source.match(/bool capture_replay_outputs\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /D3D11_USAGE_STAGING/)
  assert.match(source, /D3D11_CPU_ACCESS_READ/)
  assert.match(source, /D3D11_MAP_READ/)
  assert.match(source, /mapped\.RowPitch/)
  assert.match(source, /reconstruct_black_white_rgba/)
  assert.match(source, /white\.pixels\[index \+ 0\] - black\.pixels\[index \+ 0\]/)
  assert.doesNotMatch(replayExport, /_black_rgba32f\.bin|_white_rgba32f\.bin|_rgba\.png|_alpha\.png|_lens_/)
})

test('PNG export uses WIC BGRA without replacing the captured alpha byte', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const build = read('./build.cmd')

  assert.match(source, /GUID_ContainerFormatPng/)
  assert.match(source, /GUID_WICPixelFormat32bppBGRA/)
  assert.match(source, /rgba_to_bgra/)
  assert.match(source, /bgra\[offset \+ 3\]\s*=\s*rgba\[offset \+ 3\]/)
  assert.match(source, /WritePixels\s*\(/)
  assert.doesNotMatch(source, /ClearAlpha|alpha\s*=\s*255/)
  assert.match(build, /windowscodecs\.lib/)
  assert.match(build, /ole32\.lib/)
})

test('WIC failures report the exact encoder stage and HRESULT', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /wic_error\s*\(/)
  assert.match(source, /0x%08X/)
  for (const stage of ['SetPixelFormat', 'WritePixels', 'FrameCommit', 'EncoderCommit'])
    assert.match(source, new RegExp(`"${stage}"`))
})

test('build emits the installed addon64 filename', () => {
  const build = read('./build.cmd')

  assert.match(build, /\/OUT:NS_AlphaCapture\.addon64(?:\s|$)/)
  assert.doesNotMatch(build, /NS_AlphaCapture\.addon64\.dll/)
})
