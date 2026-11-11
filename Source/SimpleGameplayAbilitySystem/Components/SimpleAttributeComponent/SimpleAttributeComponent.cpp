#include "SimpleAttributeComponent.h"

#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent//AttributeHandler/SimpleAttributeHandler.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleTimeSynchronizerComponent/ExtendedTimeSynchronizer/SimpleTimeSynchronizerExtended.h"
#include "SimpleGameplayAbilitySystem/Module/SimpleGameplayAbilitySystem.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeModifier/SimpleAttributeModifier.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeModifier/ModifierActions/ChangeFloatAttributeAction/FloatAttributeActionTypes.h"
#include "SimpleGameplayAbilitySystem/DefaultTags/DefaultTags.h"
#include "SimpleGameplayAbilitySystem/SimpleEventSubsystem/SimpleEventSubsystem.h"
#include "TimerManager.h"
#include <limits>

USimpleAttributeComponent::USimpleAttributeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void USimpleAttributeComponent::BeginPlay()
{
	UActorComponent::BeginPlay();

	SetIsReplicated(true);

	if (UWorld* World = GetWorld())
	{
		// Start periodic EventID cleanup timer (every 30 seconds)
		World->GetTimerManager().SetTimer(
			EventIDCleanupTimerHandle,
			this,
			&USimpleAttributeComponent::CleanupOldEventIDs,
			30.0f,
			true
		);

		// Periodic cleanup of old modifier states (every 10 seconds). Needed on both sides: the server prunes the
		// replicated states and mutations, a client prunes its predictions and local copies.
		World->GetTimerManager().SetTimer(
			CleanupTimerHandle,
			this,
			&USimpleAttributeComponent::CleanupOldModifierStates,
			10.0f,
			true
		);
	}
	
	if (HasAuthority())
	{
		// Add default attributes defined on the component
		for (const FStructAttribute Attribute : DefaultStructAttributes)
		{
			AddStructAttribute(Attribute);
		}

		for (const FFloatAttribute Attribute : DefaultFloatAttributes)
		{
			AddFloatAttribute(Attribute);
		}

		for (USimpleAttributeSet* AttributeSet : AttributeSets)
		{
			for (const FFloatAttribute Attribute : AttributeSet->FloatAttributes)
			{
				AddFloatAttribute(Attribute);
			}

			for (const FStructAttribute Attribute : AttributeSet->StructAttributes)
			{
				AddStructAttribute(Attribute);
			}
		}

		// Add default gameplay tags defined on the component
		for (const FGameplayTag& Tag : DefaultGameplayTags)
		{
			AddGameplayTag(Tag);
		}

		return;
	}

	// Delegates called on the client to handle replicated data
	AuthorityAttributeModifierStates.OnStateAdded.BindUObject(
		this, &USimpleAttributeComponent::ClientOnAttributeModiferStateAdded);
	AuthorityAttributeModifierStates.OnStateChanged.BindUObject(
		this, &USimpleAttributeComponent::ClientOnAttributeModifierStateChanged);
	AuthorityAttributeModifierStates.OnStateRemoved.BindUObject(
		this, &USimpleAttributeComponent::ClientOnAttributeModiferStateRemoved);

	AuthorityAttributeModifierMutations.OnStateAdded.BindUObject(
		this, &USimpleAttributeComponent::ClientOnAttributeModifierMutationAdded);

	AuthorityFloatAttributes.OnFloatAttributeAdded.BindUObject(
		this, &USimpleAttributeComponent::ClientOnFloatAttributeAdded);
	AuthorityFloatAttributes.OnFloatAttributeChanged.BindUObject(
		this, &USimpleAttributeComponent::ClientOnFloatAttributeChanged);
	AuthorityFloatAttributes.OnFloatAttributeRemoved.BindUObject(
        this, &USimpleAttributeComponent::ClientOnFloatAttributeRemoved);
    AuthorityOwnerOnlyFloatAttributes.OnFloatAttributeAdded.BindUObject(
        this, &USimpleAttributeComponent::ClientOnFloatAttributeAdded);
    AuthorityOwnerOnlyFloatAttributes.OnFloatAttributeChanged.BindUObject(
        this, &USimpleAttributeComponent::ClientOnFloatAttributeChanged);
    AuthorityOwnerOnlyFloatAttributes.OnFloatAttributeRemoved.BindUObject(
        this, &USimpleAttributeComponent::ClientOnFloatAttributeRemoved);

	AuthorityStructAttributes.OnStructAttributeAdded.BindUObject(
		this, &USimpleAttributeComponent::ClientOnStructAttributeAdded);
	AuthorityStructAttributes.OnStructAttributeChanged.BindUObject(
		this, &USimpleAttributeComponent::ClientOnStructAttributeChanged);
	AuthorityStructAttributes.OnStructAttributeRemoved.BindUObject(
		this, &USimpleAttributeComponent::ClientOnStructAttributeRemoved);

	AuthorityGameplayTags.OnGameplayTagCounterAdded.BindUObject(
		this, &USimpleAttributeComponent::ClientOnGameplayTagAdded);
	AuthorityGameplayTags.OnGameplayTagCounterRemoved.BindUObject(
		this, &USimpleAttributeComponent::ClientOnGameplayTagRemoved);

	// Only initialize local state if we've already received initial replication data
	// Otherwise, the FastArraySerializer callbacks will populate the local state
	// A client with only owner-private attributes must still initialize them.
	if (!AuthorityFloatAttributes.Attributes.IsEmpty()
        || !AuthorityOwnerOnlyFloatAttributes.Attributes.IsEmpty())
	{
		LocalFloatAttributes = AuthorityFloatAttributes.Attributes;
        for (const FFloatAttribute& PrivateAttribute : AuthorityOwnerOnlyFloatAttributes.Attributes)
            LocalFloatAttributes.AddUnique(PrivateAttribute);
	}
	if (AuthorityStructAttributes.Attributes.Num() > 0)
	{
		LocalStructAttributes = AuthorityStructAttributes.Attributes;
	}
	if (AuthorityGameplayTags.Tags.Num() > 0)
	{
		LocalGameplayTags = AuthorityGameplayTags.Tags;
	}
}

void USimpleAttributeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Clear cleanup timers
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CleanupTimerHandle);
		World->GetTimerManager().ClearTimer(EventIDCleanupTimerHandle);
	}

	// Modifiers still running are torn down quietly (timers, temporary tags, event subscription): running their
	// end/cancel actions on a component that is going away would only touch dying state.
	const TArray<USimpleAttributeModifier*> ModifiersToAbort = InstancedAttributeModifiers;
	InstancedAttributeModifiers.Empty();
	for (USimpleAttributeModifier* Modifier : ModifiersToAbort)
	{
		if (Modifier)
		{
			Modifier->AbortWithoutNotify();
		}
	}

	Super::EndPlay(EndPlayReason);
}

/* Gameplay Tags */

void USimpleAttributeComponent::AddGameplayTag(FGameplayTag Tag)
{
	TArray<FGameplayTagCounter>& TagCounters = HasAuthority() ? AuthorityGameplayTags.Tags : LocalGameplayTags;
	FGameplayTagCounter* TagCounter = TagCounters.FindByPredicate([Tag](const FGameplayTagCounter& TagCounter)
	{
		return TagCounter.GameplayTag.MatchesTagExact(Tag);
	});

	if (TagCounter)
	{
		TagCounter->ReferenceCounter++;

		if (HasAuthority())
		{
			AuthorityGameplayTags.MarkItemDirty(*TagCounter);
		}

		return;
	}

	FGameplayTagCounter NewTagCounter;
	NewTagCounter.GameplayTag = Tag;
	NewTagCounter.ReferenceCounter = 1;

	// Mark the array element, not the local copy, so the element itself carries its replication ID.
	FGameplayTagCounter& AddedTagCounter = TagCounters.Add_GetRef(NewTagCounter);

	if (HasAuthority())
	{
		AuthorityGameplayTags.MarkItemDirty(AddedTagCounter);
	}

	OnGameplayTagAdded.Broadcast(Tag);
}

void USimpleAttributeComponent::RemoveGameplayTag(FGameplayTag Tag)
{
	TArray<FGameplayTagCounter>& TagCounters = HasAuthority() ? AuthorityGameplayTags.Tags : LocalGameplayTags;
	FGameplayTagCounter* TagCounter = TagCounters.FindByPredicate([Tag](const FGameplayTagCounter& TagCounter)
	{
		return TagCounter.GameplayTag.MatchesTagExact(Tag);
	});

	if (!TagCounter)
	{
		return;
	}

	if (TagCounter->ReferenceCounter > 1)
	{
		TagCounter->ReferenceCounter--;

		if (HasAuthority())
		{
			AuthorityGameplayTags.MarkItemDirty(*TagCounter);
		}

		return;
	}

	TagCounters.RemoveSingle(*TagCounter);

	if (HasAuthority())
	{
		AuthorityGameplayTags.MarkArrayDirty();
	}

	OnGameplayTagRemoved.Broadcast(Tag);
}

bool USimpleAttributeComponent::HasGameplayTag(FGameplayTag Tag)
{
	const TArray<FGameplayTagCounter>& TagCounters = HasAuthority() ? AuthorityGameplayTags.Tags : LocalGameplayTags;
	return TagCounters.ContainsByPredicate([Tag](const FGameplayTagCounter& TagCounter)
	{
		return TagCounter.GameplayTag.MatchesTagExact(Tag);
	});
}

bool USimpleAttributeComponent::HasAllGameplayTags(FGameplayTagContainer Tags)
{
	const TArray<FGameplayTagCounter>& TagCounters = HasAuthority() ? AuthorityGameplayTags.Tags : LocalGameplayTags;

	for (const FGameplayTag& Tag : Tags)
	{
		if (!TagCounters.ContainsByPredicate([Tag](const FGameplayTagCounter& TagCounter)
		{
			return TagCounter.GameplayTag.MatchesTagExact(Tag);
		}))
		{
			return false;
		}
	}

	return true;
}

bool USimpleAttributeComponent::HasAnyGameplayTags(FGameplayTagContainer Tags)
{
	const TArray<FGameplayTagCounter>& TagCounters = HasAuthority() ? AuthorityGameplayTags.Tags : LocalGameplayTags;

	for (const FGameplayTag& Tag : Tags)
	{
		if (TagCounters.ContainsByPredicate([Tag](const FGameplayTagCounter& TagCounter)
		{
			return TagCounter.GameplayTag.MatchesTagExact(Tag);
		}))
		{
			return true;
		}
	}

	return false;
}

FGameplayTagContainer USimpleAttributeComponent::GetActiveGameplayTags() const
{
	FGameplayTagContainer ActiveGameplayTags;

	const TArray<FGameplayTagCounter>& TagCounters = HasAuthority() ? AuthorityGameplayTags.Tags : LocalGameplayTags;
	for (const FGameplayTagCounter& TagCounter : TagCounters)
	{
		if (TagCounter.ReferenceCounter > 0)
		{
			ActiveGameplayTags.AddTag(TagCounter.GameplayTag);
		}
	}

	return ActiveGameplayTags;
}

/* Float Attributes */

void USimpleAttributeComponent::MarkFloatAttributeDirty(FFloatAttribute& Attribute)
{
    if (Attribute.Replication == EAttributeReplication::OwnerOnly)
        AuthorityOwnerOnlyFloatAttributes.MarkItemDirty(Attribute);
    else
        AuthorityFloatAttributes.MarkItemDirty(Attribute);
}

