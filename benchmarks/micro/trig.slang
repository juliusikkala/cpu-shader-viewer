void mainImage(out float4 color, float2 coord)
{
    float2 uv = coord / iResolution.xy;
    float pi = float.getPi();
    color = float4(
        sin(uv.x * pi) * 0.5 + 0.5,
        cos(uv.y * pi) * 0.5 + 0.5,
        tan(uv.x * uv.y * pi * 0.25),
        1.0);
}
