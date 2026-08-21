// FF14 VFX capture helper: snapshot the completed scene once, then expose
// synchronized black-background, white-background, repaired-alpha, straight-RGBA
// and unmodified game-alpha textures.

#include "ReShade.fxh"

uniform float NS_CAPTURE_Threshold
<
	ui_label = "Depth threshold";
	ui_tooltip = "Use the same threshold as NS_AlphaBase.";
	ui_type = "slider";
	ui_min = 0.0;
	ui_max = 0.999;
	ui_step = 0.001;
> = 0.135;

uniform bool NS_CAPTURE_InvertDepth
<
	ui_label = "Invert depth";
> = false;

uniform bool NS_CAPTURE_AntiAliasMask
<
	ui_label = "Anti-alias depth mask";
	ui_tooltip = "Leave disabled when matching the original FF14 capture preset.";
> = false;

uniform int NS_CAPTURE_AlphaMode
<
	ui_label = "Output alpha source";
	ui_tooltip = "Combined matches the old Keep alpha plus depth-backed subject workflow.";
	ui_type = "combo";
	ui_items = "Game alpha only\0Combined game + depth alpha\0Depth alpha only\0";
> = 1;

uniform int NS_CAPTURE_CoverageResolve
<
	ui_label = "Ordered transparency resolve";
	ui_tooltip = "Leave off for ordinary VFX. Use 2x2 or 4x4 only when translucent FF14 materials show a regular screen-door grid.";
	ui_type = "combo";
	ui_items = "Off\0Light 2x2\0Strong 4x4\0";
> = 2;

uniform float NS_CAPTURE_ResolveStrength
<
	ui_label = "Resolve strength";
	ui_tooltip = "Blends screen-door pixels toward the local coverage average.";
	ui_type = "slider";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.01;
> = 1.0;

uniform float NS_CAPTURE_ContourSmoothing
<
	ui_label = "Transparent contour smoothing";
	ui_tooltip = "Smooths only the resolved translucent silhouette. Opaque depth-backed edges are preserved.";
	ui_type = "slider";
	ui_min = 0.0;
	ui_max = 1.0;
	ui_step = 0.01;
> = 0.65;

uniform float NS_CAPTURE_TranslucentAlphaGain
<
	ui_label = "Translucent alpha gain";
	ui_tooltip = "Curves only depthless fractional alpha while preserving transparent and opaque endpoints.";
	ui_type = "slider";
	ui_min = 1.0;
	ui_max = 2.0;
	ui_step = 0.01;
> = 1.55;

uniform int NS_CAPTURE_PreviewMode
<
	ui_label = "Live transparent preview";
	ui_tooltip = "Shows the final exported RGBA over a selected background. Depth input and Background mask verify the Generic Depth buffer before trusting any alpha math. Capture output is unchanged.";
	ui_type = "combo";
	ui_items = "Off\0Checkerboard\0Black\0White\0Alpha\0Red\0Depth input\0Background mask\0";
> = 1;

uniform bool NS_CAPTURE_DiagnosticOutputs
<
	ui_label = "Capture raw alpha diagnostics";
	ui_tooltip = "Temporarily writes Raw Alpha, Coverage and Active Mean into the black, white and alpha PNG slots. RGBA stays final.";
> = false;

texture NS_CAPTURE_Prepared
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = RGBA16F;
};

