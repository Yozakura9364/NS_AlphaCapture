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
  assert.doesNotMatch(replayExport, /_rgba\.png|_alpha\.png|_rgba32f\.bin|L"_lens_[^"]*\.png/)
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
  assert.match(source, /preview_toggle_isolation\(g\.preview, current,\s*g\.replay_capture_active\)/)
  assert.match(rules, /preview_exit/)
  assert.match(rules, /preview = preview_state\{\}/)
  assert.match(rules, /if \(capture_active \|\| pixel == 0\)\s*return false/)
})

test('shader hunting collects a complete frame after the overlay arms scanning', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const presentCallback = source.match(/void on_reshade_present\([^]*?\n\}/)?.[0] ?? ''
  const nonindexedCallback = source.match(/bool on_draw\([^]*?\n\}/)?.[0] ?? ''
  const indexedRecorder = source.match(/void record_shader_candidate\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /bool shader_selector_skip_present = false/)
  assert.match(source, /g\.shader_selector_active = true;\s*g\.shader_selector_skip_present = true;/)
  assert.match(presentCallback,
    /if \(g\.shader_selector_skip_present\)\s*g\.shader_selector_skip_present = false;\s*else if \(!g\.shader_candidates\.empty\(\) \|\|\s*!g\.hunting_nonindexed_candidates\.empty\(\)\) \{/)
  assert.match(presentCallback,
    /else if \(!g\.shader_candidates\.empty\(\) \|\|[^]*?g\.shader_selector_active = false;[^]*?scan complete/)
  assert.doesNotMatch(indexedRecorder, /if \(render_target == 0\)\s*return;/)
  assert.match(indexedRecorder, /if \(hashes\.pixel == 0 && hashes\.vertex == 0\)\s*return;/)
  assert.match(nonindexedCallback,
    /if \(g\.shader_selector_active\)\s*record_hunting_nonindexed_candidate\(/)
})

test('transient isolation combines selected indexed and non-indexed shaders during capture', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const rules = read('./shader_rules.hpp')
  const indexedCallback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''
  const nonindexedCallback = source.match(/bool on_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(rules, /std::vector<uint32_t> isolation_pixels/)
  assert.match(rules, /preview_toggle_isolation/)
  assert.match(rules, /std::find\(preview\.isolation_pixels\.begin\(\), preview\.isolation_pixels\.end\(\), pixel\)/)
  assert.match(rules, /preview\.isolation_pixels\.empty\(\)/)
  assert.match(indexedCallback, /g\.preview\.active &&\s*ns_alpha_rules::preview_hides_draw\(/)
  assert.doesNotMatch(indexedCallback, /!g\.replay_capture_active && g\.preview\.active/)
  assert.ok(indexedCallback.indexOf('preview_hides_draw(') <
    indexedCallback.indexOf('if (g.replay_capture_active && g.cfg.lens_capture)'))
  assert.match(nonindexedCallback, /preview_hides_nonindexed_draw\(g\.preview, hashes\.pixel\)/)
  assert.doesNotMatch(nonindexedCallback,
    /!g\.replay_capture_active &&\s*ns_alpha_rules::preview_hides_nonindexed_draw/)
  assert.ok(nonindexedCallback.indexOf('preview_hides_nonindexed_draw(') <
    nonindexedCallback.indexOf('if (!g.replay_capture_active)'))
  assert.match(source, /preview_toggle_isolation\(g\.preview, current,\s*g\.replay_capture_active\)/)
  assert.match(source, /preview_toggle_isolation\(g\.preview, candidate\.pixel,\s*g\.replay_capture_active\)/)
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

test('lens scene-color substitution accepts half-resolution and sRGB inputs', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const lens = source.match(/bool lens_scene_srv_matches\([^]*?\n\}/)?.[0] ?? ''
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /DXGI_FORMAT_R8G8B8A8_UNORM_SRGB/)
  assert.match(source, /DXGI_FORMAT_B8G8R8A8_UNORM_SRGB/)
  assert.match(lens, /allow_half_resolution/)
  assert.match(lens, /description\.Width \* 2 == width/)
  assert.match(lens, /description\.Height \* 2 == height/)
  assert.match(replay, /bind_lens_scene_srvs\(context, g\.replay\.scene_black_srv\.Get\(\), lens_scene_srvs,[^;]*true, true\)/s)
  assert.match(replay, /bind_lens_scene_srvs\(context, g\.replay\.scene_white_srv\.Get\(\), lens_scene_srvs,[^;]*false, true\)/s)
})

test('lens replay substitutes only the confirmed scene-color SRV slot', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const bind = source.match(/void bind_lens_scene_srvs\([^]*?\n\}/)?.[0] ?? ''
  const query = source.match(/UINT query_lens_scene_srv_count\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /constexpr UINT lens_scene_color_slot = 2/)
  assert.match(bind, /slot = lens_scene_color_slot/)
  assert.doesNotMatch(bind, /slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT/)
  assert.match(query, /views\[lens_scene_color_slot\]/)
})

test('lens replay extracts gradient color over neutral support and restores it after alpha reconstruction', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''
  const exportPath = source.match(/bool capture_replay_outputs\([^]*?\n\}/)?.[0] ?? ''
  const override = source.match(/void apply_lens_gradient_override\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /lens_gradient_texture/)
  assert.match(source, /lens_gradient_rtv/)
  assert.match(replay, /gradient_rtv = g\.replay\.lens_gradient_rtv\.Get\(\)/)
  assert.match(replay, /g\.replay\.scene_white_srv\.Get\(\)/)
  assert.match(source, /apply_lens_gradient_override\(/)
  assert.match(exportPath, /apply_lens_gradient_override\(rgba, gradient_raw, support_raw, underlay_ptr\)/)
  assert.match(override, /scene saved immediately before the lens draw/)
  assert.match(override, /gradient\.pixels\[index \+ channel\]/)
  assert.match(override, /support\.pixels\[index \+ channel\].*\(1\.0f - lens_alpha\)/s)
  assert.match(override, /base_premul/)
  assert.match(source, /lens gradient underlay restored from pre-lens scene/)
})

test('lens gradient replay canvas is cleared once per frame, not once per draw', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const initialize = source.match(/void initialize_replay_targets\([^]*?\n\}/)?.[0] ?? ''
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(initialize, /ClearRenderTargetView\(g\.replay\.lens_gradient_rtv\.Get\(\)/)
  assert.match(initialize, /ClearRenderTargetView\(g\.replay\.lens_support_rtv\.Get\(\)/)
  assert.doesNotMatch(replay, /ClearRenderTargetView\(g\.replay\.lens_gradient_rtv\.Get\(/)
  assert.doesNotMatch(replay, /ClearRenderTargetView\(g\.replay\.lens_support_rtv\.Get\(/)
})

test('ordinary capture isolates the confirmed lens draw without enabling lens-only mode', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''
  const replay = source.match(/bool replay_color_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(callback, /g\.replay_capture_active && g\.cfg\.lens_capture/)
  assert.match(callback, /is_lens_target\([^]*?query_lens_replay_state/)
  assert.match(callback, /is_runtime_lens_target\([^]*?query_lens_replay_state/)
  assert.match(source, /UINT query_lens_scene_srv_count\(/)
  assert.match(callback, /replay_color_draw\([^]*?hashes\.pixel, true\)/)
  assert.match(replay, /if \(lens_only\)\s*\{[^}]*copy_scene_color_substitutes\(context\)/)
  assert.ok(
    replay.indexOf('copy_scene_color_substitutes(context)') <
      replay.indexOf('bind_lens_scene_srvs(context, g.replay.scene_black_srv.Get()'),
  )
})

test('configured gradient lens rules take precedence over runtime lens fallback', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''
  const configuredIndex = callback.indexOf('const bool configured = is_configured_shader_rule')
  const lensIndex = callback.indexOf('is_runtime_lens_target')

  assert.ok(configuredIndex >= 0, 'configured rule check must remain in indexed capture')
  assert.ok(lensIndex >= 0, 'runtime lens fallback must remain in indexed capture')
  assert.ok(configuredIndex < lensIndex,
    'configured gradient rules must be evaluated before runtime lens fallback')
  assert.match(callback, /g\.cfg\.lens_capture && !configured/)
})

test('configured gradient lens rules use lens replay while ordinary configured rules stay normal', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''
  const configuredLensStart = callback.indexOf('const bool configured_lens = configured &&')
  const ordinaryConfiguredStart = callback.indexOf('bool configured_additive = false')
  const configuredLensBlock = callback.slice(configuredLensStart, ordinaryConfiguredStart)

  assert.match(callback, /const bool configured_lens = configured &&/)
  assert.match(callback, /is_lens_target\(hashes, configured_mesh\)/)
  assert.match(callback, /query_lens_replay_state\(context, configured_mesh\)/)
  assert.match(configuredLensBlock, /replay_color_draw\([^;]*hashes\.pixel, true\)/s)
  assert.match(callback, /if \(g\.replay_capture_active && g\.cfg\.lens_capture && !configured\)/)
})

test('lens capture records runtime branch diagnostics for the confirmed draw', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /lens candidate observed ps=/)
  assert.match(source, /lens replay state format=/)
  assert.match(source, /lens_diagnostic_logged/)
})

test('lens diagnostics include a bounded indexed-draw probe for changed mesh signatures', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /capture indexed probe ps=/)
  assert.match(source, /lens_probe_draw_count < 128/)
})

test('lens diagnostics separately probe indexed draws that write color', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /capture color probe ps=/)
  assert.match(source, /lens_color_probe_draw_count < 64/)
  assert.match(source, /RenderTargetWriteMask/)
  assert.match(source, /query_probe_scene_srv_slots/)
  assert.match(source, /srv=%s/)
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
  assert.doesNotMatch(replayExport, /capture_lens_effect_outputs|L"_lens_[^"]*\.png/)
  assert.match(presentCallback, /capture_replay_outputs\(runtime\)/)
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

test('Escape clears the shortcut while recording instead of only cancelling capture', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const capture = source.match(/void process_hotkey_capture\(effect_runtime \*runtime\)\s*\{[^]*?\n\}/)?.[0] ?? ''

  assert.match(capture, /ImGui::IsKeyPressed\(ImGuiKey_Escape, false\)/)
  assert.match(capture, /finish_hotkey_capture\(0, ns_white_backing::modifier_none\)/)
  assert.match(capture, /g\.hotkey_suppress_key = VK_ESCAPE/)
  assert.doesNotMatch(capture, /if \(ImGui::IsKeyPressed\(ImGuiKey_Escape, false\)\) \{\s*g\.hotkey_capture_target = 0;/)
})

test('shortcut recording stages a key and requires explicit confirmation or cancellation', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const capture = source.match(/void process_hotkey_capture\(effect_runtime \*runtime\)\s*\{[^]*?\n\}/)?.[0] ?? ''
  const status = source.match(/void draw_hotkey_capture_status\(\)\s*\{[^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /uint32_t hotkey_pending_key = 0/)
  assert.match(capture, /stage_hotkey_capture\(virtual_key,/)
  assert.doesNotMatch(capture, /finish_hotkey_capture\(virtual_key,/)
  assert.match(status, /Detected shortcut/)
  assert.match(status, /Confirm and save/)
  assert.match(status, /Cancel/)
  assert.match(status, /finish_hotkey_capture\(key, modifiers\)/)
  assert.match(status, /cancel_hotkey_capture\(\)/)
  assert.match(source, /Esc clears the shortcut/)
})

test('shortcut settings always show that Escape clears the selected shortcut', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const settings = source.slice(
    source.indexOf('void on_settings_overlay'),
    source.indexOf('draw_group_list_settings();', source.indexOf('void on_settings_overlay')),
  )
  const hint = 'text("Press ESC to clear", "按 ESC 清空")'
  const pathControl = 'ImGui::TextUnformatted(text("Screenshot path", "截图路径"))'

  assert.match(settings, /text\("Press ESC to clear", "按 ESC 清空"\)/)
  assert.ok(settings.indexOf(hint) > settings.indexOf('Reload shortcut'))
  assert.ok(settings.indexOf(hint) < settings.indexOf(pathControl))
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
  assert.match(readme, /NS Alpha Capture/)
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

test('later composites reuse the captured scene on the same render-target resource', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_nonindexed_composite_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(replay, /const bool reuse_captured_scene = g\.replay_frame_started/)
  assert.match(replay, /original_resource_id = render_target_resource_id/)
  assert.match(replay, /replay_frame_target_resource == original_resource_id/)
  assert.match(replay, /\(!reuse_captured_scene &&\s*!ensure_replay_resources\(/)
  assert.match(replay, /if \(!reuse_captured_scene\)\s*initialize_replay_targets\(/)
  assert.match(replay, /context->DrawInstanced\(vertex_count, instance_count, first_vertex, first_instance\)/)
})

test('non-indexed composites restart replay when the bound scene resource is recreated', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const replay = source.match(/bool replay_nonindexed_composite_draw\([^]*?\n\}/)?.[0] ?? ''

  assert.match(replay, /const uint64_t original_resource_id = render_target_resource_id\(original_rtvs\[0\]\)/)
  assert.match(replay, /const bool reuse_captured_scene = g\.replay_frame_started &&\s*\n\s*g\.replay_frame_target_resource == original_resource_id/)
  assert.match(replay, /replay_frame_target_resource/)
  assert.match(replay, /resolution\/render-target change detected.*resetting replay scene/s)
  assert.match(replay, /initialize_replay_targets\(context, original_resource_id\)/)
})

test('auto-match learns a recreated scene target when a known subject mesh returns', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(callback, /query_mesh_signature\(context, arguments, mesh\) && learned_mesh\(mesh\)/)
  assert.match(callback, /if \(query_color_replay_state\(context, mesh, additive\)\) \{[^]*remember_current_render_target\(context\);[^]*replay_color_draw/s)
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
  assert.equal((replay.match(/DrawIndexedInstanced\(/g) ?? []).length, 4)
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
  assert.doesNotMatch(replayExport, /_black_rgba32f\.bin|_white_rgba32f\.bin|_rgba\.png|_alpha\.png|L"_lens_[^"]*\.png/)
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

test('release packaging always includes project and third-party licenses', () => {
  const publish = read('./scripts/publish-release.ps1')

  assert.match(publish, /LICENSE\.txt/)
  assert.match(publish, /THIRD_PARTY_NOTICES\.txt/)
  assert.match(publish, /Relative = 'LICENSE\.txt'/)
  assert.match(publish, /Relative = 'THIRD_PARTY_NOTICES\.txt'/)
  assert.match(publish, /Name = 'LICENSE\.txt'/)
  assert.match(publish, /Name = 'THIRD_PARTY_NOTICES\.txt'/)
  assert.match(publish, /'LICENSE\.txt',\s*'THIRD_PARTY_NOTICES\.txt'/)
})

test('addon capture disables enabled_in_screenshot=false techniques (e.g. VerticalPreviewer)', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.match(source, /screenshot_hidden_techniques/)
  assert.match(source, /enumerate_techniques\(nullptr/)
  assert.match(source, /get_annotation_bool_from_technique\(technique,\s*"enabled_in_screenshot"/s)
  assert.match(source, /enabled_in_screenshot \|\| !owner->get_technique_state/)
  assert.match(source, /set_technique_state\(technique, false\)/)
  assert.match(source, /restore_screenshot_hidden_techniques\(runtime\)/)
  assert.match(source, /set_technique_state\(technique, true\)/)
})

test('screenshot-excluded techniques are toggled in present (non-render timing), never inside effects callbacks', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const begin = source.match(/void on_reshade_begin_effects\([^]*?\n\}/)?.[0] ?? ''
  const finish = source.match(/void on_reshade_finish_effects\([^]*?\n\}/)?.[0] ?? ''

  // 禁用发生在按下捕获键（present，非渲染时机）
  assert.match(source, /capture_pressed\) \{[^]*?hide_screenshot_excluded_techniques\(runtime\)/s)
  // 捕获完成后恢复
  assert.match(source, /capture_replay_outputs\(runtime\);[^]*?restore_screenshot_hidden_techniques\(runtime\)/s)
  // effects 渲染回调里绝不做 technique 切换（避免管线重入死锁）
  assert.doesNotMatch(begin, /set_technique_state|enumerate_techniques/)
  assert.doesNotMatch(finish, /set_technique_state|enumerate_techniques/)
  assert.match(finish, /capture_game_image\(runtime, "finish_effects"\)/)
  assert.match(source, /g\.screenshot_hidden_techniques\.clear\(\);/)
})

test('capture does not post-process the final image to erase shader artifacts', () => {
  const source = read('./NS_AlphaCapture.cpp')

  assert.doesNotMatch(source, /detect_(?:grid|guide|line)|erase_(?:grid|guide|line)|remove_(?:grid|guide|line)/i)
  assert.doesNotMatch(source, /VerticalPreviewer|Vertical_Previewer/)
})

test('game image capture falls back when ReShade skips effects callbacks', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const present = source.match(/void on_reshade_present\([^]*?\n\}/)?.[0] ?? ''
  const overlay = source.match(/void on_reshade_overlay\([^]*?\n\}/)?.[0] ?? ''
  const finish = source.match(/void on_reshade_finish_effects\([^]*?\n\}/)?.[0] ?? ''

  assert.match(source, /bool capture_game_image\(effect_runtime \*runtime, const char \*stage\)/)
  assert.match(finish, /capture_game_image\(runtime, "finish_effects"\)/)
  assert.match(overlay, /if \(g\.replay_capture_active && !g\.game_image_valid\)\s*capture_game_image\(runtime, "overlay_fallback"\)/s)
  assert.match(present, /capture_game_image\(runtime, "present_fallback"\)/)
  assert.match(present, /last clean point before external overlay addons/)
  assert.match(overlay, /final fallback/)
  assert.match(source, /g\.game_image_valid = false;/)
})

test('game image export is opaque like a regular ReShade screenshot', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const capture = source.match(/bool capture_game_image\([^]*?\n\}/)?.[0] ?? ''

  assert.match(capture, /for \(size_t index = 3; index < g\.game_image\.pixels\.size\(\); index \+= 4\)/)
  assert.match(capture, /g\.game_image\.pixels\[index\] = 255/)
})

test('configured water pixel shader is skipped from capture replay only', () => {
  const source = read('./NS_AlphaCapture.cpp')
  const config = read('./NS_AlphaCapture.ini')
  const callback = source.match(/bool on_draw_indexed\([^]*?\n\}/)?.[0] ?? ''

  assert.match(config, /^CaptureExcludePixelShaderHash=0xD24A3428$/m)
  assert.match(source, /CaptureExcludePixelShaderHash/)
  assert.match(callback, /g\.replay_capture_active && g\.cfg\.capture_exclude_pixel_shader_hash != 0/)
  assert.match(callback, /hashes\.pixel == g\.cfg\.capture_exclude_pixel_shader_hash/)
  assert.match(callback, /capture excluded pixel shader/)
  assert.match(callback, /return false/)
})
