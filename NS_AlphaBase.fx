// FF14 VFX capture helper: prepare transparent render targets before selected
// translucent draws and provide a white support surface for the lens shader.

#include "ReShade.fxh"

uniform float NS_REST_Threshold
<
	ui_label = "Depth threshold";
	ui_tooltip = "Use the same threshold as the working Chromakey setup.";
	ui_type = "slider";
	ui_min = 0.0;
	ui_max = 0.999;
	ui_step = 0.001;
> = 0.135;

uniform bool NS_REST_InvertDepth
<
	ui_label = "Invert depth";
> = false;

uniform bool NS_REST_AntiAliasMask
<
	ui_label = "Anti-alias depth mask";
	ui_tooltip = "Leave disabled when matching the original FF14 capture preset.";
> = false;

uniform int NS_REST_AlphaMode
<
	ui_label = "Prepared alpha";
	ui_tooltip = "Depth alpha gives the selected translucent draws a transparent background. REST must not preserve the target alpha channel for this group.";
	ui_type = "combo";
	ui_items = "Keep existing alpha\0Combined game + depth alpha\0Depth alpha only\0";
> = 2;

uniform float3 NS_LENS_SupportColor
<
	ui_label = "Lens support color";
	ui_tooltip = "White RGB placed behind the isolated lens draw so its tint can be reconstructed over the saved scene.";
	ui_type = "color";
> = float3(1.0, 1.0, 1.0);

uniform float NS_LENS_DifferenceThreshold
<
	ui_label = "Lens difference threshold";
	ui_tooltip = "Rejects unchanged support pixels after the lens draw.";
	ui_type = "slider";
	ui_min = 0.0;
	ui_max = 0.05;
	ui_step = 0.001;
> = 0.003;

uniform float NS_LENS_DifferenceGain
<
	ui_label = "Lens alpha gain";
	ui_tooltip = "Converts the lens color difference into output opacity.";
	ui_type = "slider";
	ui_min = 1.0;
	ui_max = 10.0;
	ui_step = 0.1;
> = 1.0;

uniform float NS_LENS_MinAlpha
<
	ui_label = "Lens minimum alpha";
	ui_tooltip = "Minimum opacity assigned to pixels that differ from the neutral support.";
	ui_type = "slider";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.01;
> = 0.15;

uniform uint NS_LENS_FrameCount < source = "framecount"; >;

texture NS_LENS_Underlay
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = RGBA16F;
};

texture NS_LENS_UnderlayFrame
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = RGBA8;
};

sampler NS_LENS_UnderlaySampler
{
	Texture = NS_LENS_Underlay;
	MinFilter = POINT;
	MagFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

sampler NS_LENS_UnderlayFrameSampler
{
	Texture = NS_LENS_UnderlayFrame;
	MinFilter = POINT;
	MagFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

texture NS_LENS_Composite
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = RGBA16F;
};

sampler NS_LENS_CompositeSampler
{
	Texture = NS_LENS_Composite;
	MinFilter = POINT;
	MagFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

float NS_REST_BackgroundMask(float2 texcoord)
{
	float depth = ReShade::GetLinearizedDepth(texcoord);
	if (NS_REST_InvertDepth)
		depth = 1.0 - depth;

	if (!NS_REST_AntiAliasMask)
		return step(NS_REST_Threshold, depth);

	const float halfPixel = max(fwidth(depth) * 0.5, 0.000001);
	return smoothstep(
		NS_REST_Threshold - halfPixel,
		NS_REST_Threshold + halfPixel,
		depth
	);
}

float4 NS_REST_AlphaBasePS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	const float4 source = tex2D(ReShade::BackBuffer, texcoord);
	const float background = NS_REST_BackgroundMask(texcoord);
	const float foreground = 1.0 - background;

	float alpha = source.a;
	if (NS_REST_AlphaMode == 1)
		alpha = max(source.a, foreground);
	else if (NS_REST_AlphaMode == 2)
		alpha = foreground;

	return float4(lerp(source.rgb, 0.0, background), saturate(alpha));
}

void NS_REST_LensSnapshotPS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD,
	out float4 underlay : SV_Target0,
	out float4 frameStamp : SV_Target1
)
{
	underlay = tex2D(ReShade::BackBuffer, texcoord);
	frameStamp = float4((NS_LENS_FrameCount % 251u) / 250.0, 0.0, 0.0, 1.0);
}

float NS_REST_LensFrameIsFresh(float2 texcoord)
{
	const float expected = (NS_LENS_FrameCount % 251u) / 250.0;
	const float stored = tex2D(NS_LENS_UnderlayFrameSampler, texcoord).r;
	return 1.0 - step(0.006, abs(expected - stored));
}