void USimpleAttributeComponent::AddFloatAttribute(FFloatAttribute AttributeToAdd, bool OverrideValuesIfExists)
{
    if (!HasAuthority()) return;
    FFloatAttribute* Existing = GetFloatAttribute(AttributeToAdd.AttributeTag);
    if (Existing)
    {
        if (!OverrideValuesIfExists) return;
        if (Existing->Replication == AttributeToAdd.Replication)
        {
            AssignKeepingReplicationIdentity(*Existing, AttributeToAdd);
            MarkFloatAttributeDirty(*Existing);
            return;
        }
        // Moving containers is mandatory: a public array must never keep a stale
        // copy of an attribute newly marked private.
        AuthorityFloatAttributes.Attributes.RemoveAll([&](const FFloatAttribute& A)
            { return A.AttributeTag.MatchesTagExact(AttributeToAdd.AttributeTag); });
        AuthorityOwnerOnlyFloatAttributes.Attributes.RemoveAll([&](const FFloatAttribute& A)
            { return A.AttributeTag.MatchesTagExact(AttributeToAdd.AttributeTag); });
        AuthorityFloatAttributes.MarkArrayDirty();
        AuthorityOwnerOnlyFloatAttributes.MarkArrayDirty();
    }
    FFloatAttributeContainer& Destination = AttributeToAdd.Replication == EAttributeReplication::OwnerOnly
        ? AuthorityOwnerOnlyFloatAttributes : AuthorityFloatAttributes;
    FFloatAttribute& Added = Destination.Attributes.Add_GetRef(AttributeToAdd);
    Destination.MarkItemDirty(Added);
    if (!Existing) OnFloatAttributeAdded.Broadcast(AttributeToAdd.AttributeTag, AttributeToAdd);
}

bool USimpleAttributeComponent::SetFloatAttributeReplication(
    FGameplayTag AttributeTag, EAttributeReplication Replication)
{
    if (!HasAuthority()) return false;
    const FFloatAttribute* Existing = GetFloatAttribute(AttributeTag);
    if (!Existing) return false;
    if (Existing->Replication == Replication) return true;
    FFloatAttribute Copy = *Existing;
    Copy.Replication = Replication;
    AddFloatAttribute(Copy, true);
    return true;
}

void USimpleAttributeComponent::RemoveFloatAttribute(FGameplayTag AttributeTag)
{
    if (!HasAuthority()) return;
    const int32 PublicRemoved = AuthorityFloatAttributes.Attributes.RemoveAll([&](const FFloatAttribute& A)
        { return A.AttributeTag.MatchesTagExact(AttributeTag); });
    const int32 PrivateRemoved = AuthorityOwnerOnlyFloatAttributes.Attributes.RemoveAll([&](const FFloatAttribute& A)
        { return A.AttributeTag.MatchesTagExact(AttributeTag); });
    if (PublicRemoved) AuthorityFloatAttributes.MarkArrayDirty();
    if (PrivateRemoved) AuthorityOwnerOnlyFloatAttributes.MarkArrayDirty();
    if (PublicRemoved || PrivateRemoved) OnFloatAttributeRemoved.Broadcast(AttributeTag);
}

bool USimpleAttributeComponent::HasFloatAttribute(const FGameplayTag AttributeTag)
{
	if (GetFloatAttribute(AttributeTag))
	{
		return true;
	}

	return false;
}

bool USimpleAttributeComponent::HasStructAttribute(const FGameplayTag AttributeTag)
{
	if (GetStructAttribute(AttributeTag))
	{
		return true;
	}

	return false;
}

float USimpleAttributeComponent::GetFloatAttributeValue(EFloatAttributeValueType ValueType, FGameplayTag AttributeTag,
                                                        bool& WasFound)
{
	if (const FFloatAttribute* Attribute = GetFloatAttribute(AttributeTag))
	{
		WasFound = true;

		switch (ValueType)
		{
			case EFloatAttributeValueType::BaseValue:
				return Attribute->BaseValue;
			case EFloatAttributeValueType::SyncedValue:
				return Attribute->CurrentValue;
			case EFloatAttributeValueType::CurrentValue:
				return Attribute->CurrentValue;
			case EFloatAttributeValueType::CurrentValueRatio:
			{
				const float Denom = (Attribute->ValueLimits.UseMaxCurrentValue && Attribute->ValueLimits.MaxCurrentValue > 0.f)
					? Attribute->ValueLimits.MaxCurrentValue
					: 0.f;
				return Denom > 0.f ? Attribute->CurrentValue / Denom : 0.f;
			}
			case EFloatAttributeValueType::MaxCurrentValue:
				return Attribute->ValueLimits.MaxCurrentValue;
			case EFloatAttributeValueType::MinCurrentValue:
				return Attribute->ValueLimits.MinCurrentValue;
			case EFloatAttributeValueType::MaxBaseValue:
				return Attribute->ValueLimits.MaxBaseValue;
			case EFloatAttributeValueType::MinBaseValue:
				return Attribute->ValueLimits.MinBaseValue;
			case EFloatAttributeValueType::RegenRate:
				return Attribute->CurrentRegenRate;
			default:
				SIMPLE_LOG(this, FString::Printf(
					           TEXT(
						           "[USimpleAttributeFunctionLibrary::GetFloatAttributeValue]: ValueType %d not supported."),
					           static_cast<int32>(ValueType)));
				WasFound = false;
				return 0.0f;
		}
	}

	WasFound = false;
	return 0.0f;
}

bool USimpleAttributeComponent::SetFloatAttributeValue(EFloatAttributeValueType ValueType, FGameplayTag AttributeTag,
                                                       float NewValue, float& Overflow)
{
	FFloatAttribute* Attribute = GetFloatAttribute(AttributeTag);

	if (!Attribute)
	{
		SIMPLE_LOG(this, FString::Printf(
			           TEXT("[USimpleAttributeFunctionLibrary::SetFloatAttributeValue]: Attribute %s not found."),
			           *AttributeTag.ToString()));
		return false;
	}

	float OldValue = 0.0f;
	const float ClampedValue = ClampFloatAttributeValue(*Attribute, ValueType, NewValue, Overflow);

	switch (ValueType)
	{
		case EFloatAttributeValueType::BaseValue:
			OldValue = Attribute->BaseValue;
			Attribute->BaseValue = ClampedValue;
			if (OldValue != ClampedValue)
			{
				OnFloatAttributeBaseValueChanged.Broadcast(AttributeTag, OldValue, ClampedValue);
			}
			// Broadcast limit reached events if the value reached or exceeded a limit
			if (Attribute->ValueLimits.UseMaxBaseValue && ClampedValue == Attribute->ValueLimits.MaxBaseValue && OldValue < ClampedValue)
			{
				OnMaxBaseValueReached.Broadcast(AttributeTag, Overflow);
			}
			else if (Attribute->ValueLimits.UseMinBaseValue && ClampedValue == Attribute->ValueLimits.MinBaseValue && OldValue > ClampedValue)
			{
				OnMinBaseValueReached.Broadcast(AttributeTag, Overflow);
			}
			break;

		case EFloatAttributeValueType::SyncedValue:
		case EFloatAttributeValueType::CurrentValue:
			OldValue = Attribute->CurrentValue;
			Attribute->CurrentValue = ClampedValue;
			Attribute->LastRegenParamsUpdateTime_Server = GetServerTime();
			if (OldValue != ClampedValue)
			{
				OnFloatAttributeCurrentValueChanged.Broadcast(AttributeTag, OldValue, ClampedValue);
			}
			// Broadcast limit reached events if the value reached or exceeded a limit
			if (Attribute->ValueLimits.UseMaxCurrentValue && ClampedValue == Attribute->ValueLimits.MaxCurrentValue && OldValue < ClampedValue)
			{
				OnMaxCurrentValueReached.Broadcast(AttributeTag, Overflow);
			}
			else if (Attribute->ValueLimits.UseMinCurrentValue && ClampedValue == Attribute->ValueLimits.MinCurrentValue && OldValue > ClampedValue)
			{
				OnMinCurrentValueReached.Broadcast(AttributeTag, Overflow);
			}
			break;

		case EFloatAttributeValueType::MaxCurrentValue:
			OldValue = Attribute->ValueLimits.MaxCurrentValue;
			Attribute->ValueLimits.MaxCurrentValue = ClampedValue;
			if (OldValue != ClampedValue)
			{
				OnFloatAttributeMaxCurrentValueChanged.Broadcast(AttributeTag, OldValue, ClampedValue);
			}
			break;

		case EFloatAttributeValueType::MinCurrentValue:
			OldValue = Attribute->ValueLimits.MinCurrentValue;
			Attribute->ValueLimits.MinCurrentValue = ClampedValue;
			if (OldValue != ClampedValue)
			{
				OnFloatAttributeMinCurrentValueChanged.Broadcast(AttributeTag, OldValue, ClampedValue);
			}
			break;

		case EFloatAttributeValueType::MaxBaseValue:
			OldValue = Attribute->ValueLimits.MaxBaseValue;
			Attribute->ValueLimits.MaxBaseValue = ClampedValue;
			if (OldValue != ClampedValue)
			{
				OnFloatAttributeMaxBaseValueChanged.Broadcast(AttributeTag, OldValue, ClampedValue);
			}
			break;

		case EFloatAttributeValueType::MinBaseValue:
			OldValue = Attribute->ValueLimits.MinBaseValue;
			Attribute->ValueLimits.MinBaseValue = ClampedValue;
			if (OldValue != ClampedValue)
			{
				OnFloatAttributeMinBaseValueChanged.Broadcast(AttributeTag, OldValue, ClampedValue);
			}
			break;

		case EFloatAttributeValueType::RegenRate:
			OldValue = Attribute->CurrentRegenRate;
			// Materialize before changing regen to avoid discontinuities
			MaterializeFloat(AttributeTag);
			Attribute->CurrentRegenRate = FMath::Max(0.f, ClampedValue);
			Attribute->LastRegenParamsUpdateTime_Server = GetServerTime();
			break;

		case EFloatAttributeValueType::CurrentValueRatio:
		{
			const float Denom = (Attribute->ValueLimits.UseMaxCurrentValue && Attribute->ValueLimits.MaxCurrentValue > 0.f)
				? Attribute->ValueLimits.MaxCurrentValue
				: 0.f;
			if (Denom <= 0.f)
			{
				SIMPLE_LOG(this, TEXT("[USimpleAttributeComponent::SetFloatAttributeValue]: No valid MaxCurrentValue for ratio"));
				return false;
			}
			const float RatioClamped = ClampedValue; // already clamped to [0,1] by ClampFloatAttributeValue
			float OverflowUnits = 0.f;
			const float TargetCurrent = RatioClamped * Denom;
			const bool bOk = SetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, AttributeTag, TargetCurrent, OverflowUnits);
			// Convert overflow back to ratio delta based on the achieved current value
			bool bDummyFound = false;
			const float NewCurrent = GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, AttributeTag, bDummyFound);
			Overflow = RatioClamped - (bDummyFound && Denom > 0.f ? NewCurrent / Denom : 0.f);
			return bOk;
		}
	}

	if (HasAuthority())
	{
		MarkFloatAttributeDirty(*Attribute);
	}

	return true;
}

bool USimpleAttributeComponent::IncrementFloatAttributeValue(EFloatAttributeValueType ValueType,
                                                             FGameplayTag AttributeTag, float Increment,
                                                             float& Overflow)
{
	bool WasFound = false;
	const float CurrentValue = GetFloatAttributeValue(ValueType, AttributeTag, WasFound);

	if (!WasFound)
	{
		return false;
	}

	return SetFloatAttributeValue(ValueType, AttributeTag, CurrentValue + Increment, Overflow);
}

bool USimpleAttributeComponent::OverrideFloatAttribute(FGameplayTag AttributeTag, FFloatAttribute NewAttribute)
{
    if (!HasAuthority() || !GetFloatAttribute(AttributeTag)) return false;
    NewAttribute.AttributeTag = AttributeTag;
    AddFloatAttribute(NewAttribute, true);
    return true;
}

