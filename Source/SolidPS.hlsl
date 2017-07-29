
[RootSignature("RootFlags(0)")]
float4 main(float4 Position : SV_Position) : SV_TARGET
{
	float2 P = Position.xy / float2(1280.0f, 720.0f);
	return float4(P, 0.0f, 1.0f);
}
