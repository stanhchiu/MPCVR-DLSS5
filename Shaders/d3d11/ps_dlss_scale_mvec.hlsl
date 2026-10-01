Texture2D<float2> MotionVectors : register(t0);
Texture2D<float4> UpscaledLuma  : register(t1);
SamplerState samLinear          : register(s0);
SamplerState samPoint           : register(s1);

cbuffer constants : register(b0)
{
	float2 scale;     // x = dst_width / src_width, y = dst_height / src_height
	float2 inv_scale; // 1.0 / scale
	float2 src_texel; // 1.0 / src_width, 1.0 / src_height
	float2 dst_texel; // 1.0 / dst_width, 1.0 / dst_height
};

struct PS_INPUT
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD0;
};

// Joint Bilateral Upsampling of Motion Vectors
// Color/Edge aware upsampling using the upscaled frame's luma.
float2 main(PS_INPUT input) : SV_Target
{
	// The coordinate in the source texture
	float2 src_coord = input.Tex * inv_scale;
	
	// Sample the upscaled luma at the current high-res pixel
	float center_luma = dot(UpscaledLuma.SampleLevel(samPoint, input.Tex, 0).rgb, float3(0.299, 0.587, 0.114));
	
	float2 best_mv = float2(0.0, 0.0);
	float total_weight = 0.0;
	
	// Evaluate a 3x3 neighborhood in the low-res motion vector space
	for (int y = -1; y <= 1; ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			float2 offset = float2(x, y) * src_texel;
			float2 sample_uv = saturate(src_coord + offset); // Prevent out-of-bounds
			
			// Get low-res motion vector
			float2 mv = MotionVectors.SampleLevel(samPoint, sample_uv, 0);
			
			// We need the corresponding luma in the high-res texture
			// Wait, joint bilateral upsampling uses the high-res luma to guide the weights.
			// Let's sample the high-res luma at the corresponding position of the low-res pixel.
			float2 hr_uv = sample_uv * scale;
			float sample_luma = dot(UpscaledLuma.SampleLevel(samLinear, hr_uv, 0).rgb, float3(0.299, 0.587, 0.114));
			
			// Compute spatial weight (Gaussian)
			float spatial_dist_sq = dot(offset, offset);
			float spatial_weight = exp(-spatial_dist_sq * 10.0);
			
			// Compute range (color) weight
			float color_dist = abs(center_luma - sample_luma);
			float color_weight = exp(-color_dist * 10.0);
			
			float weight = spatial_weight * color_weight;
			
			best_mv += mv * weight;
			total_weight += weight;
		}
	}
	
	if (total_weight > 0.0) {
		best_mv /= total_weight;
	}
	
	// Scale the vector magnitude
	return best_mv * scale;
}