float USimpleAttributeComponent::ClampFloatAttributeValue(const FFloatAttribute& Attribute,
                                                          EFloatAttributeValueType ValueType, float NewValue,
                                                          float& Overflow)
{
	switch (ValueType)
	{
	case EFloatAttributeValueType::BaseValue:
		if (Attribute.ValueLimits.UseMaxBaseValue && NewValue > Attribute.ValueLimits.MaxBaseValue)
		{
			Overflow = NewValue - Attribute.ValueLimits.MaxBaseValue;
			return Attribute.ValueLimits.MaxBaseValue;
		}

		if (Attribute.ValueLimits.UseMinBaseValue && NewValue < Attribute.ValueLimits.MinBaseValue)
		{
			Overflow = NewValue - Attribute.ValueLimits.MinBaseValue;
			return Attribute.ValueLimits.MinBaseValue;
		}

		return NewValue;

	case EFloatAttributeValueType::SyncedValue:
	case EFloatAttributeValueType::CurrentValue:
		if (Attribute.ValueLimits.UseMaxCurrentValue && NewValue > Attribute.ValueLimits.MaxCurrentValue)
		{
			Overflow = NewValue - Attribute.ValueLimits.MaxCurrentValue;
			return Attribute.ValueLimits.MaxCurrentValue;
		}

		if (Attribute.ValueLimits.UseMinCurrentValue && NewValue < Attribute.ValueLimits.MinCurrentValue)
		{
			Overflow = NewValue - Attribute.ValueLimits.MinCurrentValue;
			return Attribute.ValueLimits.MinCurrentValue;
		}

		return NewValue;

	case EFloatAttributeValueType::RegenRate:
		// Regen has no min/max limit fields; only clamp to non-negative
		return FMath::Max(0.f, NewValue);
case EFloatAttributeValueType::CurrentValueRatio:
		if (NewValue > 1.0f)
		{
			Overflow = NewValue - 1.0f;
			return 1.0f;
		}
		if (NewValue < 0.0f)
		{
			Overflow = NewValue;
			return 0.0f;
		}
		return NewValue;

	// The limits themselves are not limited. (They used to fall into the default case and come back as zero, so
	// setting a min/max through SetFloatAttributeValue silently zeroed it.)
	case EFloatAttributeValueType::MaxCurrentValue:
	case EFloatAttributeValueType::MinCurrentValue:
	case EFloatAttributeValueType::MaxBaseValue:
	case EFloatAttributeValueType::MinBaseValue:
		return NewValue;

default:
		SIMPLE_LOG(this, TEXT("[USimpleGameplayAbilityComponent::ClampFloatAttributeValue]: ValueType not supported."));
		return 0.0f;
	}
}

FFloatAttribute USimpleAttributeComponent::GetFloatAttributeCopy(FGameplayTag AttributeTag)
{
	if (FFloatAttribute* Attribute = GetFloatAttribute(AttributeTag))
	{
		return *Attribute;
	}

	SIMPLE_LOG(this, FString::Printf(
		           TEXT("[USimpleAttributeFunctionLibrary::GetFloatAttributeCopy]: Attribute %s not found."),
		           *AttributeTag.ToString()));
	return FFloatAttribute();
}

FStructAttribute USimpleAttributeComponent::GetStructAttributeCopy(FGameplayTag AttributeTag)
{
	if (FStructAttribute* Attribute = GetStructAttribute(AttributeTag))
	{
		return *Attribute;
	}

	SIMPLE_LOG(this, FString::Printf(
		           TEXT("[USimpleAttributeFunctionLibrary::GetStructAttributeCopy]: Attribute %s not found."),
		           *AttributeTag.ToString()));
	return FStructAttribute();
}

FFloatAttribute* USimpleAttributeComponent::GetFloatAttribute(FGameplayTag AttributeTag)
{
    TArray<FFloatAttribute>& Public = HasAuthority()
        ? AuthorityFloatAttributes.Attributes : LocalFloatAttributes;
    for (FFloatAttribute& Attribute : Public)
        if (Attribute.AttributeTag.MatchesTagExact(AttributeTag)) return &Attribute;
    if (HasAuthority())
        for (FFloatAttribute& Attribute : AuthorityOwnerOnlyFloatAttributes.Attributes)
            if (Attribute.AttributeTag.MatchesTagExact(AttributeTag)) return &Attribute;
    return nullptr;
}

FStructAttribute* USimpleAttributeComponent::GetStructAttribute(FGameplayTag AttributeTag)
{
	TArray<FStructAttribute>& StructAttributes = HasAuthority() ? AuthorityStructAttributes.Attributes : LocalStructAttributes;

	for (FStructAttribute& StructAttribute : StructAttributes)
	{
		if (StructAttribute.AttributeTag.MatchesTagExact(AttributeTag))
		{
			return &StructAttribute;
		}
	}

	return nullptr;
}

/* Regeneration Helpers */

float USimpleAttributeComponent::GetEffectiveFloatCurrentValue(FGameplayTag AttributeTag, bool& WasFound, bool bIgnoreRegen)
{
	const FFloatAttribute* A = GetFloatAttribute(AttributeTag);
	if (!A)
	{
		WasFound = false;
		return 0.f;
	}
	WasFound = true;
	if (bIgnoreRegen || !A->bIsRegenerating || A->CurrentRegenRate <= 0.f)
	{
		return A->CurrentValue;
	}
	const double Now = GetServerTime();
	const double Dt = FMath::Max(0.0, Now - A->LastRegenParamsUpdateTime_Server);
	const float Target = (A->ValueLimits.UseMaxCurrentValue ? A->ValueLimits.MaxCurrentValue : std::numeric_limits<float>::max());
	const float Effective = FMath::Min(A->CurrentValue + A->CurrentRegenRate * static_cast<float>(Dt), Target);
	return Effective;
}

void USimpleAttributeComponent::MaterializeFloat(FGameplayTag AttributeTag)
{
	FFloatAttribute* A = GetFloatAttribute(AttributeTag);
	if (!A)
	{
		return;
	}
	bool bFound = false;
	const float Effective = GetEffectiveFloatCurrentValue(AttributeTag, bFound, /*bIgnoreRegen=*/false);
	if (!bFound)
	{
		return;
	}
	if (!FMath::IsNearlyEqual(Effective, A->CurrentValue))
	{
		const float Old = A->CurrentValue;
		A->CurrentValue = Effective;
		OnFloatAttributeCurrentValueChanged.Broadcast(AttributeTag, Old, Effective);
	}
	A->LastRegenParamsUpdateTime_Server = GetServerTime();
	if (HasAuthority())
	{
		MarkFloatAttributeDirty(*A);
	}
}

void USimpleAttributeComponent::StartFloatRegen(FGameplayTag AttributeTag)
{
	FFloatAttribute* A = GetFloatAttribute(AttributeTag);
	if (!A)
	{
		return;
	}
	MaterializeFloat(AttributeTag);
	A->bIsRegenerating = true;
	A->LastRegenParamsUpdateTime_Server = GetServerTime();
	if (HasAuthority())
	{
		MarkFloatAttributeDirty(*A);
	}
}

void USimpleAttributeComponent::StopFloatRegen(FGameplayTag AttributeTag)
{
	FFloatAttribute* A = GetFloatAttribute(AttributeTag);
	if (!A)
	{
		return;
	}
	MaterializeFloat(AttributeTag);
	A->bIsRegenerating = false;
	if (HasAuthority())
	{
		MarkFloatAttributeDirty(*A);
	}
}

/* Struct Attributes */

void USimpleAttributeComponent::AddStructAttribute(FStructAttribute AttributeToAdd, bool OverrideValuesIfExists)
{
	if (!HasAuthority())
	{
		SIMPLE_LOG(this, TEXT("[USimpleAttributeComponent::AddStructAttribute]: Cannot add struct attribute on client!"));
		return;
	}

	if (!AttributeToAdd.StructType)
	{
		SIMPLE_LOG(this, FString::Printf(
			           TEXT(
				           "[USimpleGameplayAbilityComponent::AddStructAttribute]: StructType is null for attribute %s! Can't add new attribute"),
			           *AttributeToAdd.AttributeTag.ToString()));
		return;
	}

	const int32 AttributeIndex = AuthorityStructAttributes.Attributes.Find(AttributeToAdd);

	// This is a new attribute
	if (AttributeIndex == INDEX_NONE)
	{
		// Initialize the data within the struct if it's not already initialized
		if (AttributeToAdd.StructType && !AttributeToAdd.AttributeValue.IsValid())
		{
			AttributeToAdd.AttributeValue.InitializeAs(AttributeToAdd.StructType);
		}

		// Mark the array element, not the local copy, so the element itself carries its replication ID.
		FStructAttribute& AddedAttribute = AuthorityStructAttributes.Attributes.Add_GetRef(AttributeToAdd);
		AuthorityStructAttributes.MarkItemDirty(AddedAttribute);
		OnStructAttributeAdded.Broadcast(AttributeToAdd.AttributeTag, AttributeToAdd);

		return;
	}

	// Attribute exists but we don't want to override it
	if (!OverrideValuesIfExists)
	{
		return;
	}

	// Attribute exists and we want to override it
	AssignKeepingReplicationIdentity(AuthorityStructAttributes.Attributes[AttributeIndex], AttributeToAdd);
	AuthorityStructAttributes.MarkItemDirty(AuthorityStructAttributes.Attributes[AttributeIndex]);
	OnStructAttributeAdded.Broadcast(AttributeToAdd.AttributeTag, AttributeToAdd);
}

void USimpleAttributeComponent::RemoveStructAttribute(FGameplayTag AttributeTag)
{
	if (!HasAuthority())
	{
		SIMPLE_LOG(this, TEXT("[USimpleAttributeComponent::RemoveStructAttribute]: Cannot remove struct attribute on client!"));
		return;
	}

	AuthorityStructAttributes.Attributes.RemoveAll([AttributeTag](const FStructAttribute& Attribute)
	{
		return Attribute.AttributeTag == AttributeTag;
	});
	AuthorityStructAttributes.MarkArrayDirty();
	OnStructAttributeRemoved.Broadcast(AttributeTag);
}

FInstancedStruct USimpleAttributeComponent::GetStructAttributeValue(FGameplayTag AttributeTag, bool& WasFound)
{
	if (FStructAttribute* Attribute = GetStructAttribute(AttributeTag))
	{
		WasFound = true;
		return Attribute->AttributeValue;
	}

	SIMPLE_LOG(this, FString::Printf(
		           TEXT("[USimpleAttributeFunctionLibrary::GetStructAttributeValue]: Attribute %s not found."),
		           *AttributeTag.ToString()));
	WasFound = false;
	return FInstancedStruct();
}

bool USimpleAttributeComponent::SetStructAttributeValue(FGameplayTag AttributeTag, FInstancedStruct NewValue)
{
	FStructAttribute* Attribute = GetStructAttribute(AttributeTag);

	if (!Attribute)
	{
		SIMPLE_LOG(this, FString::Printf(
			           TEXT("[USimpleAttributeFunctionLibrary::SetStructAttributeValue]: Attribute %s not found."),
			           *AttributeTag.ToString()));
		return false;
	}

	if (NewValue.GetScriptStruct() != Attribute->StructType)
	{
		UE_LOG(LogSimpleGAS, Warning,
		       TEXT(
			       "[USimpleAttributeComponent::SetStructAttributeValue]: NewValue does not match expected struct type of %s. It's of type %s"
		       ), *Attribute->StructType->GetName(), *NewValue.GetScriptStruct()->GetName());
		return false;
	}

	const FInstancedStruct OldValue = Attribute->AttributeValue;
	FGameplayTagContainer ModificationTags = FGameplayTagContainer();

	if (Attribute->StructAttributeHandler)
	{
		EGetInstancedStructResult HandlerResult = EGetInstancedStructResult::Invalid;
		USimpleAttributeHandler* Handler = GetStructAttributeHandlerInstance(AttributeTag, Attribute->StructAttributeHandler, HandlerResult);
		if (Handler && HandlerResult == EGetInstancedStructResult::Valid)
		{
			ModificationTags = Handler->GetModificationEvents(OldValue, NewValue);
		}
	}

	Attribute->AttributeValue = NewValue;

	if (HasAuthority())
	{
		AuthorityStructAttributes.MarkItemDirty(*Attribute);
	}

	if (OldValue != NewValue)
	{
		OnStructAttributeChanged.Broadcast(AttributeTag, OldValue, NewValue, ModificationTags);
	}

	return true;
}

/* Attribute Modifiers */

bool USimpleAttributeComponent::ApplyAttributeModifierToTarget(
	FGuid& ModifierID,
	TSubclassOf<USimpleAttributeModifier> ModifierClass,
	USimpleAttributeComponent* ModifierTarget,
	const float Magnitude,
	FInstancedStruct ModifierContext)
{
	if (!ModifierClass || !ModifierTarget)
	{
		return false;
	}

	// Check for consolidated stacking - add to existing modifier if one exists
	const USimpleAttributeModifier* ModifierCDO = ModifierClass.GetDefaultObject();
	if (ModifierCDO->StackingConfig.bEnableStacking && ModifierCDO->StackingConfig.StackGroupTag.IsValid())
	{
		USimpleAttributeModifier* ExistingModifier = ModifierTarget->GetActiveModifierInStackGroup(ModifierCDO->StackingConfig.StackGroupTag);
		if (ExistingModifier)
		{
			// Add stack to existing modifier
			ModifierID = ExistingModifier->ModifierID;
			return ExistingModifier->AddStacks(1);
		}
	}

	// Create new modifier instance
	ModifierID = FGuid::NewGuid();
	USimpleAttributeModifier* Modifier = GetAttributeModifierInstance(
		ModifierClass,
		ModifierID,
		this,
		ModifierTarget,
		Magnitude,
		ModifierContext,
		false);

	return ApplyModifierInstance(Modifier, 1);
}

