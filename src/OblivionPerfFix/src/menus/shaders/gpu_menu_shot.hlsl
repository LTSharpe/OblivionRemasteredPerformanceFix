// GpuMenuShot (lab): the menu-open screenshot's backdrop picture made on the GPU.
// Input: the back buffer's R10G10B10A2 pixels, copied into a raw buffer (rows InPitch uints apart).
// Each 10-bit channel goes through a 1024-entry table to its 8-bit value (the table is built on the CPU with the game's own
// conversion: the SDR read-back's requantize, or the HDR route's ToFColorSRGB), then the same-size 2x2 box filter of
// FImageUtils::ImageResize: the pixel and its right, lower and lower-right neighbours (those that exist), each channel
// summed and shifted right by 0, 1 or 2 (truncated), then each colour byte through a 256-entry table (identity, or
// HdrMenuBackground's HDR10 table, which the game's handler applies after its resize when that tweak is on), alpha 255.
// Output: FColor (B, G, R, A bytes) rows OutPitch uints apart.
// Compiled with: fxc /T cs_5_1 /E main /Qstrip_reflect /Qstrip_debug /Fh gpu_menu_shot_cs.h /Vn kGpuMenuShotCs
#define RS "RootConstants(num32BitConstants=4, b0), SRV(t0), SRV(t1), UAV(u0)"

cbuffer Params : register(b0) { uint W; uint H; uint InPitch; uint OutPitch; };
ByteAddressBuffer Lut : register(t0);       // B table, G table, R table: 1024 uints each; then the 256-entry byte table
ByteAddressBuffer Src : register(t1);
RWByteAddressBuffer Dst : register(u0);

uint3 Quant(uint p)
{
    return uint3(Lut.Load(((p >> 20) & 1023u) * 4u), Lut.Load((1024u + ((p >> 10) & 1023u)) * 4u), Lut.Load((2048u + (p & 1023u)) * 4u));
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint x = id.x, y = id.y;
    if (x >= W || y >= H) return;
    const bool right = x + 1u < W, down = y + 1u < H;
    const uint row = y * InPitch;
    uint3 s = Quant(Src.Load((row + x) * 4u));
    if (right) s += Quant(Src.Load((row + x + 1u) * 4u));
    if (down) s += Quant(Src.Load((row + InPitch + x) * 4u));
    if (right && down) s += Quant(Src.Load((row + InPitch + x + 1u) * 4u));
    const uint shift = (right ? 1u : 0u) + (down ? 1u : 0u);
    s >>= shift;
    s = uint3(Lut.Load((3072u + s.x) * 4u), Lut.Load((3072u + s.y) * 4u), Lut.Load((3072u + s.z) * 4u));
    Dst.Store((y * OutPitch + x) * 4u, s.x | (s.y << 8) | (s.z << 16) | 0xFF000000u);
}
