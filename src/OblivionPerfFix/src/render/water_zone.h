// AWaterZone's layout (UE 5.3 WaterZoneActor.h, build 19115871), shared by WaterInfoSpread (water_spread.cpp) and WaterInfoSkip
// (water_info_skip.cpp) so the two fixes read one layout (rev130f WS-S3). Neither trusts these numbers alone: WaterInfoSpread reads
// the rebuild flag, WaterHeightExtents and GroundZMin offsets from UpdateWaterInfoTexture's own code and installs only when they
// equal these; WaterInfoSkip finds the same WaterHeightExtents / GroundZMin reads in UpdateWaterInfoRendering's code (its pins).
// So a game update that moves a field switches both off (each with a log line) instead of one reading the wrong bytes.
#pragma once
#include <cstdint>

namespace opf {
constexpr uint32_t kWaterZoneTexture = 0x298;        // UTextureRenderTarget2D* WaterInfoTexture
constexpr uint32_t kWaterZoneResolution = 0x2B0;     // FIntPoint RenderTargetResolution
constexpr uint32_t kWaterZoneRebuildFlag = 0x300;    // bool bNeedsWaterInfoRebuild
constexpr uint32_t kWaterZoneHeightExtents = 0x304;  // FVector2f WaterHeightExtents (right after the flag)
constexpr uint32_t kWaterZoneGroundZMin = 0x30C;     // float GroundZMin
}  // namespace opf