bool USimpleAttributeComponent::ApplyAttributeModifierToTargetPredicted(
	FGuid& ModifierID,
	const TSubclassOf<USimpleAttributeModifier> ModifierClass,
	USimpleAttributeComponent* ModifierTarget,
	const float Magnitude,
	const FInstancedStruct ModifierContext)
{
	if (!ModifierClass || !ModifierTarget)
	{
		return false;
	}

	// Check for consolidated stacking - add to existing modifier if one exists
	const USimpleAttributeModifier* ModifierCDO = ModifierClass.GetDefaultObject();
	if (ModifierCDO->StackingConfig.bEnableStacking && ModifierCDO->StackingConfig.StackGroupTag.IsValid())
	{
		USimpleAttributeModifier* ExistingModifier = ModifierTarget->GetActiveModifierInStackGroup(ModifierCDO->StackingConfig.StackGroupTag);
		if (ExistingModifier)
		{
			// Add stack to existing modifier
			ModifierID = ExistingModifier->ModifierID;

			// Capture snapshot for prediction
			if (!HasAuthority())
			{
				FPredictedModifierSnapshot Snapshot;
				Snapshot.ModifierID = ModifierID;
				Snapshot.AttributeSnapshot = ModifierTarget->CaptureAttributeSnapshot();
				Snapshot.SnapshotTimestamp = GetServerTime();
				ModifierTarget->PredictedModifierSnapshots.Add(Snapshot);
			}

			const bool WasApplied = ExistingModifier->AddStacks(1);

			if (!HasAuthority() && WasApplied)
			{
				ServerApplyAttributeModifierToTarget(ModifierID, ModifierClass, ModifierTarget, Magnitude, ModifierContext);
			}

			return WasApplied;
		}
	}

	ModifierID = FGuid::NewGuid();

	// Capture snapshot before applying if this is a client prediction
	if (!HasAuthority() && ModifierTarget)
	{
		FPredictedModifierSnapshot Snapshot;
		Snapshot.ModifierID = ModifierID;
		Snapshot.AttributeSnapshot = ModifierTarget->CaptureAttributeSnapshot();
		Snapshot.SnapshotTimestamp = GetServerTime();
		ModifierTarget->PredictedModifierSnapshots.Add(Snapshot);
	}

	USimpleAttributeModifier* Modifier = GetAttributeModifierInstance(
		ModifierClass,
		ModifierID,
		this,
		ModifierTarget,
		Magnitude,
		ModifierContext,
		true);

	const bool WasApplied = ApplyModifierInstance(Modifier, 1);

	if (!HasAuthority())
	{
		if (WasApplied)
		{
			ServerApplyAttributeModifierToTarget(ModifierID, ModifierClass, ModifierTarget, Magnitude, ModifierContext);
		}
		else
		{
			// Application failed on client, remove the snapshot
			if (ModifierTarget)
			{
				ModifierTarget->PredictedModifierSnapshots.RemoveAll([ModifierID](const FPredictedModifierSnapshot& Snapshot)
				{
					return Snapshot.ModifierID == ModifierID;
				});
			}
		}
	}

	return WasApplied;
}

void USimpleAttributeComponent::ApplyAttributeModifierToTargetServerInitiated(FGuid& ModifierID,
	const TSubclassOf<USimpleAttributeModifier> ModifierClass, USimpleAttributeComponent* ModifierTarget, float Magnitude,
	FInstancedStruct ModifierContext)
{
	ModifierID = FGuid::NewGuid();
	ServerApplyAttributeModifierToTarget(ModifierID, ModifierClass, ModifierTarget, Magnitude, ModifierContext);
}

void USimpleAttributeComponent::ServerApplyAttributeModifierToTarget_Implementation(
	const FGuid ModifierID,
	const TSubclassOf<USimpleAttributeModifier> ModifierClass,
	USimpleAttributeComponent* ModifierTarget,
	const float Magnitude,
	const FInstancedStruct ModifierContext)
{
	if (!ModifierClass || !ModifierTarget)
	{
		return;
	}

	// Check for consolidated stacking - add to existing modifier if one exists
	const USimpleAttributeModifier* ModifierCDO = ModifierClass.GetDefaultObject();
	if (ModifierCDO->StackingConfig.bEnableStacking && ModifierCDO->StackingConfig.StackGroupTag.IsValid())
	{
		USimpleAttributeModifier* ExistingModifier = ModifierTarget->GetActiveModifierInStackGroup(ModifierCDO->StackingConfig.StackGroupTag);
		if (ExistingModifier)
		{
			// Add stack to existing modifier
			ExistingModifier->AddStacks(1);
			return;
		}
	}

	USimpleAttributeModifier* Modifier = GetAttributeModifierInstance(
		ModifierClass,
		ModifierID,
		this,
		ModifierTarget,
		Magnitude,
		ModifierContext,
		true);

	ApplyModifierInstance(Modifier, 1);
}

bool USimpleAttributeComponent::ApplyAttributeModifierToSelf(
	FGuid& ModifierID,
	const TSubclassOf<USimpleAttributeModifier> ModifierClass,
	const float Magnitude, const FInstancedStruct ModifierContext)
{
	return ApplyAttributeModifierToTarget(ModifierID, ModifierClass, this, Magnitude, ModifierContext);
}

bool USimpleAttributeComponent::ApplyAttributeModifierToSelfPredicted(
	FGuid& ModifierID,
	const TSubclassOf<USimpleAttributeModifier> ModifierClass,
	const float Magnitude,
	const FInstancedStruct ModifierContext)
{
	return ApplyAttributeModifierToTargetPredicted(ModifierID, ModifierClass, this, Magnitude, ModifierContext);
}

void USimpleAttributeComponent::ApplyAttributeModifierToSelfServerInitiated(FGuid& ModifierID,
	const TSubclassOf<USimpleAttributeModifier> ModifierClass, const float Magnitude, const FInstancedStruct ModifierContext)
{
	ModifierID = FGuid::NewGuid();
	ServerApplyAttributeModifierToTarget(ModifierID, ModifierClass, this, Magnitude, ModifierContext);
}

void USimpleAttributeComponent::CancelAttributeModifier(const FGuid ModifierID)
{
	for (USimpleAttributeModifier* Modifier : InstancedAttributeModifiers)
	{
		if (Modifier && Modifier->ModifierID == ModifierID)
		{
			Modifier->CancelModifier(FDefaultTags::AttributeModifierCancelled(), FInstancedStruct());
			return;
		}
	}
}

void USimpleAttributeComponent::CancelAttributeModifierPredicted(const FGuid ModifierID)
{
	CancelAttributeModifier(ModifierID);

	if (!HasAuthority())
	{
		ServerCancelAttributeModifier(ModifierID);
	}
}

void USimpleAttributeComponent::CancelAttributeModifierServerInitiated(const FGuid ModifierID)
{
	if (!HasAuthority())
	{
		ServerCancelAttributeModifier(ModifierID);
		return;
	}

	CancelAttributeModifier(ModifierID);
}

void USimpleAttributeComponent::ServerCancelAttributeModifier_Implementation(const FGuid ModifierID)
{
	CancelAttributeModifier(ModifierID);
}

void USimpleAttributeComponent::CancelAttributeModifiersWithTags(const FGameplayTagContainer ModifierTags)
{
	// Iterate a copy: a cancelled modifier is released from the array, and its actions may apply new modifiers.
	const TArray<USimpleAttributeModifier*> Candidates = InstancedAttributeModifiers;
	for (USimpleAttributeModifier* Modifier : Candidates)
	{
		if (Modifier && Modifier->IsActive && Modifier->ModifierTags.HasAny(ModifierTags))
		{
			Modifier->CancelModifier(FDefaultTags::AttributeModifierCancelled(), FInstancedStruct());
		}
	}
}

void USimpleAttributeComponent::CancelAttributeModifiersWithTagsPredicted(const FGameplayTagContainer ModifierTags)
{
	CancelAttributeModifiersWithTags(ModifierTags);

	if (!HasAuthority())
	{
		ServerCancelAttributeModifiersWithTags(ModifierTags);
	}
}

void USimpleAttributeComponent::CancelAttributeModifiersWithTagsServerInitiated(const FGameplayTagContainer ModifierTags)
{
	if (!HasAuthority())
	{
		ServerCancelAttributeModifiersWithTags(ModifierTags);
		return;
	}

	CancelAttributeModifiersWithTags(ModifierTags);
}

void USimpleAttributeComponent::ServerCancelAttributeModifiersWithTags_Implementation(const FGameplayTagContainer ModifierTags)
{
	CancelAttributeModifiersWithTags(ModifierTags);
}

void USimpleAttributeComponent::CancelAttributeModifiersWithClass(TSubclassOf<USimpleAttributeModifier> ModifierClass)
{
	// Iterate a copy: a cancelled modifier is released from the array, and its actions may apply new modifiers.
	const TArray<USimpleAttributeModifier*> Candidates = InstancedAttributeModifiers;
	for (USimpleAttributeModifier* Modifier : Candidates)
	{
		if (Modifier && Modifier->IsActive && Modifier->IsA(ModifierClass))
		{
			Modifier->CancelModifier(FDefaultTags::AttributeModifierCancelled(), FInstancedStruct());
		}
	}
}

void USimpleAttributeComponent::CancelAttributeModifiersWithClassPredicted(TSubclassOf<USimpleAttributeModifier> ModifierClass)
{
	CancelAttributeModifiersWithClass(ModifierClass);

	if (!HasAuthority())
	{
		ServerCancelAttributeModifiersWithClass(ModifierClass);
	}
}

void USimpleAttributeComponent::CancelAttributeModifiersWithClassServerInitiated(TSubclassOf<USimpleAttributeModifier> ModifierClass)
{
	if (!HasAuthority())
	{
		ServerCancelAttributeModifiersWithClass(ModifierClass);
		return;
	}

	CancelAttributeModifiersWithClass(ModifierClass);
}

void USimpleAttributeComponent::ServerCancelAttributeModifiersWithClass_Implementation(TSubclassOf<USimpleAttributeModifier> ModifierClass)
{
	CancelAttributeModifiersWithClass(ModifierClass);
}

bool USimpleAttributeComponent::IsModifierWithTagsActive(const FGameplayTagContainer ModifierTags) const
{
	for (const USimpleAttributeModifier* Modifier : InstancedAttributeModifiers)
	{
		if (Modifier && Modifier->IsActive && Modifier->ModifierTags.HasAny(ModifierTags))
		{
			return true;
		}
	}

	return false;
}

USimpleAttributeModifier* USimpleAttributeComponent::FindModifierInstance(const FGuid& ModifierID) const
{
	for (USimpleAttributeModifier* Modifier : InstancedAttributeModifiers)
	{
		if (Modifier && Modifier->ModifierID == ModifierID)
		{
			return Modifier;
		}
	}
	return nullptr;
}