sampler NS_CAPTURE_PreparedSampler
{
	Texture = NS_CAPTURE_Prepared;
	MinFilter = LINEAR;
	MagFilter = LINEAR;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

texture NS_VFX_Black : NS_ALPHA_CAPTURE_BLACK;

sampler NS_CAPTURE_BlackSampler
{
	Texture = NS_VFX_Black;
	MinFilter = POINT;
	MagFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

texture NS_VFX_White : NS_ALPHA_CAPTURE_WHITE;

sampler NS_CAPTURE_WhiteSampler
{
	Texture = NS_VFX_White;
	MinFilter = POINT;
	MagFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

texture NS_VFX_Alpha
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = RGBA8;
};

texture NS_VFX_RGBA
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = RGBA8;
};

sampler NS_CAPTURE_RGBA_sampler
{
	Texture = NS_VFX_RGBA;
	MinFilter = POINT;
	MagFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

texture NS_VFX_RawGameAlpha
{
	Width = BUFFER_WIDTH;
	Height = BUFFER_HEIGHT;
	Format = RGBA8;
};

sampler NS_CAPTURE_RawGameAlphaSampler
{
	Texture = NS_VFX_RawGameAlpha;
	MinFilter = POINT;
	MagFilter = POINT;
	MipFilter = POINT;
	AddressU = CLAMP;
	AddressV = CLAMP;
};

float NS_CAPTURE_ReadRawAlpha(float2 texcoord)
{
	return tex2D(NS_CAPTURE_RawGameAlphaSampler, texcoord).r;
}

float NS_CAPTURE_BackgroundMask(float2 texcoord)
{
	float depth = ReShade::GetLinearizedDepth(texcoord);
	if (NS_CAPTURE_InvertDepth)
		depth = 1.0 - depth;

	if (!NS_CAPTURE_AntiAliasMask)
		return step(NS_CAPTURE_Threshold, depth);

	const float halfPixel = max(fwidth(depth) * 0.5, 0.000001);
	return smoothstep(
		NS_CAPTURE_Threshold - halfPixel,
		NS_CAPTURE_Threshold + halfPixel,
		depth
	);
}

float NS_CAPTURE_SelectAlpha(float gameAlpha, float foreground)
{
	float alpha = gameAlpha;
	if (NS_CAPTURE_AlphaMode == 1)
	{
		const float hasUsableTranslucentAlpha =
			step(0.01, gameAlpha) * (1.0 - step(0.99, gameAlpha));
		alpha = lerp(max(gameAlpha, foreground), gameAlpha, hasUsableTranslucentAlpha);
	}
	else if (NS_CAPTURE_AlphaMode == 2)
		alpha = foreground;

	return saturate(alpha);
}

float4 NS_CAPTURE_ReadPrepared(float2 texcoord)
{
	const float4 source = tex2D(ReShade::BackBuffer, texcoord);
	const float foreground = 1.0 - NS_CAPTURE_BackgroundMask(texcoord);
	return float4(source.rgb, NS_CAPTURE_SelectAlpha(source.a, foreground));
}

float NS_CAPTURE_HasOpaqueDepth(float2 texcoord, float radius)
{
	const float2 offset = BUFFER_PIXEL_SIZE * radius;
	float foreground = 1.0 - NS_CAPTURE_BackgroundMask(texcoord);
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2(-1.0,  0.0)));
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2( 1.0,  0.0)));
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2( 0.0, -1.0)));
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2( 0.0,  1.0)));
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2(-1.0, -1.0)));
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2( 1.0, -1.0)));
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2(-1.0,  1.0)));
	foreground = max(foreground, 1.0 - NS_CAPTURE_BackgroundMask(texcoord + offset * float2( 1.0,  1.0)));
	return step(0.5, foreground);
}

void NS_CAPTURE_AnalyzeRawAlpha(float2 texcoord, out float coverage, out float activeMean)
{
	float activeCount = 0.0;
	float activeSum = 0.0;
	[unroll]
	for (int y = 0; y < 4; ++y)
	{
		[unroll]
		for (int x = 0; x < 4; ++x)
		{
			const float2 offset = float2(x - 1.5, y - 1.5) * BUFFER_PIXEL_SIZE;
			const float rawAlpha = NS_CAPTURE_ReadRawAlpha(texcoord + offset);
			const float active = step(0.01, rawAlpha);
			activeCount += active;
			activeSum += rawAlpha * active;
		}
	}

	coverage = activeCount * 0.0625;
	activeMean = activeCount > 0.0 ? activeSum / activeCount : 0.0;
}

