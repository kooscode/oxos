// Test extension (spec/oxos_internal/34 §9.3): built for a later major API version, so the kernel refuses it.
#include <oxos_sdk.h>

namespace {

NTSTATUS NTAPI Load(OXOS_CONTEXT *Context) {
    return STATUS_SUCCESS;
}

} // namespace

extern "C" __declspec(dllexport) const OXOS_EXTENSION oxos_extension = {
    .Magic = OXOS_EXTENSION_MAGIC, .ApiVersion = OXOS_API_VERSION + (1u << 16), .Name = "badversion", .Load = Load};
