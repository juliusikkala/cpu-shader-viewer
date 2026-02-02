void mainImage(out float4 color, float2 coord)
{
    int2 icoord = int2(coord) / 16;

    if (((icoord.x ^ icoord.y) & 1) == 1)
    {
        color = float4(0,0,0,1);
    }
    else
    {
        color = float4(coord.x / iResolution.x, coord.y / iResolution.y,0,1);
    }
}