float NS_CAPTURE_PeriodicSupport(float2 texcoord, float activeMean)
{
	const float2 pixel = BUFFER_PIXEL_SIZE;
	float phaseErrorX = 0.0;
	float phaseErrorY = 0.0;

	[unroll]
	for (int y = -1; y <= 2; ++y)
	{
		[unroll]
		for (int x = -1; x <= 2; ++x)
		{
			const float2 baseOffset = float2(x, y);
			const float alpha = NS_CAPTURE_ReadRawAlpha(texcoord + pixel * baseOffset);
			phaseErrorX += abs(alpha - NS_CAPTURE_ReadRawAlpha(
				texcoord + pixel * (baseOffset + float2(4.0, 0.0))));
			phaseErrorY += abs(alpha - NS_CAPTURE_ReadRawAlpha(
				texcoord + pixel * (baseOffset + float2(0.0, 4.0))));
		}
	}

	phaseErrorX *= 0.0625;
	phaseErrorY *= 0.0625;
	const float normalizedPhaseError =
		max(phaseErrorX, phaseErrorY) / max(activeMean, 0.0001);
	return 1.0 - smoothstep(0.01, 0.05, normalizedPhaseError);
}

float NS_CAPTURE_ResolveWeight(float2 texcoord, float radius, float coverage, float activeMean)
{
	const float mixedCoverage = smoothstep(0.01, 0.05, min(coverage, 1.0 - coverage));
	const float noOpaqueDepth = 1.0 - NS_CAPTURE_HasOpaqueDepth(texcoord, radius);
	float periodicSupport = 0.0;
	[branch]
	if (noOpaqueDepth < 0.5 && mixedCoverage > 0.0)
		periodicSupport = NS_CAPTURE_PeriodicSupport(texcoord, activeMean);

	// Every phase of a proven four-pixel pattern must use the same weight. A
	// center-local transition score makes sparse 1-3/16 and dense 13-15/16 Bayer
	// coverage resolve to several different alpha values.
	const float depthlessResolve = noOpaqueDepth;
	const float depthBackedResolve = (1.0 - noOpaqueDepth) * periodicSupport;
	const float safeResolveRegion = saturate(depthlessResolve + depthBackedResolve);
	return saturate(NS_CAPTURE_ResolveStrength * mixedCoverage * safeResolveRegion);
}

float4 NS_CAPTURE_Resolve2x2(float2 texcoord, float4 center)
{
	const float2 halfPixel = BUFFER_PIXEL_SIZE * 0.5;
	float4 mean = 0.0;
	mean += NS_CAPTURE_ReadPrepared(texcoord + halfPixel * float2(-1.0, -1.0));
	mean += NS_CAPTURE_ReadPrepared(texcoord + halfPixel * float2( 1.0, -1.0));
	mean += NS_CAPTURE_ReadPrepared(texcoord + halfPixel * float2(-1.0,  1.0));
	mean += NS_CAPTURE_ReadPrepared(texcoord + halfPixel * float2( 1.0,  1.0));
	mean *= 0.25;

	float coverage = 0.0;
	float activeMean = 0.0;
	NS_CAPTURE_AnalyzeRawAlpha(texcoord, coverage, activeMean);
	return lerp(center, mean, NS_CAPTURE_ResolveWeight(texcoord, 0.5, coverage, activeMean));
}

float4 NS_CAPTURE_Resolve4x4(float2 texcoord, float4 center)
{
	const float2 pixel = BUFFER_PIXEL_SIZE;
	float4 mean = 0.0;
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-1.5, -1.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-0.5, -1.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 0.5, -1.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 1.5, -1.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-1.5, -0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-0.5, -0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 0.5, -0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 1.5, -0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-1.5,  0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-0.5,  0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 0.5,  0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 1.5,  0.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-1.5,  1.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2(-0.5,  1.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 0.5,  1.5));
	mean += NS_CAPTURE_ReadPrepared(texcoord + pixel * float2( 1.5,  1.5));
	mean *= 0.0625;

	float coverage = 0.0;
	float activeMean = 0.0;
	NS_CAPTURE_AnalyzeRawAlpha(texcoord, coverage, activeMean);
	mean.a = coverage * activeMean;
	return lerp(center, mean, NS_CAPTURE_ResolveWeight(texcoord, 1.5, coverage, activeMean));
}

float4 NS_CAPTURE_PreparePS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	float4 prepared = NS_CAPTURE_ReadPrepared(texcoord);
	if (NS_CAPTURE_CoverageResolve == 1)
		prepared = NS_CAPTURE_Resolve2x2(texcoord, prepared);
	else if (NS_CAPTURE_CoverageResolve == 2)
		prepared = NS_CAPTURE_Resolve4x4(texcoord, prepared);

	return prepared;
}