USimpleAttributeModifier* USimpleAttributeComponent::GetAttributeModifierInstance(const TSubclassOf<USimpleAttributeModifier>& ModifierClass, const FGuid NewModifierID, USimpleAttributeComponent* Instigator, USimpleAttributeComponent* Target, const float Magnitude, const FInstancedStruct Context, const bool DoesReplicate)
{
	if (!ModifierClass)
	{
		SIMPLE_LOG(this, TEXT("[USimpleGameplayAbilityComponent::GetAttributeModifierInstance]: ModifierClass is null!"));
		return nullptr;
	}

	// Check if an instance with this ModifierID already exists (e.g., from client prediction)
	if (USimpleAttributeModifier* ExistingModifier = FindModifierInstance(NewModifierID))
	{
		// Don't reinitialize if already active to preserve state like ActivationTime
		if (!ExistingModifier->IsActive)
		{
			ExistingModifier->InitializeModifier(NewModifierID, Instigator, Target, Magnitude, Context, DoesReplicate);
		}
		return ExistingModifier;
	}

	// No existing instance found, create new one
	USimpleAttributeModifier* ModifierInstance = NewObject<USimpleAttributeModifier>(this, ModifierClass);
	ModifierInstance->OnModifierApplied.AddDynamic(this, &USimpleAttributeComponent::OnAttributeModifierInitiallyApplied);
	ModifierInstance->OnActionStackApplied.AddDynamic(this, &USimpleAttributeComponent::OnAttributeModifierActionStackApplied);
	ModifierInstance->OnAttributeModifierEnded.AddDynamic(this, &USimpleAttributeComponent::OnAttributeModifierEnded);
	ModifierInstance->OnAttributeModifierCancelled.AddDynamic(this, &USimpleAttributeComponent::OnAttributeModifierCancelled);
	InstancedAttributeModifiers.Add(ModifierInstance);

	ModifierInstance->InitializeModifier(NewModifierID, Instigator, Target, Magnitude, Context, DoesReplicate);
	return ModifierInstance;
}

bool USimpleAttributeComponent::ApplyModifierInstance(USimpleAttributeModifier* Modifier, const int32 InitialStackCount)
{
	if (!Modifier)
	{
		return false;
	}

	// Initialize stack count for consolidated stacking
	if (InitialStackCount > 0 && Modifier->StackingConfig.bEnableStacking)
	{
		Modifier->CurrentStackCount = InitialStackCount;
		Modifier->RecalculateMagnitude();
		Modifier->InjectStackCountToScratchpad();
	}

	const bool bWasApplied = Modifier->ApplyModifier();

	// Refused, or instant and therefore already over: nothing keeps the instance alive past this point.
	if (!Modifier->IsActive)
	{
		ReleaseModifierInstance(Modifier);
	}

	return bWasApplied;
}

void USimpleAttributeComponent::ReleaseModifierInstance(USimpleAttributeModifier* Modifier)
{
	// Idempotent: the stop callbacks and ApplyModifierInstance may both get here for the same instance.
	if (Modifier && InstancedAttributeModifiers.Remove(Modifier) > 0)
	{
		Modifier->OnCleanupModifier();
	}
}

/* Attribute Modifier State Callbacks */

FAttributeModifierState USimpleAttributeComponent::MakeModifierState(const USimpleAttributeModifier& ModifierInstance, const EModifierStatus Status)
{
	FAttributeModifierState NewState;
	NewState.ModifierID = ModifierInstance.ModifierID;
	NewState.ModifierContext = ModifierInstance.ModifierContext;
	NewState.ModifierMagnitude = ModifierInstance.ModifierMagnitude;
	NewState.InstigatorAttributeComponent = ModifierInstance.InstigatorAttributeComponent;
	NewState.TargetAttributeComponent = ModifierInstance.TargetAttributeComponent;
	NewState.ModifierClass = ModifierInstance.GetClass();
	NewState.ModifierStatus = Status;
	NewState.ApplicationTimestamp = GetServerTime();

	// Include stacking state
	NewState.StackCount = ModifierInstance.CurrentStackCount > 0 ? ModifierInstance.CurrentStackCount : 1;
	NewState.ScaledMagnitude = ModifierInstance.ScaledMagnitude;
	NewState.StackDurations = ModifierInstance.StackDurations;
	return NewState;
}

void USimpleAttributeComponent::OnAttributeModifierInitiallyApplied(USimpleAttributeModifier* ModifierInstance)
{
	const FAttributeModifierState NewState = MakeModifierState(*ModifierInstance, EModifierStatus::Applied);

	if (!HasAuthority())
	{
		LocalAttributeModifierStates.Add(NewState);
		return;
	}

	// Mark the array element, not the local copy, so the element itself carries its replication ID.
	FAttributeModifierState& AddedState = AuthorityAttributeModifierStates.ModifierStates.Add_GetRef(NewState);
	AuthorityAttributeModifierStates.MarkItemDirty(AddedState);
}

void USimpleAttributeComponent::OnAttributeModifierEnded(USimpleAttributeModifier* ModifierInstance, FGameplayTag EndStatus, FInstancedStruct EndContext)
{
	RecordModifierStopped(ModifierInstance, EModifierStatus::Ended);
}

void USimpleAttributeComponent::OnAttributeModifierCancelled(USimpleAttributeModifier* ModifierInstance, FGameplayTag EndStatus, FInstancedStruct EndContext)
{
	RecordModifierStopped(ModifierInstance, EModifierStatus::Cancelled);
}

void USimpleAttributeComponent::RecordModifierStopped(USimpleAttributeModifier* ModifierInstance, const EModifierStatus TerminalStatus)
{
	if (!ModifierInstance)
	{
		return;
	}

	// Only replicating modifiers are tracked in the state arrays.
	if (ModifierInstance->DoesReplicate())
	{
		const bool bIsAuthority = HasAuthority();
		const FGuid ModifierID = ModifierInstance->ModifierID;
		TArray<FAttributeModifierState>& ModifierStates = bIsAuthority ? AuthorityAttributeModifierStates.ModifierStates : LocalAttributeModifierStates;
		FAttributeModifierState* State = ModifierStates.FindByPredicate([&ModifierID](const FAttributeModifierState& Candidate) { return Candidate.ModifierID == ModifierID; });

		// The server refused the application before it ever became a state. A client that predicted this modifier
		// only rolls its prediction back when it sees a Cancelled state for the ID, so record one.
		if (!State && bIsAuthority && TerminalStatus == EModifierStatus::Cancelled)
		{
			State = &ModifierStates.Add_GetRef(MakeModifierState(*ModifierInstance, EModifierStatus::Cancelled));
		}

		if (State)
		{
			State->ModifierStatus = TerminalStatus;
			State->EndedTimestamp = GetServerTime();
			if (bIsAuthority)
			{
				AuthorityAttributeModifierStates.MarkItemDirty(*State);
			}
		}
	}

	// Stopped for good, tracked or not: unsubscribe it from the event subsystem and let it be collected.
	ReleaseModifierInstance(ModifierInstance);
}

// Modifier Mutation Addition
void USimpleAttributeComponent::OnAttributeModifierActionStackApplied(USimpleAttributeModifier* ModifierInstance, FModifierActionStackResults ActionResult)
{
	FAttributeModifierMutation NewMutation;
	NewMutation.ModifierID = ModifierInstance->ModifierID;
	NewMutation.ModifierClass = ActionResult.ModifierClass;
	NewMutation.ActionStackResult = ActionResult;
	NewMutation.MutationTimestamp = GetServerTime();

	if (!HasAuthority())
	{
		// Find the highest mutation counter for this modifier and increment
		int32 HighestCounter = 0;
		for (const FAttributeModifierMutation& Mutation : LocalAttributeModiferMutations)
		{
			if (Mutation.ModifierID == ModifierInstance->ModifierID && Mutation.MutationCounter > HighestCounter)
			{
				HighestCounter = Mutation.MutationCounter;
			}
		}
		NewMutation.MutationCounter = HighestCounter + 1;

		LocalAttributeModiferMutations.Add(NewMutation);
		return;
	}

	// Find the highest mutation counter for this modifier and increment
	int32 HighestCounter = 0;
	for (const FAttributeModifierMutation& Mutation : AuthorityAttributeModifierMutations.Mutations)
	{
		if (Mutation.ModifierID == ModifierInstance->ModifierID && Mutation.MutationCounter > HighestCounter)
		{
			HighestCounter = Mutation.MutationCounter;
		}
	}
	NewMutation.MutationCounter = HighestCounter + 1;

	FAttributeModifierMutation& AddedRef = AuthorityAttributeModifierMutations.Mutations.Add_GetRef(NewMutation);
	AuthorityAttributeModifierMutations.MarkItemDirty(AddedRef);
}

/* Utility */

bool USimpleAttributeComponent::HasAuthority() const
{
	if (GetOwner())
	{
		return GetOwner()->HasAuthority();
	}

	return false;
}

USimpleTimeSynchronizer* USimpleAttributeComponent::GetTimeSynchronizerComponent_Implementation()
{
	// Default to assuming the owner actor has a time synchronizer component
	return GetOwner()->GetComponentByClass<USimpleTimeSynchronizerExtended>();
}

double USimpleAttributeComponent::GetServerTime()
{
	if (USimpleTimeSynchronizer* TimeSynchronizer = GetTimeSynchronizerComponent())
	{
		return TimeSynchronizer->GetServerTime();
	}

	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0;
	}

	// A client has no GameState until it replicates; its own clock is the best estimate until then.
	const AGameStateBase* GameState = World->GetGameState();
	return GameState ? GameState->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
}

USimpleAttributeHandler* USimpleAttributeComponent::GetStructAttributeHandler(const FGameplayTag AttributeTag, const TSubclassOf<USimpleAttributeHandler> AttributeHandlerClass, EGetInstancedStructResult& OutResult)
{
	return GetStructAttributeHandlerInstance(AttributeTag, AttributeHandlerClass, OutResult);
}

USimpleAttributeHandler* USimpleAttributeComponent::GetStructAttributeHandlerInstance(const FGameplayTag AttributeTag, const TSubclassOf<USimpleAttributeHandler> HandlerClass, EGetInstancedStructResult& OutResult)
{
	// Initialize result to Invalid
	OutResult = EGetInstancedStructResult::Invalid;

	// Check if the struct attribute exists
	if (!HasStructAttribute(AttributeTag))
	{
		SIMPLE_LOG(this, FString::Printf(
					   TEXT(
						   "[USimpleGameplayAbilityComponent::GetStructAttributeHandlerInstance]: Struct Attribute %s not found."),
					   *AttributeTag.ToString()));
		return nullptr;
	}

	// Get the struct attribute to check its type
	FStructAttribute* StructAttribute = GetStructAttribute(AttributeTag);
	if (!StructAttribute)
	{
		SIMPLE_LOG(this, FString::Printf(
					   TEXT(
						   "[USimpleGameplayAbilityComponent::GetStructAttributeHandlerInstance]: Failed to get Struct Attribute %s."),
					   *AttributeTag.ToString()));
		return nullptr;
	}

	// Get or create the handler instance
	USimpleAttributeHandler* HandlerInstance = nullptr;

	for (USimpleAttributeHandler* InstancedHandler : InstancedAttributeHandlers)
	{
		if (InstancedHandler->GetClass() == HandlerClass)
		{
			HandlerInstance = InstancedHandler;
			break;
		}
	}

	if (!HandlerInstance)
	{
		HandlerInstance = NewObject<USimpleAttributeHandler>(this, HandlerClass);
		InstancedAttributeHandlers.Add(HandlerInstance);
	}

	// Check if the handler's StructType matches the attribute's StructType
	if (HandlerInstance->StructType && StructAttribute->StructType)
	{
		if (HandlerInstance->StructType != StructAttribute->StructType)
		{
			SIMPLE_LOG(this, FString::Printf(
						   TEXT(
							   "[USimpleGameplayAbilityComponent::GetStructAttributeHandlerInstance]: StructType mismatch for Attribute %s. Handler expects %s but attribute is %s."),
						   *AttributeTag.ToString(),
						   *HandlerInstance->StructType->GetName(),
						   *StructAttribute->StructType->GetName()));
			return nullptr;
		}
	}
	else if (!StructAttribute->StructType)
	{
		SIMPLE_LOG(this, FString::Printf(
					   TEXT(
						   "[USimpleGameplayAbilityComponent::GetStructAttributeHandlerInstance]: Attribute %s has no StructType set."),
					   *AttributeTag.ToString()));
		return nullptr;
	}

	HandlerInstance->InitializeHandler(this, AttributeTag);

	// All checks passed, set result to Valid
	OutResult = EGetInstancedStructResult::Valid;
	return HandlerInstance;
}

/* Replication */

void USimpleAttributeComponent::ClientOnAttributeModiferStateAdded(const FAttributeModifierState& NewAttributeModiferState)
{
	ClientResolveModifierState(NewAttributeModiferState);

	// Process any pending mutations for this modifier
	ProcessPendingMutations(NewAttributeModiferState.ModifierID);
}

void USimpleAttributeComponent::ClientOnAttributeModifierStateChanged(const FAttributeModifierState& ChangedAttributeModiferState)
{
	ClientResolveModifierState(ChangedAttributeModiferState);
}

