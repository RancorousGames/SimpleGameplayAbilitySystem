#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GameplayTagContainer.h"
#include "SimpleGameplayAbilitySystem/SimpleEventSubsystem/SimpleEventTypes.h" // For FInstancedStruct
#include "AttributeEventReceiver.generated.h"

UCLASS()
class UAttributeEventReceiver : public UObject
{
    GENERATED_BODY()
public:
    bool bEventFired = false;

    UFUNCTION()
    void HandleFloatAttributeChanged(FGameplayTag AttributeTag, float OldValue, float NewValue)
    {
        bEventFired = true;
    }

    UFUNCTION()
    void HandleStructAttributeChanged(FGameplayTag AttributeTag, FInstancedStruct OldValue, FInstancedStruct NewValue, FGameplayTagContainer ModificationTags)
    {
        bEventFired = true;
    }
};