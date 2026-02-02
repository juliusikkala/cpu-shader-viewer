void mainImage(out float4 color, float2 coord)
{
    float2 uv = coord / iResolution.xy;
    color = uv.xyxy;
}