void USimpleAttributeComponent::ClientResolveModifierState(const FAttributeModifierState& AuthorityState)
{
	const FGuid ModifierID = AuthorityState.ModifierID;
	const auto MatchesID = [&ModifierID](const FAttributeModifierState& State) { return State.ModifierID == ModifierID; };

	// A local state means this modifier has already run here (predicted, or started by an earlier update).
	const bool bHadLocalState = LocalAttributeModifierStates.ContainsByPredicate(MatchesID);

	// Only an instance that already exists is ever ended or cancelled. Creating one just to stop it would run its
	// end/cancel actions and take back temporary tags for a modifier that never ran here.
	USimpleAttributeModifier* ActiveModifier = FindModifierInstance(ModifierID);
	if (ActiveModifier && !ActiveModifier->IsActive)
	{
		ActiveModifier = nullptr;
	}

	USimpleAttributeComponent* TargetComponent = AuthorityState.TargetAttributeComponent;
	const auto DropPredictionSnapshot = [TargetComponent, &ModifierID]
	{
		if (TargetComponent)
		{
			TargetComponent->PredictedModifierSnapshots.RemoveAll([&ModifierID](const FPredictedModifierSnapshot& Snapshot) { return Snapshot.ModifierID == ModifierID; });
		}
	};

	switch (AuthorityState.ModifierStatus)
	{
		case EModifierStatus::Applied:
			if (ActiveModifier)
			{
				// Running here already: the server confirmed our prediction (or revised a modifier we mirror).
				DropPredictionSnapshot();
			}
			else if (!bHadLocalState)
			{
				// New to this client: run it here too.
				USimpleAttributeModifier* Modifier = GetAttributeModifierInstance(
					AuthorityState.ModifierClass,
					ModifierID,
					AuthorityState.InstigatorAttributeComponent,
					AuthorityState.TargetAttributeComponent,
					AuthorityState.ModifierMagnitude,
					AuthorityState.ModifierContext,
					true);
				ApplyModifierInstance(Modifier, 0);
			}
			// Otherwise it already ran and stopped here; it is never started a second time.
			break;

		case EModifierStatus::Cancelled:
			// The server rejected or cancelled the modifier: roll a prediction back to its snapshot.
			if (TargetComponent)
			{
				const FPredictedModifierSnapshot* Snapshot = TargetComponent->PredictedModifierSnapshots.FindByPredicate(
					[&ModifierID](const FPredictedModifierSnapshot& Candidate) { return Candidate.ModifierID == ModifierID; });
				if (Snapshot)
				{
					const FAttributeSnapshot AttributeSnapshot = Snapshot->AttributeSnapshot;
					DropPredictionSnapshot();
					TargetComponent->RestoreAttributeSnapshot(AttributeSnapshot);
				}
			}
			if (ActiveModifier)
			{
				ActiveModifier->CancelModifier(FDefaultTags::AttributeModifierCancelled(), FInstancedStruct());
			}
			break;

		case EModifierStatus::Ended:
			if (ActiveModifier)
			{
				ActiveModifier->EndModifier(FDefaultTags::AttributeModifierEnded(), FInstancedStruct());
			}
			DropPredictionSnapshot();
			break;
	}

	// The calls above run game code that can add local states, so look the entry up again. The server's state wins.
	if (FAttributeModifierState* LocalState = LocalAttributeModifierStates.FindByPredicate(MatchesID))
	{
		*LocalState = AuthorityState;
	}
	else
	{
		LocalAttributeModifierStates.Add(AuthorityState);
	}
}

void USimpleAttributeComponent::ClientOnAttributeModiferStateRemoved(const FAttributeModifierState& RemovedAttributeModiferState)
{
	const FGuid ModifierID = RemovedAttributeModiferState.ModifierID;
	LocalAttributeModifierStates.RemoveAll([&ModifierID](const FAttributeModifierState& ModifierState)
	{
		return ModifierState.ModifierID == ModifierID;
	});
}

void USimpleAttributeComponent::ClientOnAttributeModifierMutationAdded(const FAttributeModifierMutation& NewModifierMutation)
{
	if (!NewModifierMutation.ModifierClass)
	{
		SIMPLE_LOG(this, FString::Printf(
			TEXT("[USimpleAttributeComponent::ClientOnAttributeModifierMutationAdded]: ModifierClass is null for modifier ID %s"),
			*NewModifierMutation.ModifierID.ToString()));
		return;
	}

	// The local prediction this mutation answers. Both sides produce a modifier's mutations in the same order, so the
	// oldest unresolved local one is the counterpart (the counters themselves drift as resolved entries are removed).
	const FGuid ModifierID = NewModifierMutation.ModifierID;
	int32 PredictedIndex = INDEX_NONE;
	for (int32 Index = 0; Index < LocalAttributeModiferMutations.Num(); ++Index)
	{
		const FAttributeModifierMutation& Candidate = LocalAttributeModiferMutations[Index];
		if (Candidate.ModifierID == ModifierID
			&& (PredictedIndex == INDEX_NONE || Candidate.MutationCounter < LocalAttributeModiferMutations[PredictedIndex].MutationCounter))
		{
			PredictedIndex = Index;
		}
	}

	const FAttributeModifierState* AuthorityState = AuthorityAttributeModifierStates.ModifierStates.FindByPredicate(
		[&ModifierID](const FAttributeModifierState& State)
		{
			return State.ModifierID == ModifierID;
		});

	if (!AuthorityState)
	{
		SIMPLE_LOG(this, FString::Printf(
			TEXT("[USimpleAttributeComponent::ClientOnAttributeModifierMutationAdded]: Modifier with ID %s not found in AuthorityAttributeModifierStates array. Queueing for later processing."),
			*NewModifierMutation.ModifierID.ToString()));

		// Queue this mutation to be processed when the state arrives
		PendingMutationQueue.Add(NewModifierMutation);
		return;
	}

	// Take the prediction out before resolving: resolving runs actions, which is game code.
	FModifierActionStackResults LocalActionStackResults;
	if (PredictedIndex != INDEX_NONE)
	{
		LocalActionStackResults = LocalAttributeModiferMutations[PredictedIndex].ActionStackResult;
		LocalAttributeModiferMutations.RemoveAt(PredictedIndex);
	}

	const bool bHadInstance = FindModifierInstance(ModifierID) != nullptr;
	USimpleAttributeModifier* Modifier = GetAttributeModifierInstance(
		AuthorityState->ModifierClass,
		AuthorityState->ModifierID,
		AuthorityState->InstigatorAttributeComponent,
		AuthorityState->TargetAttributeComponent,
		AuthorityState->ModifierMagnitude,
		AuthorityState->ModifierContext,
		true);
	if (!Modifier)
	{
		return;
	}

	Modifier->OnClientReceivedServerActionsResult(NewModifierMutation.ActionStackResult, LocalActionStackResults);

	// An instance created only to resolve this mutation (the modifier isn't running here) is not kept.
	if (!bHadInstance && !Modifier->IsActive)
	{
		ReleaseModifierInstance(Modifier);
	}
}

void USimpleAttributeComponent::ClientOnFloatAttributeAdded(const FFloatAttribute& NewFloatAttribute)
{
	LocalFloatAttributes.AddUnique(NewFloatAttribute);
	OnFloatAttributeAdded.Broadcast(NewFloatAttribute.AttributeTag, NewFloatAttribute);
}

void USimpleAttributeComponent::ClientOnFloatAttributeChanged(const FFloatAttribute& ChangedFloatAttribute)
{
	FFloatAttribute* LocalFloatAttribute = GetFloatAttribute(ChangedFloatAttribute.AttributeTag);

	if (!LocalFloatAttribute)
	{
		LocalFloatAttributes.AddUnique(ChangedFloatAttribute);
		OnFloatAttributeAdded.Broadcast(ChangedFloatAttribute.AttributeTag, ChangedFloatAttribute);
		return;
	}

	// Fields without a change event are mirrored as they are. The regeneration parameters matter most: a client
	// computes the effective current value from them, so stale ones make it regenerate from the wrong instant.
	LocalFloatAttribute->AttributeName = ChangedFloatAttribute.AttributeName;
	LocalFloatAttribute->Replication = ChangedFloatAttribute.Replication;
	LocalFloatAttribute->CurrentRegenRate = ChangedFloatAttribute.CurrentRegenRate;
	LocalFloatAttribute->bIsRegenerating = ChangedFloatAttribute.bIsRegenerating;
	LocalFloatAttribute->LastRegenParamsUpdateTime_Server = ChangedFloatAttribute.LastRegenParamsUpdateTime_Server;

	float OldFloatValue;
	bool OldBoolValue;

	// Base value changed
	if (LocalFloatAttribute->BaseValue != ChangedFloatAttribute.BaseValue)
	{
		OldFloatValue = LocalFloatAttribute->BaseValue;
		LocalFloatAttribute->BaseValue = ChangedFloatAttribute.BaseValue;
		OnFloatAttributeBaseValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldFloatValue,
		                                           ChangedFloatAttribute.BaseValue);
	}

	// Current value changed
	if (LocalFloatAttribute->CurrentValue != ChangedFloatAttribute.CurrentValue)
	{
		OldFloatValue = LocalFloatAttribute->CurrentValue;
		LocalFloatAttribute->CurrentValue = ChangedFloatAttribute.CurrentValue;
		OnFloatAttributeCurrentValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldFloatValue,
		                                              ChangedFloatAttribute.CurrentValue);
	}

	// Use MinBaseValue changed
	if (LocalFloatAttribute->ValueLimits.UseMinBaseValue != ChangedFloatAttribute.ValueLimits.UseMinBaseValue)
	{
		OldBoolValue = LocalFloatAttribute->ValueLimits.UseMinBaseValue;
		LocalFloatAttribute->ValueLimits.UseMinBaseValue = ChangedFloatAttribute.ValueLimits.UseMinBaseValue;
		OnUseMinBaseValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldBoolValue,
		                                   ChangedFloatAttribute.ValueLimits.UseMinBaseValue);
	}

	// MinBaseValue changed
	if (LocalFloatAttribute->ValueLimits.MinBaseValue != ChangedFloatAttribute.ValueLimits.MinBaseValue)
	{
		OldFloatValue = LocalFloatAttribute->ValueLimits.MinBaseValue;
		LocalFloatAttribute->ValueLimits.MinBaseValue = ChangedFloatAttribute.ValueLimits.MinBaseValue;
		OnFloatAttributeMinBaseValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldFloatValue,
		                                              ChangedFloatAttribute.ValueLimits.MinBaseValue);
	}

	// Use MinCurrentValue changed
	if (LocalFloatAttribute->ValueLimits.UseMinCurrentValue != ChangedFloatAttribute.ValueLimits.UseMinCurrentValue)
	{
		OldBoolValue = LocalFloatAttribute->ValueLimits.UseMinCurrentValue;
		LocalFloatAttribute->ValueLimits.UseMinCurrentValue = ChangedFloatAttribute.ValueLimits.UseMinCurrentValue;
		OnUseMinCurrentValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldBoolValue,
		                                      ChangedFloatAttribute.ValueLimits.UseMinCurrentValue);
	}

	// MinCurrentValue changed
	if (LocalFloatAttribute->ValueLimits.MinCurrentValue != ChangedFloatAttribute.ValueLimits.MinCurrentValue)
	{
		OldFloatValue = LocalFloatAttribute->ValueLimits.MinCurrentValue;
		LocalFloatAttribute->ValueLimits.MinCurrentValue = ChangedFloatAttribute.ValueLimits.MinCurrentValue;
		OnFloatAttributeMinCurrentValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldFloatValue,
		                                                 ChangedFloatAttribute.ValueLimits.MinCurrentValue);
	}

	// Use MaxBaseValue changed
	if (LocalFloatAttribute->ValueLimits.UseMaxBaseValue != ChangedFloatAttribute.ValueLimits.UseMaxBaseValue)
	{
		OldBoolValue = LocalFloatAttribute->ValueLimits.UseMaxBaseValue;
		LocalFloatAttribute->ValueLimits.UseMaxBaseValue = ChangedFloatAttribute.ValueLimits.UseMaxBaseValue;
		OnUseMaxBaseValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldBoolValue,
		                                   ChangedFloatAttribute.ValueLimits.UseMaxBaseValue);
	}

	// MaxBaseValue changed
	if (LocalFloatAttribute->ValueLimits.MaxBaseValue != ChangedFloatAttribute.ValueLimits.MaxBaseValue)
	{
		OldFloatValue = LocalFloatAttribute->ValueLimits.MaxBaseValue;
		LocalFloatAttribute->ValueLimits.MaxBaseValue = ChangedFloatAttribute.ValueLimits.MaxBaseValue;
		OnFloatAttributeMaxBaseValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldFloatValue,
		                                              ChangedFloatAttribute.ValueLimits.MaxBaseValue);
	}

	// Use MaxCurrentValue changed
	if (LocalFloatAttribute->ValueLimits.UseMaxCurrentValue != ChangedFloatAttribute.ValueLimits.UseMaxCurrentValue)
	{
		OldBoolValue = LocalFloatAttribute->ValueLimits.UseMaxCurrentValue;
		LocalFloatAttribute->ValueLimits.UseMaxCurrentValue = ChangedFloatAttribute.ValueLimits.UseMaxCurrentValue;
		OnUseMaxCurrentValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldBoolValue,
		                                      ChangedFloatAttribute.ValueLimits.UseMaxCurrentValue);
	}

	// MaxCurrentValue changed
	if (LocalFloatAttribute->ValueLimits.MaxCurrentValue != ChangedFloatAttribute.ValueLimits.MaxCurrentValue)
	{
		OldFloatValue = LocalFloatAttribute->ValueLimits.MaxCurrentValue;
		LocalFloatAttribute->ValueLimits.MaxCurrentValue = ChangedFloatAttribute.ValueLimits.MaxCurrentValue;
		OnFloatAttributeMaxCurrentValueChanged.Broadcast(ChangedFloatAttribute.AttributeTag, OldFloatValue,
		                                                 ChangedFloatAttribute.ValueLimits.MaxCurrentValue);
	}
}

