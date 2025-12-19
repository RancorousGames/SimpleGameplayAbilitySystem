#pragma once

#include "TestGameplayTags.h"
#include "NativeGameplayTags.h"

// Define Gameplay Tags
UE_DEFINE_GAMEPLAY_TAG(TestAttributeTag, "Test.SGAS.Attributes.MyTestAttribute");
UE_DEFINE_GAMEPLAY_TAG(StructModifiedEventTag, "Test.Event.StructModified");

// Regeneration tests native tags
UE_DEFINE_GAMEPLAY_TAG(TestHealthTag, "Test.SGAS.Attributes.Health");
UE_DEFINE_GAMEPLAY_TAG(TestHealthRegenRateTag, "Test.SGAS.Attributes.HealthRegenRate");
UE_DEFINE_GAMEPLAY_TAG(TestEnergyTag, "Test.SGAS.Attributes.Energy");
UE_DEFINE_GAMEPLAY_TAG(TestEnergyRegenRateTag, "Test.SGAS.Attributes.EnergyRegenRate");
