#include "guest_patch_set.h"
#include "guest_small_data.h"

#include "generated/RuntimeConfig.h"

namespace GuestSmallData {
extern const uint32_t kSda1Base = RuntimeConfig::SDA1_BASE;
extern const uint32_t kSda2Base = RuntimeConfig::SDA2_BASE;
} // namespace GuestSmallData

namespace GuestPatchSet {
extern const char* const kName = RuntimeConfig::GUEST_PATCH_SET;
} // namespace GuestPatchSet