float4 NS_CAPTURE_RawGameAlphaPS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	const float gameAlpha = saturate(tex2D(ReShade::BackBuffer, texcoord).a);
	return float4(gameAlpha, gameAlpha, gameAlpha, 1.0);
}

float4 NS_CAPTURE_MakeRepresentable(float4 value)
{
	// Straight-alpha export cannot represent a premultiplied RGB channel that
	// exceeds alpha. Raise alpha to the smallest physically valid value instead
	// of clipping RGB or producing white seams in the synthesized white capture.
	value.a = max(value.a, max(value.r, max(value.g, value.b)));
	return saturate(value);
}

float4 NS_CAPTURE_RefineTransparentContour(float2 texcoord, float4 center)
{
	center = NS_CAPTURE_MakeRepresentable(center);
	if (NS_CAPTURE_CoverageResolve == 0 || NS_CAPTURE_ContourSmoothing <= 0.0)
		return center;

	const float2 pixel = BUFFER_PIXEL_SIZE;
	const float4 northWest = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(-1.0, -1.0)));
	const float4 north = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(0.0, -1.0)));
	const float4 northEast = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(1.0, -1.0)));
	const float4 west = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(-1.0, 0.0)));
	const float4 east = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(1.0, 0.0)));
	const float4 southWest = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(-1.0, 1.0)));
	const float4 south = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(0.0, 1.0)));
	const float4 southEast = NS_CAPTURE_MakeRepresentable(tex2D(NS_CAPTURE_PreparedSampler, texcoord + pixel * float2(1.0, 1.0)));

	const float4 tent = (
		northWest + northEast + southWest + southEast +
		2.0 * (north + west + east + south) +
		4.0 * center
	) * 0.0625;

	float minAlpha = min(center.a, min(min(north.a, south.a), min(west.a, east.a)));
	minAlpha = min(minAlpha, min(min(northWest.a, northEast.a), min(southWest.a, southEast.a)));
	float maxAlpha = max(center.a, max(max(north.a, south.a), max(west.a, east.a)));
	maxAlpha = max(maxAlpha, max(max(northWest.a, northEast.a), max(southWest.a, southEast.a)));

	const float alphaRange = maxAlpha - minAlpha;
	const float translucentNeighborhood = smoothstep(0.005, 0.05, min(tent.a, 1.0 - tent.a));
	const float contour = smoothstep(0.01, 0.20, alphaRange) * translucentNeighborhood;
	const float noOpaqueDepth = 1.0 - NS_CAPTURE_HasOpaqueDepth(texcoord, 1.0);
	const float weight = saturate(NS_CAPTURE_ContourSmoothing * contour * noOpaqueDepth);

	return lerp(center, tent, weight);
}

float NS_CAPTURE_StrengthenTranslucentAlpha(float2 texcoord, float alpha)
{
	const float isTranslucent = step(0.01, alpha) * (1.0 - step(0.99, alpha));
	const float noOpaqueDepth = 1.0 - NS_CAPTURE_HasOpaqueDepth(texcoord, 1.0);
	const float curvedAlpha = alpha + (NS_CAPTURE_TranslucentAlphaGain - 1.0) * alpha * (1.0 - alpha);
	return saturate(lerp(alpha, curvedAlpha,
		isTranslucent * noOpaqueDepth));
}

void NS_CAPTURE_InitBackingPS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD,
	out float4 blackOutput : SV_Target0,
	out float4 whiteOutput : SV_Target1
)
{
	blackOutput = float4(0.0, 0.0, 0.0, 1.0);
	whiteOutput = float4(1.0, 1.0, 1.0, 1.0);
}

float4 NS_CAPTURE_CompositePreparedPS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	float4 prepared = tex2D(NS_CAPTURE_PreparedSampler, texcoord);
	prepared = NS_CAPTURE_RefineTransparentContour(texcoord, prepared);

	const float3 premultipliedRGB = saturate(prepared.rgb);
	float alpha = saturate(max(prepared.a,
		max(premultipliedRGB.r, max(premultipliedRGB.g, premultipliedRGB.b))));
	alpha = NS_CAPTURE_StrengthenTranslucentAlpha(texcoord, alpha);
	return float4(premultipliedRGB, alpha);
}