void USimpleAttributeComponent::ClientOnFloatAttributeRemoved(const FFloatAttribute& RemovedFloatAttribute)
{
	LocalFloatAttributes.Remove(RemovedFloatAttribute);
	OnFloatAttributeRemoved.Broadcast(RemovedFloatAttribute.AttributeTag);
}

void USimpleAttributeComponent::ClientOnStructAttributeAdded(const FStructAttribute& NewStructAttribute)
{
	LocalStructAttributes.AddUnique(NewStructAttribute);
	OnStructAttributeAdded.Broadcast(NewStructAttribute.AttributeTag, NewStructAttribute);
}

void USimpleAttributeComponent::ClientOnStructAttributeChanged(const FStructAttribute& ChangedStructAttribute)
{
	FStructAttribute* LocalStructAttribute = GetStructAttribute(ChangedStructAttribute.AttributeTag);

	if (!LocalStructAttribute)
	{
		LocalStructAttributes.Add(ChangedStructAttribute);
		OnStructAttributeAdded.Broadcast(ChangedStructAttribute.AttributeTag, ChangedStructAttribute);
		return;
	}

	if (LocalStructAttribute->AttributeValue == ChangedStructAttribute.AttributeValue)
	{
		return;
	}

	const FInstancedStruct OldValue = LocalStructAttribute->AttributeValue;
	FGameplayTagContainer ModificationTags = FGameplayTagContainer();
	LocalStructAttribute->AttributeValue = ChangedStructAttribute.AttributeValue;

	if (LocalStructAttribute->StructAttributeHandler)
	{
		EGetInstancedStructResult HandlerResult = EGetInstancedStructResult::Invalid;
		USimpleAttributeHandler* Handler = GetStructAttributeHandlerInstance(ChangedStructAttribute.AttributeTag,
		                                                     LocalStructAttribute->StructAttributeHandler, HandlerResult);
		if (Handler && HandlerResult == EGetInstancedStructResult::Valid)
		{
			ModificationTags = Handler->GetModificationEvents(OldValue, ChangedStructAttribute.AttributeValue);
		}
	}

	OnStructAttributeChanged.Broadcast(ChangedStructAttribute.AttributeTag, OldValue,
	                                   ChangedStructAttribute.AttributeValue, ModificationTags);
}

void USimpleAttributeComponent::ClientOnStructAttributeRemoved(const FStructAttribute& RemovedStructAttribute)
{
	LocalStructAttributes.Remove(RemovedStructAttribute);
	OnStructAttributeRemoved.Broadcast(RemovedStructAttribute.AttributeTag);
}

void USimpleAttributeComponent::ClientOnGameplayTagAdded(const FGameplayTagCounter& NewGameplayTag)
{
	if (LocalGameplayTags.Contains(NewGameplayTag))
	{
		return;
	}

	AddGameplayTag(NewGameplayTag.GameplayTag);
}

void USimpleAttributeComponent::ClientOnGameplayTagRemoved(const FGameplayTagCounter& RemovedGameplayTag)
{
	// The server's removal wins over any local reference count. Only announce a tag that was actually still held
	// here (a local prediction may already have removed and announced it).
	if (LocalGameplayTags.Remove(RemovedGameplayTag) > 0)
	{
		OnGameplayTagRemoved.Broadcast(RemovedGameplayTag.GameplayTag);
	}
}

/* Stack Group Query Functions */

TArray<USimpleAttributeModifier*> USimpleAttributeComponent::GetModifiersInStackGroup(FGameplayTag StackGroupTag) const
{
	TArray<USimpleAttributeModifier*> Result;

	if (!StackGroupTag.IsValid())
		return Result;

	for (USimpleAttributeModifier* Modifier : InstancedAttributeModifiers)
	{
		if (Modifier &&
			Modifier->IsActive &&
			Modifier->bUseStackGroup &&
			Modifier->StackGroupTag.MatchesTagExact(StackGroupTag))
		{
			Result.Add(Modifier);
		}
	}

	return Result;
}

int32 USimpleAttributeComponent::GetModifierStackCountInGroup(FGameplayTag StackGroupTag) const
{
	return GetModifiersInStackGroup(StackGroupTag).Num();
}

USimpleAttributeModifier* USimpleAttributeComponent::GetOldestModifierInGroup(FGameplayTag StackGroupTag) const
{
	TArray<USimpleAttributeModifier*> Modifiers = GetModifiersInStackGroup(StackGroupTag);

	if (Modifiers.Num() == 0)
		return nullptr;

	USimpleAttributeModifier* Oldest = Modifiers[0];
	for (USimpleAttributeModifier* Modifier : Modifiers)
	{
		if (Modifier->GetActivationTime() < Oldest->GetActivationTime())
			Oldest = Modifier;
	}

	return Oldest;
}

USimpleAttributeModifier* USimpleAttributeComponent::GetNewestModifierInGroup(FGameplayTag StackGroupTag) const
{
	TArray<USimpleAttributeModifier*> Modifiers = GetModifiersInStackGroup(StackGroupTag);

	if (Modifiers.Num() == 0)
		return nullptr;

	USimpleAttributeModifier* Newest = Modifiers[0];
	for (USimpleAttributeModifier* Modifier : Modifiers)
	{
		if (Modifier->GetActivationTime() > Newest->GetActivationTime())
			Newest = Modifier;
	}

	return Newest;
}

/* Class-based Stack Group Implementation */

TArray<USimpleAttributeModifier*> USimpleAttributeComponent::GetModifiersByClass(TSubclassOf<USimpleAttributeModifier> ModifierClass) const
{
	TArray<USimpleAttributeModifier*> Result;

	if (!ModifierClass)
		return Result;

	for (USimpleAttributeModifier* Modifier : InstancedAttributeModifiers)
	{
		if (Modifier &&
			Modifier->IsActive &&
			Modifier->GetClass() == ModifierClass)
		{
			Result.Add(Modifier);
		}
	}

	return Result;
}

int32 USimpleAttributeComponent::GetModifierCountByClass(TSubclassOf<USimpleAttributeModifier> ModifierClass) const
{
	return GetModifiersByClass(ModifierClass).Num();
}

USimpleAttributeModifier* USimpleAttributeComponent::GetOldestModifierByClass(TSubclassOf<USimpleAttributeModifier> ModifierClass) const
{
	TArray<USimpleAttributeModifier*> Modifiers = GetModifiersByClass(ModifierClass);

	if (Modifiers.Num() == 0)
		return nullptr;

	USimpleAttributeModifier* Oldest = Modifiers[0];
	for (USimpleAttributeModifier* Modifier : Modifiers)
	{
		if (Modifier->GetActivationTime() < Oldest->GetActivationTime())
			Oldest = Modifier;
	}

	return Oldest;
}

USimpleAttributeModifier* USimpleAttributeComponent::GetNewestModifierByClass(TSubclassOf<USimpleAttributeModifier> ModifierClass) const
{
	TArray<USimpleAttributeModifier*> Modifiers = GetModifiersByClass(ModifierClass);

	if (Modifiers.Num() == 0)
		return nullptr;

	USimpleAttributeModifier* Newest = Modifiers[0];
	for (USimpleAttributeModifier* Modifier : Modifiers)
	{
		if (Modifier->GetActivationTime() > Newest->GetActivationTime())
			Newest = Modifier;
	}

	return Newest;
}

/* Consolidated Stacking Implementation */

USimpleAttributeModifier* USimpleAttributeComponent::GetActiveModifierInStackGroup(FGameplayTag StackGroupTag) const
{
	if (!StackGroupTag.IsValid())
	{
		return nullptr;
	}

	// For consolidated stacking, look for a modifier that uses the new stacking system
	for (USimpleAttributeModifier* Modifier : InstancedAttributeModifiers)
	{
		if (Modifier && Modifier->IsActive && Modifier->UsesConsolidatedStacking())
		{
			if (Modifier->StackingConfig.StackGroupTag == StackGroupTag)
			{
				return Modifier;
			}
		}
	}

	return nullptr;
}

bool USimpleAttributeComponent::AddStacksToModifier(
	FGuid& ModifierID,
	TSubclassOf<USimpleAttributeModifier> ModifierClass,
	USimpleAttributeComponent* ModifierTarget,
	int32 StacksToAdd,
	FInstancedStruct ModifierContext)
{
	if (!ModifierClass || !ModifierTarget || StacksToAdd <= 0)
	{
		return false;
	}

	// Get the CDO to check stacking config
	const USimpleAttributeModifier* ModifierCDO = ModifierClass.GetDefaultObject();
	if (!ModifierCDO->StackingConfig.bEnableStacking)
	{
		SIMPLE_LOG(this, FString::Printf(TEXT("AddStacksToModifier: Modifier %s doesn't use consolidated stacking"), *ModifierClass->GetName()));
		return false;
	}

	// Check if there's an existing modifier - use tag-based or class-based grouping
	USimpleAttributeModifier* ExistingModifier = nullptr;
	if (ModifierCDO->StackingConfig.StackGroupTag.IsValid())
	{
		ExistingModifier = ModifierTarget->GetActiveModifierInStackGroup(ModifierCDO->StackingConfig.StackGroupTag);
	}
	else
	{
		// Class-based grouping - get the first active modifier of this class
		TArray<USimpleAttributeModifier*> ExistingModifiers = ModifierTarget->GetModifiersByClass(ModifierClass);
		if (ExistingModifiers.Num() > 0)
		{
			ExistingModifier = ExistingModifiers[0];
		}
	}

	if (ExistingModifier)
	{
		// Add stacks to existing modifier
		ModifierID = ExistingModifier->ModifierID;
		return ExistingModifier->AddStacks(StacksToAdd);
	}

	// No existing modifier, create a new one with initial stack count
	ModifierID = FGuid::NewGuid();
	USimpleAttributeModifier* NewModifier = GetAttributeModifierInstance(
		ModifierClass,
		ModifierID,
		this,
		ModifierTarget,
		ModifierCDO->ModifierMagnitude,
		ModifierContext,
		true // replicate
	);

	// Apply the modifier with its initial stack count
	return ApplyModifierInstance(NewModifier, StacksToAdd);
}

