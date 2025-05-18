#pragma once

#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/AttributeHandler/SimpleAttributeHandler.h"
#include "TestGameplayTags.cpp"

// Include generated class
#include "AttributesTest.generated.h"

// Mock Attribute Handler for struct tests
UCLASS()
class UMockAttributeHandler : public USimpleAttributeHandler
{
	GENERATED_BODY()
public:
	virtual FGameplayTagContainer GetModificationEvents_Implementation(const FInstancedStruct& OldValue, const FInstancedStruct& NewValue) override
	{
		FGameplayTagContainer Tags;
		// When the struct is modified, we want to send this specific event tag.
		Tags.AddTag(StructModifiedEventTag);
		return Tags;
	}
};