void NS_CAPTURE_ReconstructPS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD,
	out float4 alphaOutput : SV_Target0,
	out float4 rgbaOutput : SV_Target1
)
{
	const float3 blackRGB = saturate(tex2D(NS_CAPTURE_BlackSampler, texcoord).rgb);
	const float3 whiteRGB = saturate(tex2D(NS_CAPTURE_WhiteSampler, texcoord).rgb);
	const float uncovered = dot(saturate(whiteRGB - blackRGB), 1.0 / 3.0);
	const float alpha = saturate(1.0 - uncovered);
	const float3 straightRGB = alpha > 0.000001 ? blackRGB / alpha : 0.0;

	rgbaOutput = float4(saturate(straightRGB), alpha);
	if (NS_CAPTURE_DiagnosticOutputs)
	{
		const float rawAlpha = tex2D(NS_CAPTURE_RawGameAlphaSampler, texcoord).r;
		alphaOutput = float4(rawAlpha, rawAlpha, rawAlpha, 1.0);
		return;
	}

	alphaOutput = float4(alpha, alpha, alpha, 1.0);
}

float4 NS_CAPTURE_PreviewPS(
	float4 position : SV_Position,
	float2 texcoord : TEXCOORD
) : SV_Target
{
	if (NS_CAPTURE_PreviewMode == 0)
		return tex2D(ReShade::BackBuffer, texcoord);

	const float4 finalRGBA = tex2D(NS_CAPTURE_RGBA_sampler, texcoord);
	if (NS_CAPTURE_PreviewMode == 4)
		return float4(finalRGBA.aaa, 1.0);

	// Depth diagnostics: verify the Generic Depth buffer itself before any
	// alpha math is trusted. Depth input shows the linearized depth ReShade
	// selected; Background mask shows the exact step(threshold, depth) the
	// capture uses (white = background, black = subject).
	if (NS_CAPTURE_PreviewMode == 6)
	{
		float depth = ReShade::GetLinearizedDepth(texcoord);
		if (NS_CAPTURE_InvertDepth)
			depth = 1.0 - depth;
		return float4(saturate(depth).xxx, 1.0);
	}
	if (NS_CAPTURE_PreviewMode == 7)
	{
		const float mask = NS_CAPTURE_BackgroundMask(texcoord);
		return float4(mask.xxx, 1.0);
	}

	const float checker = frac((floor(position.x / 16.0) + floor(position.y / 16.0)) * 0.5) * 2.0;
	float3 background = lerp(0.18, 0.42, checker);
	if (NS_CAPTURE_PreviewMode == 2)
		background = 0.0;
	else if (NS_CAPTURE_PreviewMode == 3)
		background = 1.0;
	else if (NS_CAPTURE_PreviewMode == 5)
		background = float3(1.0, 0.0, 0.0);

	return float4(finalRGBA.rgb * finalRGBA.a + background * (1.0 - finalRGBA.a), 1.0);
}

technique NS_VFXCapture
<
	ui_label = "NS_VFXCapture";
	ui_tooltip = "Run once after scene/VFX rendering and before UI. NS_VFX_RawGameAlpha bypasses all depth and alpha repair.";
>
{
	pass CaptureRawGameAlpha
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_CAPTURE_RawGameAlphaPS;
		RenderTarget = NS_VFX_RawGameAlpha;
	}

	pass ResolveOrderedTransparency
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_CAPTURE_PreparePS;
		RenderTarget = NS_CAPTURE_Prepared;
	}

	pass ReconstructSynchronizedRGBA
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_CAPTURE_ReconstructPS;
		RenderTarget0 = NS_VFX_Alpha;
		RenderTarget1 = NS_VFX_RGBA;
	}

	pass PreviewFinalRGBA
	{
		VertexShader = PostProcessVS;
		PixelShader = NS_CAPTURE_PreviewPS;
	}
}