bool USimpleAttributeComponent::RemoveStacksFromModifier(FGameplayTag StackGroupTag, int32 StacksToRemove)
{
	if (!StackGroupTag.IsValid() || StacksToRemove <= 0)
	{
		return false;
	}

	USimpleAttributeModifier* ExistingModifier = GetActiveModifierInStackGroup(StackGroupTag);
	if (!ExistingModifier)
	{
		SIMPLE_LOG(this, FString::Printf(TEXT("RemoveStacksFromModifier: No active modifier found in stack group %s"), *StackGroupTag.ToString()));
		return false;
	}

	return ExistingModifier->RemoveStacks(StacksToRemove);
}

bool USimpleAttributeComponent::RemoveStacksFromModifierByClass(TSubclassOf<USimpleAttributeModifier> ModifierClass, int32 StacksToRemove)
{
	if (!ModifierClass || StacksToRemove <= 0)
	{
		return false;
	}

	TArray<USimpleAttributeModifier*> ExistingModifiers = GetModifiersByClass(ModifierClass);
	if (ExistingModifiers.Num() == 0)
	{
		SIMPLE_LOG(this, FString::Printf(TEXT("RemoveStacksFromModifierByClass: No active modifier found for class %s"), *ModifierClass->GetName()));
		return false;
	}

	// Remove from the first (oldest) modifier
	return ExistingModifiers[0]->RemoveStacks(StacksToRemove);
}

void USimpleAttributeComponent::OnModifierStackCountChanged(USimpleAttributeModifier* Modifier)
{
	if (!Modifier || !HasAuthority())
	{
		return;
	}

	// Find and update the modifier state for replication
	for (FAttributeModifierState& State : AuthorityAttributeModifierStates.ModifierStates)
	{
		if (State.ModifierID == Modifier->ModifierID)
		{
			State.StackCount = Modifier->CurrentStackCount;
			State.ScaledMagnitude = Modifier->ScaledMagnitude;
			State.StackDurations = Modifier->StackDurations;
			AuthorityAttributeModifierStates.MarkItemDirty(State);
			return;
		}
	}
}

FAttributeSnapshot USimpleAttributeComponent::CaptureAttributeSnapshot() const
{
	FAttributeSnapshot Snapshot;
	Snapshot.FloatAttributes = HasAuthority() ? AuthorityFloatAttributes.Attributes : LocalFloatAttributes;
    if (HasAuthority()) Snapshot.FloatAttributes.Append(AuthorityOwnerOnlyFloatAttributes.Attributes);
	Snapshot.StructAttributes = HasAuthority() ? AuthorityStructAttributes.Attributes : LocalStructAttributes;
	Snapshot.GameplayTags = HasAuthority() ? AuthorityGameplayTags.Tags : LocalGameplayTags;
	return Snapshot;
}

void USimpleAttributeComponent::RestoreAttributeSnapshot(const FAttributeSnapshot& Snapshot)
{
	if (HasAuthority())
	{
		SIMPLE_LOG(this, TEXT("[USimpleAttributeComponent::RestoreAttributeSnapshot]: Attempting to restore snapshot on server - this should only happen on clients!"));
		return;
	}

	// Restore float attributes
	for (const FFloatAttribute& SnapshotAttr : Snapshot.FloatAttributes)
	{
		FFloatAttribute* LocalAttr = GetFloatAttribute(SnapshotAttr.AttributeTag);
		if (LocalAttr)
		{
			const float OldBaseValue = LocalAttr->BaseValue;
			const float OldCurrentValue = LocalAttr->CurrentValue;

			*LocalAttr = SnapshotAttr;

			// Fire events for changes
			if (OldBaseValue != SnapshotAttr.BaseValue)
			{
				OnFloatAttributeBaseValueChanged.Broadcast(SnapshotAttr.AttributeTag, SnapshotAttr.BaseValue, OldBaseValue);
			}
			if (OldCurrentValue != SnapshotAttr.CurrentValue)
			{
				OnFloatAttributeCurrentValueChanged.Broadcast(SnapshotAttr.AttributeTag, SnapshotAttr.CurrentValue, OldCurrentValue);
			}
		}
	}

	// Restore struct attributes
	for (const FStructAttribute& SnapshotAttr : Snapshot.StructAttributes)
	{
		FStructAttribute* LocalAttr = GetStructAttribute(SnapshotAttr.AttributeTag);
		if (LocalAttr)
		{
			const FInstancedStruct OldValue = LocalAttr->AttributeValue;
			*LocalAttr = SnapshotAttr;

			FGameplayTagContainer ModificationTags;
			if (LocalAttr->StructAttributeHandler)
			{
				EGetInstancedStructResult HandlerResult = EGetInstancedStructResult::Invalid;
				USimpleAttributeHandler* Handler = GetStructAttributeHandlerInstance(SnapshotAttr.AttributeTag, LocalAttr->StructAttributeHandler, HandlerResult);
				if (Handler && HandlerResult == EGetInstancedStructResult::Valid)
				{
					ModificationTags = Handler->GetModificationEvents(OldValue, SnapshotAttr.AttributeValue);
				}
			}

			if (OldValue != SnapshotAttr.AttributeValue)
			{
				OnStructAttributeChanged.Broadcast(SnapshotAttr.AttributeTag, OldValue, SnapshotAttr.AttributeValue, ModificationTags);
			}
		}
	}

	// Restore gameplay tags
	LocalGameplayTags = Snapshot.GameplayTags;
}

void USimpleAttributeComponent::ProcessPendingMutations(FGuid ModifierID)
{
	TArray<FAttributeModifierMutation> MutationsToProcess;

	// Find all pending mutations for this modifier
	for (int32 i = PendingMutationQueue.Num() - 1; i >= 0; i--)
	{
		if (PendingMutationQueue[i].ModifierID == ModifierID)
		{
			MutationsToProcess.Add(PendingMutationQueue[i]);
			PendingMutationQueue.RemoveAt(i);
		}
	}

	// Process each queued mutation
	for (const FAttributeModifierMutation& Mutation : MutationsToProcess)
	{
		ClientOnAttributeModifierMutationAdded(Mutation);
	}
}

void USimpleAttributeComponent::CleanupOldModifierStates()
{
	const double CurrentTime = GetServerTime();

	// Clean up old authority states (server only)
	if (HasAuthority())
	{
		// Remove ended/cancelled states that are older than retention time
		const int32 RemovedStates = AuthorityAttributeModifierStates.ModifierStates.RemoveAll([CurrentTime](const FAttributeModifierState& State)
		{
			return (State.ModifierStatus == EModifierStatus::Ended || State.ModifierStatus == EModifierStatus::Cancelled)
				&& (CurrentTime - State.EndedTimestamp) > MaxStateRetentionTime;
		});
		if (RemovedStates > 0)
		{
			AuthorityAttributeModifierStates.MarkArrayDirty();
		}

		// Clean up old mutations
		const int32 RemovedMutations = AuthorityAttributeModifierMutations.Mutations.RemoveAll([CurrentTime](const FAttributeModifierMutation& Mutation)
		{
			return (CurrentTime - Mutation.MutationTimestamp) > MaxStateRetentionTime;
		});
		if (RemovedMutations > 0)
		{
			AuthorityAttributeModifierMutations.MarkArrayDirty();
		}
	}
	else
	{
		// Clean up local states (client only)
		for (int32 i = LocalAttributeModifierStates.Num() - 1; i >= 0; i--)
		{
			const FAttributeModifierState& State = LocalAttributeModifierStates[i];

			if ((State.ModifierStatus == EModifierStatus::Ended || State.ModifierStatus == EModifierStatus::Cancelled) &&
				(CurrentTime - State.EndedTimestamp) > MaxStateRetentionTime)
			{
				LocalAttributeModifierStates.RemoveAt(i);
			}
		}

		// Clean up local mutations
		for (int32 i = LocalAttributeModiferMutations.Num() - 1; i >= 0; i--)
		{
			const FAttributeModifierMutation& Mutation = LocalAttributeModiferMutations[i];

			if ((CurrentTime - Mutation.MutationTimestamp) > MaxStateRetentionTime)
			{
				LocalAttributeModiferMutations.RemoveAt(i);
			}
		}
	}

	// Clean up old predicted snapshots (client only)
	if (!HasAuthority())
	{
		// Remove snapshots older than retention time
		for (int32 i = PredictedModifierSnapshots.Num() - 1; i >= 0; i--)
		{
			if ((CurrentTime - PredictedModifierSnapshots[i].SnapshotTimestamp) > MaxStateRetentionTime)
			{
				PredictedModifierSnapshots.RemoveAt(i);
			}
		}

		// If we still have too many snapshots, remove oldest
		while (PredictedModifierSnapshots.Num() > MaxPredictedSnapshots)
		{
			// Find and remove oldest snapshot
			int32 OldestIndex = 0;
			double OldestTime = PredictedModifierSnapshots[0].SnapshotTimestamp;

			for (int32 i = 1; i < PredictedModifierSnapshots.Num(); i++)
			{
				if (PredictedModifierSnapshots[i].SnapshotTimestamp < OldestTime)
				{
					OldestTime = PredictedModifierSnapshots[i].SnapshotTimestamp;
					OldestIndex = i;
				}
			}

			PredictedModifierSnapshots.RemoveAt(OldestIndex);
		}
	}
}

void USimpleAttributeComponent::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	DOREPLIFETIME(USimpleAttributeComponent, AuthorityGameplayTags);
	DOREPLIFETIME(USimpleAttributeComponent, AuthorityFloatAttributes);
    DOREPLIFETIME_CONDITION(USimpleAttributeComponent, AuthorityOwnerOnlyFloatAttributes, COND_OwnerOnly);
	DOREPLIFETIME(USimpleAttributeComponent, AuthorityStructAttributes);
	DOREPLIFETIME(USimpleAttributeComponent, AuthorityAttributeModifierStates);
	DOREPLIFETIME(USimpleAttributeComponent, AuthorityAttributeModifierMutations);

	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
}

/* ISimpleEventReplicator Interface Implementation */

void USimpleAttributeComponent::SendEvent(
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	FInstancedStruct Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(FGuid(), EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::SendEventToServer(
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	FInstancedStruct Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	if (HasAuthority())
	{
		SendEvent(EventTag, DomainTag, Payload, Sender, ListenerFilter);
		return;
	}

	const FGuid EventID = FGuid::NewGuid();
	ServerOnlySendEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::SendEventToClient(
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	FInstancedStruct Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	if (!HasAuthority())
	{
		SIMPLE_LOG(this, TEXT("[USimpleAttributeComponent::SendEventToClient]: Called on client, ignoring!"));
		return;
	}

	const FGuid EventID = FGuid::NewGuid();
	ClientSendEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::SendEventToAllClients(
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	FInstancedStruct Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	// Generate unique EventID for deduplication
	const FGuid EventID = FGuid::NewGuid();

	// Send locally first
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);

	if (!HasAuthority())
	{
		// Request server to multicast
		ServerSendEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
		return;
	}

	// Server multicasts to all clients
	MulticastSendEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::ServerSendEvent_Implementation(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	// Broadcast locally on server
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);

	// Also multicast to all clients
	MulticastSendEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::ServerOnlySendEvent_Implementation(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::ClientSendEvent_Implementation(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::MulticastSendEvent_Implementation(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleAttributeComponent::ProcessIncomingEvent(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Purely local events carry no EventID and are never duplicates; deduplicating the empty ID would drop every
	// later local event until the next cleanup.
	if (EventID.IsValid())
	{
		// Already processed here (we sent it locally and the server's multicast echoed it back).
		if (EventIDTimestamps.Contains(EventID))
		{
			return;
		}
		EventIDTimestamps.Add(EventID, World->GetTimeSeconds());
	}

	// Get the SimpleEventSubsystem and dispatch the event
	if (UGameInstance* GameInstance = World->GetGameInstance())
	{
		if (USimpleEventSubsystem* EventSubsystem = GameInstance->GetSubsystem<USimpleEventSubsystem>())
		{
			EventSubsystem->SendEvent(EventTag, DomainTag, Payload, Sender, ListenerFilter);
		}
	}
}

void USimpleAttributeComponent::CleanupOldEventIDs()
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// An echo arrives within a round trip; 30 seconds is far beyond any deliverable duplicate.
	const double Cutoff = World->GetTimeSeconds() - 30.0;
	for (auto It = EventIDTimestamps.CreateIterator(); It; ++It)
	{
		if (It.Value() < Cutoff)
		{
			It.RemoveCurrent();
		}
	}
}
