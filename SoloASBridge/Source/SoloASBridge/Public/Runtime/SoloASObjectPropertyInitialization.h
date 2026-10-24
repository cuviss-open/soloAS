#pragma once

#include "CoreTypes.h"

class FProperty;

namespace SoloASBridge
{
/**
 * Recognizes completed object storage in the current initializer's exact root
 * or recorded subobjects. Never derives or dereferences an object from Data.
 * Unrelated property buffers and deferred initializers are not admitted.
 */
SOLOASBRIDGE_API bool IsConstructedObjectPropertyStorage(
    const FProperty& Property, const void* Data);
}