float4 NS_REST_LensBasePS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	return float4(NS_LENS_SupportColor, 0.0);
}

float4 NS_REST_IsolateLens(float2 texcoord)
{
	const float4 source = tex2D(ReShade::BackBuffer, texcoord);
	const float3 difference = abs(source.rgb - NS_LENS_SupportColor);
	const float differenceMax = max(difference.r, max(difference.g, difference.b));
	const float detectionSignal = max(differenceMax, source.a);
	const float detectedLens = smoothstep(NS_LENS_DifferenceThreshold, NS_LENS_DifferenceThreshold * 4.0 + 0.000001, detectionSignal);
	const float3 supportDeficit = saturate((NS_LENS_SupportColor - source.rgb) / max(NS_LENS_SupportColor, 0.000001));
	const float requiredAlpha = max(supportDeficit.r, max(supportDeficit.g, supportDeficit.b));
	const float fallbackAlpha = max(requiredAlpha, saturate(differenceMax * NS_LENS_DifferenceGain));
	const float hasGameAlpha = step(NS_LENS_DifferenceThreshold, source.a);
	const float estimatedAlpha = lerp(fallbackAlpha, source.a, hasGameAlpha);
	const float lensAlpha = detectedLens * max(NS_LENS_MinAlpha, estimatedAlpha);
	const float3 isolatedLensRGB = saturate(source.rgb - NS_LENS_SupportColor * (1.0 - lensAlpha));
	return float4(isolatedLensRGB, lensAlpha);
}

float4 NS_REST_ResolveLens4x4(float2 texcoord)
{
	const float2 pixel = BUFFER_PIXEL_SIZE;
	const float4 center = NS_REST_IsolateLens(texcoord);
	float4 resolvedLens = 0.0;
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-1.5, -1.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-0.5, -1.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 0.5, -1.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 1.5, -1.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-1.5, -0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-0.5, -0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 0.5, -0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 1.5, -0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-1.5,  0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-0.5,  0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 0.5,  0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 1.5,  0.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-1.5,  1.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2(-0.5,  1.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 0.5,  1.5));
	resolvedLens += NS_REST_IsolateLens(texcoord + pixel * float2( 1.5,  1.5));
	resolvedLens *= 0.0625;

	// A neighborhood resolve is useful on the lens itself, but an un-gated
	// average dilates the lens into the scene behind it. Keep the resolve
	// center-pixel gated so background pixels cannot inherit lens alpha.
	const float centerGate = smoothstep(0.05, 0.20, center.a);
	return lerp(center, resolvedLens, centerGate * 0.65);
}

float4 NS_REST_LensCapturePS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	const float4 resolvedLens = NS_REST_ResolveLens4x4(texcoord);
	const float freshUnderlay = NS_REST_LensFrameIsFresh(texcoord);
	if (freshUnderlay < 0.5)
		return tex2D(ReShade::BackBuffer, texcoord);

	const float4 underlay = tex2D(NS_LENS_UnderlaySampler, texcoord);
	const float4 restoredLens = float4(
		resolvedLens.rgb + underlay.rgb * (1.0 - resolvedLens.a),
		resolvedLens.a + underlay.a * (1.0 - resolvedLens.a)
	);
	return restoredLens;
}

float4 NS_REST_LensCompositePS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	return tex2D(NS_LENS_CompositeSampler, texcoord);
}

technique NS_AlphaBase
<
	ui_label = "NS_AlphaBase";
	ui_tooltip = "Assign only this technique to the REST Alpha group before the four translucent pixel shaders.";
>
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_REST_AlphaBasePS;
	}
}

technique NS_REST_LensBase
<
	ui_label = "NS REST Lens Base";
	ui_tooltip = "REST BEFORE DRAW: snapshot the current alpha scene, then provide white RGB behind the isolated lens draw.";
>
{
	pass SnapshotLensUnderlay
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_REST_LensSnapshotPS;
		RenderTarget0 = NS_LENS_Underlay;
		RenderTarget1 = NS_LENS_UnderlayFrame;
	}

	pass PrepareLensSupport
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_REST_LensBasePS;
	}
}

technique NS_REST_LensCapture
<
	ui_label = "NS REST Lens Capture";
	ui_tooltip = "REST AFTER DRAW: composite the isolated lens over the saved alpha scene and expose NS_LENS_Composite for direct export.";
>
{
	pass CaptureLensComposite
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_REST_LensCapturePS;
		RenderTarget = NS_LENS_Composite;
	}

	pass ApplyLensComposite
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_REST_LensCompositePS;
	}
}
