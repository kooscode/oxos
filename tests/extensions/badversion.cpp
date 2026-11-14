// Test extension (spec/oxos_internal/34 §9.3): built for a later major API version, so the kernel refuses it. It writes its descriptor itself, with that
// version (OXOS_DEFINE_EXTENSION always writes the SDK's own).
#include <oxos_sdk.h>

static BOOLEAN ExtensionStart(void) {
    return TRUE;
}

extern "C" __declspec(dllexport) const OXOS_EXTENSION oxos_extension = {
    .Magic = OXOS_EXTENSION_MAGIC, .ApiVersion = OXOS_API_VERSION + (1u << 16), .Context = &oxos_context, .Name = "badversion", .Start = ExtensionStart};
