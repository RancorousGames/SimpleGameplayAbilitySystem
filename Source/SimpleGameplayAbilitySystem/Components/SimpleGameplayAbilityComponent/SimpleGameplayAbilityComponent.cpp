#include "SimpleGameplayAbilityComponent.h"

#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Engine/NetConnection.h"
#include "Net/UnrealNetwork.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeComponent.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleTimeSynchronizerComponent/ExtendedTimeSynchronizer/SimpleTimeSynchronizerExtended.h"
#include "SimpleGameplayAbilitySystem/SimpleAbility/SimpleGameplayAbility/SimpleGameplayAbility.h"
#include "SimpleGameplayAbilitySystem/DataAssets/AbilitySet/SimpleAbilitySet.h"
#include "SimpleGameplayAbilitySystem/DefaultTags/DefaultTags.h"
#include "SimpleGameplayAbilitySystem/Module/SimpleGameplayAbilitySystem.h"
#include "SimpleGameplayAbilitySystem/SimpleEventSubsystem/SimpleEventSubsystem.h"
#include "TimerManager.h"

using enum EAbilityStatus;

namespace
{
	/** Steps a CustomStructureParam wildcard pin off the Blueprint VM stack and copies it into an FInstancedStruct. */
	FInstancedStruct StepWildcardContext(FFrame& Stack)
	{
		Stack.MostRecentProperty = nullptr;
		Stack.MostRecentPropertyAddress = nullptr;
		Stack.StepCompiledIn<FStructProperty>(nullptr);
		const uint8* StructMemory = Stack.MostRecentPropertyAddress;
		const FStructProperty* StructProperty = CastField<FStructProperty>(Stack.MostRecentProperty);

		FInstancedStruct Context;
		if (StructProperty && StructMemory)
		{
			Context.InitializeAs(StructProperty->Struct, StructMemory);
		}
		return Context;
	}
}

USimpleGameplayAbilityComponent::USimpleGameplayAbilityComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	AvatarActor = nullptr;
}

void USimpleGameplayAbilityComponent::BeginPlay()
{
	UActorComponent::BeginPlay();

	SetIsReplicated(true);

	// Start periodic EventID cleanup timer (every 30 seconds)
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			EventIDCleanupTimerHandle,
			this,
			&USimpleGameplayAbilityComponent::CleanupOldEventIDs,
			30.0f,
			true
		);
	}

	if (HasAuthority())
	{
		// Grant abilities from ability sets
		for (USimpleAbilitySet* AbilitySet : AbilitySets)
		{
			for (const TSubclassOf<USimpleGameplayAbility> AbilityClass : AbilitySet->AbilitiesToGrant)
			{
				GrantAbility(AbilityClass);
			}
		}

		// Periodically prune cooldowns and terminal ability states, even without further activations.
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				CooldownCleanupTimerHandle,
				this,
				&USimpleGameplayAbilityComponent::CleanupExpiredCooldowns,
				1.0f,
				true
			);
		}

		return;
	}

	// Delegates called on the client to handle replicated data
	AuthorityAbilityStates.OnStateAdded.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnAbilityStateAdded);
	AuthorityAbilityStates.OnStateChanged.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnAbilityStateChanged);
	AuthorityAbilityStates.OnStateRemoved.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnAbilityStateRemoved);
	AuthorityAbilitySnapshots.OnSnapshotAdded.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnAbilitySnapshotAdded);
	AuthorityAbilitySnapshots.OnSnapshotRemoved.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnAbilitySnapshotRemoved);

	// Cooldown replication delegates
	AuthorityCooldowns.OnCooldownAdded.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnCooldownStateAdded);
	AuthorityCooldowns.OnCooldownChanged.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnCooldownStateChanged);
	AuthorityCooldowns.OnCooldownRemoved.BindUObject(this, &USimpleGameplayAbilityComponent::ClientOnCooldownStateRemoved);

	// An actor's initial replication is applied before BeginPlay, i.e. before the delegates above existed, so whatever
	// was already in the containers (an actor becoming relevant mid-ability, a late joiner) never raised a callback.
	// Catch up one tick later, once every component of the owner has begun play.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]
		{
			ClientCatchUpInitialReplication();
		}));
	}
}

void USimpleGameplayAbilityComponent::ClientCatchUpInitialReplication()
{
	if (HasAuthority() || !HasBegunPlay())
	{
		return;
	}

	// Copies: resolving a state runs game code.
	const TArray<FAbilityState> InitialStates = AuthorityAbilityStates.AbilityStates;
	for (const FAbilityState& State : InitialStates)
	{
		if (State.AbilityStatus == ActivationSuccess)
		{
			// Still running on the server: run it here too (a no-op if a callback delivered it in the meantime).
			ResolveLocalAbilityState(State);
		}
		else if (!FindLocalAbilityState(State.AbilityID))
		{
			// Already over before we could see it: remember it so later revisions are recognised, but don't replay it.
			LocalAbilityStates.Add(State);
		}
	}

	const TArray<FCooldownState> InitialCooldowns = AuthorityCooldowns.Cooldowns;
	for (const FCooldownState& Cooldown : InitialCooldowns)
	{
		ClientOnCooldownStateAdded(Cooldown);
	}
}

void USimpleGameplayAbilityComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(EventIDCleanupTimerHandle);
		World->GetTimerManager().ClearTimer(CooldownCleanupTimerHandle);
	}

	// Cancel whatever is still running. Iterate a copy: cancelling a MultipleInstances ability removes it from the array.
	const TArray<USimpleGameplayAbility*> AbilitiesToCancel = InstancedAbilities;
	for (USimpleGameplayAbility* Ability : AbilitiesToCancel)
	{
		if (Ability && Ability->IsActive)
		{
			Ability->CancelAbility(FGameplayTag::EmptyTag, FInstancedStruct());
		}
	}

	// Clear collections
	InstancedAbilities.Empty();

	Super::EndPlay(EndPlayReason);
}

/* ISimpleEventReplicator Interface Implementation */

void USimpleGameplayAbilityComponent::SendEvent(
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	FInstancedStruct Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(FGuid(), EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleGameplayAbilityComponent::SendEventToServer(
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

void USimpleGameplayAbilityComponent::SendEventToClient(
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	FInstancedStruct Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	if (!HasAuthority())
	{
		SIMPLE_LOG(this, TEXT("[USimpleGameplayAbilityComponent::SendEventToClient]: Called on client, ignoring!"));
		return;
	}

	const FGuid EventID = FGuid::NewGuid();
	ClientSendEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleGameplayAbilityComponent::SendEventToAllClients(
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

void USimpleGameplayAbilityComponent::ServerSendEvent_Implementation(
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

void USimpleGameplayAbilityComponent::ServerOnlySendEvent_Implementation(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleGameplayAbilityComponent::ClientSendEvent_Implementation(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleGameplayAbilityComponent::MulticastSendEvent_Implementation(
	const FGuid& EventID,
	FGameplayTag EventTag,
	FGameplayTag DomainTag,
	const FInstancedStruct& Payload,
	UObject* Sender,
	const TArray<UObject*>& ListenerFilter)
{
	ProcessIncomingEvent(EventID, EventTag, DomainTag, Payload, Sender, ListenerFilter);
}

void USimpleGameplayAbilityComponent::ProcessIncomingEvent(
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

void USimpleGameplayAbilityComponent::CleanupOldEventIDs()
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

/* Ability Functions */

// The Blueprint thunks and the context-less overloads all funnel into the three FInstancedStruct overloads below.

bool USimpleGameplayAbilityComponent::ActivateAbility(const TSubclassOf<USimpleGameplayAbility> AbilityClass, FGuid& AbilityID)
{
	return ActivateAbilityWithContext(AbilityClass, FInstancedStruct(), AbilityID);
}

DEFINE_FUNCTION(USimpleGameplayAbilityComponent::execActivateAbilityWithContext)
{
	P_GET_OBJECT(UClass, AbilityClass);
	FInstancedStruct AbilityContext = StepWildcardContext(Stack);
	P_GET_STRUCT_REF(FGuid, AbilityID);
	P_FINISH;

	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = P_THIS->ActivateAbilityWithContext(TSubclassOf<USimpleGameplayAbility>(AbilityClass), MoveTemp(AbilityContext), AbilityID);
	P_NATIVE_END;
}

// C++ only version that accepts FInstancedStruct directly
bool USimpleGameplayAbilityComponent::ActivateAbilityWithContext(
	TSubclassOf<USimpleGameplayAbility> AbilityClass,
	FInstancedStruct AbilityContext,
	FGuid& AbilityID)
{
	AbilityID = FGuid::NewGuid();
	return ActivateAbilityInternal(AbilityID, AbilityClass, AbilityContext, false, GetServerTime(), EAbilityNetworkRole::Client); // We're not tracking state so ActivatedOn doesn't matter
}

bool USimpleGameplayAbilityComponent::ActivateAbilityPredicted(TSubclassOf<USimpleGameplayAbility> AbilityClass, FGuid& AbilityID)
{
	return ActivateAbilityPredictedWithContext(AbilityClass, FInstancedStruct(), AbilityID);
}

DEFINE_FUNCTION(USimpleGameplayAbilityComponent::execActivateAbilityPredictedWithContext)
{
	P_GET_OBJECT(UClass, AbilityClass);
	FInstancedStruct AbilityContext = StepWildcardContext(Stack);
	P_GET_STRUCT_REF(FGuid, AbilityID);
	P_FINISH;

	P_NATIVE_BEGIN;
	*static_cast<bool*>(RESULT_PARAM) = P_THIS->ActivateAbilityPredictedWithContext(TSubclassOf<USimpleGameplayAbility>(AbilityClass), MoveTemp(AbilityContext), AbilityID);
	P_NATIVE_END;
}

// C++ only version that accepts FInstancedStruct directly
bool USimpleGameplayAbilityComponent::ActivateAbilityPredictedWithContext(
	TSubclassOf<USimpleGameplayAbility> AbilityClass,
	FInstancedStruct AbilityContext,
	FGuid& AbilityID)
{
	AbilityID = FGuid::NewGuid();
	// One stamp for the prediction and the request, so both sides schedule the activation from the same instant.
	const double ActivationTime = GetServerTime();
	const bool bIsAuthority = HasAuthority();
	const bool WasActivated = ActivateAbilityInternal(
		AbilityID,
		AbilityClass,
		AbilityContext,
		true,
		ActivationTime,
		bIsAuthority ? EAbilityNetworkRole::ListenServer : EAbilityNetworkRole::Client);

	// A prediction that failed locally is never sent: the server doesn't need to hear about it.
	if (WasActivated && !bIsAuthority)
	{
		ServerActivateAbility(AbilityID, AbilityClass, AbilityContext, ActivationTime);
	}

	return WasActivated;
}

void USimpleGameplayAbilityComponent::ActivateAbilityServerInitiated(TSubclassOf<USimpleGameplayAbility> AbilityClass, FGuid& AbilityID)
{
	ActivateAbilityServerInitiatedWithContext(AbilityClass, FInstancedStruct(), AbilityID);
}

DEFINE_FUNCTION(USimpleGameplayAbilityComponent::execActivateAbilityServerInitiatedWithContext)
{
	P_GET_OBJECT(UClass, AbilityClass);
	FInstancedStruct AbilityContext = StepWildcardContext(Stack);
	P_GET_STRUCT_REF(FGuid, AbilityID);
	P_FINISH;

	P_NATIVE_BEGIN;
	P_THIS->ActivateAbilityServerInitiatedWithContext(TSubclassOf<USimpleGameplayAbility>(AbilityClass), MoveTemp(AbilityContext), AbilityID);
	P_NATIVE_END;
}

// C++ only version that accepts FInstancedStruct directly
void USimpleGameplayAbilityComponent::ActivateAbilityServerInitiatedWithContext(
	TSubclassOf<USimpleGameplayAbility> AbilityClass,
	FInstancedStruct AbilityContext,
	FGuid& AbilityID)
{
	AbilityID = FGuid::NewGuid();

	if (HasAuthority())
	{
		ActivateAbilityInternal(AbilityID, AbilityClass, AbilityContext, true, GetServerTime(), EAbilityNetworkRole::ListenServer);
		return;
	}

	ServerActivateAbility(AbilityID, AbilityClass, AbilityContext, GetServerTime());
}

bool USimpleGameplayAbilityComponent::ActivateAbilityInternal(
	const FGuid AbilityID,
	const TSubclassOf<USimpleGameplayAbility>& AbilityClass,
	const FInstancedStruct& AbilityContext,
	const bool ShouldTrackState,
	const double ActivationTime, const EAbilityNetworkRole ActivatedOn)
{
	if (!AbilityClass)
	{
		SIMPLE_LOG(this, TEXT("[USimpleGameplayAbilityComponent::ActivateAbilityInternal]: AbilityClass is null!"));
		return false;
	}

	if (!GrantedAbilities.Contains(AbilityClass) && AbilityClass->GetDefaultObject<USimpleGameplayAbility>()->RequireGrantToActivate)
	{
		SIMPLE_LOG(this, FString::Printf(TEXT("[USimpleGameplayAbilityComponent::ActivateAbility]: Ability %s is not granted!"), *AbilityClass->GetName()));
		if (ShouldTrackState)
		{
			RecordRejectedActivation(AbilityID, AbilityClass, AbilityContext, ActivationTime, ActivatedOn, EAbilityActivationFailureReason::NotGranted);
		}
		return false;
	}

	// Check cooldown
	if (IsAbilityOnCooldown(AbilityClass))
	{
		SIMPLE_LOG(this, FString::Printf(TEXT("[USimpleGameplayAbilityComponent::ActivateAbility]: Ability %s is on cooldown!"), *AbilityClass->GetName()));
		if (ShouldTrackState)
		{
			RecordRejectedActivation(AbilityID, AbilityClass, AbilityContext, ActivationTime, ActivatedOn, EAbilityActivationFailureReason::OnCooldown);
		}
		return false;
	}

	USimpleGameplayAbility* AbilityInstance = GetAbilityInstanceByClass(AbilityClass);
	bool bPrevalidatedReactivation = false;

	if (AbilityInstance->InstancingPolicy == EAbilityInstancingPolicy::SingleInstance && AbilityInstance->IsActive)
	{
		// Admission checks run while the predecessor still exists.
		if (!AbilityInstance->PreflightReactivation(AbilityContext))
		{
			if (ShouldTrackState)
			{
				RecordRejectedActivation(AbilityID, AbilityClass, AbilityContext, ActivationTime,
					ActivatedOn, EAbilityActivationFailureReason::ReactivationRefused);
			}
			return false;
		}
		if (!AbilityInstance->HandleReactivationWhileActive(AbilityContext, false))
		{
			SIMPLE_LOG(this, FString::Printf(TEXT("[USimpleGameplayAbilityComponent::ActivateAbility]: Active instance of %s rejected reactivation."), *AbilityClass->GetName()));

			if (ShouldTrackState)
			{
				RecordRejectedActivation(AbilityID, AbilityClass, AbilityContext, ActivationTime, ActivatedOn, EAbilityActivationFailureReason::ReactivationRefused);
			}
			return false;
		}

		if (AbilityInstance->IsActive)
		{
			AbilityInstance->CancelAbility(FGameplayTag::EmptyTag, FInstancedStruct());
		}
		bPrevalidatedReactivation = true;
	}

	if (ShouldTrackState)
	{
		FAbilityState NewState;
		NewState.AbilityID = AbilityID;
		NewState.AbilityClass = AbilityClass;
		NewState.AbilityStatus = PreActivation;
		NewState.ActivatedOn = ActivatedOn;
		NewState.ActivationTimeStamp = ActivationTime;
		NewState.ActivationContext = AbilityContext;

		// Only clients have non-authority actors: a prediction is local until the server's state arrives.
		if (!HasAuthority())
		{
			LocalAbilityStates.Add(NewState);
		}
		else
		{
			AuthorityAbilityStates.AbilityStates.Add(NewState);
			MarkAbilityStateDirty(AuthorityAbilityStates.AbilityStates.Last());
		}
	}

	AbilityInstance->Initialize(this, GetSimpleAttributeComponent_Implementation(), AbilityID, ActivationTime);
	// Supersession teardown can remove tags: use the already admitted context.
	const bool bActivated = bPrevalidatedReactivation
		? AbilityInstance->ActivateAbilityPrevalidated(AbilityContext)
		: AbilityInstance->ActivateAbility(AbilityContext);

	// A client's prediction that failed here is never sent to the server, so no authoritative state will ever arrive
	// to remove its local state: drop it now instead of leaking one entry per refused input.
	if (!bActivated && ShouldTrackState && !HasAuthority())
	{
		LocalAbilityStates.RemoveAll([&AbilityID](const FAbilityState& State) { return State.AbilityID == AbilityID; });
	}

	return bActivated;
}

void USimpleGameplayAbilityComponent::ServerActivateAbility_Implementation(
	const FGuid AbilityID,
	TSubclassOf<USimpleGameplayAbility> AbilityClass,
	const FInstancedStruct& AbilityContexts,
	const double ActivationTime)
{
	if (!AbilityClass)
	{
		SIMPLE_LOG(this, FString::Printf(TEXT("[USimpleGameplayAbilityComponent::ServerActivateAbility]: AbilityClass is null!")));
		return;
	}

	// Each ability class decides how far a client's stamp may reach back (or forward) from the server's clock.
	const double ServerTime = GetServerTime();
	const double ClientTime = ActivationTime;
	const double ValidatedTime = AbilityClass.GetDefaultObject()->ValidateClientActivationTime(ClientTime, ServerTime, GetOwnerRoundTripTime());
	if (FMath::Abs(ValidatedTime - ClientTime) > 2.0)
	{
		SIMPLE_LOG(this, FString::Printf(TEXT("[USimpleGameplayAbilityComponent::ServerActivateAbility]: Client timestamp %.2f differs from server %.2f by %.2f seconds. Using %.2f."),
			ClientTime, ServerTime, ServerTime - ClientTime, ValidatedTime));
	}

	ActivateAbilityInternal(AbilityID, AbilityClass, AbilityContexts, true, ValidatedTime, EAbilityNetworkRole::Client);
}

float USimpleGameplayAbilityComponent::GetOwnerRoundTripTime() const
{
	if (const APawn* OwnerPawn = Cast<APawn>(GetOwner()))
	{
		if (const APlayerState* PS = OwnerPawn->GetPlayerState())
		{
			return PS->GetPingInMilliseconds() / 1000.f;
		}
	}
	if (const APlayerState* OwnerPlayerState = Cast<APlayerState>(GetOwner()))
	{
		return OwnerPlayerState->GetPingInMilliseconds() / 1000.f;
	}
	const UNetConnection* Connection = GetOwner() ? GetOwner()->GetNetConnection() : nullptr;
	return Connection ? Connection->AvgLag : 0.f;
}

bool USimpleGameplayAbilityComponent::ReviseActivationContext(const FGuid AbilityInstanceID, const FInstancedStruct& NewActivationContext)
{
	if (!HasAuthority())
	{
		return false;
	}

	for (FAbilityState& AbilityState : AuthorityAbilityStates.AbilityStates)
	{
		if (AbilityState.AbilityID == AbilityInstanceID)
		{
			AbilityState.ActivationContext = NewActivationContext;
			MarkAbilityStateDirty(AbilityState);
			return true;
		}
	}
	return false;
}

void USimpleGameplayAbilityComponent::MarkAbilityStateDirty(FAbilityState& AbilityState)
{
	++AbilityState.Revision;
	if (AbilityState.Revision == 0)
	{
		++AbilityState.Revision; // Zero is reserved for a local prediction, including after wraparound.
	}
	AuthorityAbilityStates.MarkItemDirty(AbilityState);
}

void USimpleGameplayAbilityComponent::RecordRejectedActivation(const FGuid& AbilityID, const TSubclassOf<USimpleGameplayAbility>& AbilityClass,
	const FInstancedStruct& AbilityContext, const double ActivationTime, const EAbilityNetworkRole ActivatedOn,
	const EAbilityActivationFailureReason Reason)
{
	// A client's own refused prediction never reaches the server, so there is nothing to tell anyone.
	if (!HasAuthority())
	{
		return;
	}

	FAbilityState RejectedState;
	RejectedState.AbilityID = AbilityID;
	RejectedState.AbilityClass = AbilityClass;
	RejectedState.AbilityStatus = ActivationFailed;
	RejectedState.FailureReason = Reason;
	RejectedState.ActivatedOn = ActivatedOn;
	RejectedState.ActivationTimeStamp = ActivationTime;
	RejectedState.ActivationContext = AbilityContext;
	RejectedState.EndingTimeStamp = GetServerTime();
	AuthorityAbilityStates.AbilityStates.Add(RejectedState);
	MarkAbilityStateDirty(AuthorityAbilityStates.AbilityStates.Last());
}

USimpleGameplayAbility* USimpleGameplayAbilityComponent::GetAbilityInstanceByClass(TSubclassOf<USimpleGameplayAbility> AbilityClass)
{
	if (AbilityClass.GetDefaultObject()->InstancingPolicy == EAbilityInstancingPolicy::SingleInstance)
	{
		// Check if we already have an instance of the ability created
		for (USimpleGameplayAbility* InstancedAbility : InstancedAbilities)
		{
			if (InstancedAbility->GetClass() == AbilityClass)
			{
				return InstancedAbility;
			}
		}
	}

	// If we don't have an instance of the ability created (or a MultipleInstance policy) we create one
	USimpleGameplayAbility* NewAbilityInstance = NewObject<USimpleGameplayAbility>(GetOuter(), AbilityClass);
	NewAbilityInstance->OnActivationSuccess.AddDynamic(this, &USimpleGameplayAbilityComponent::OnAbilityActivationSuccess);
	NewAbilityInstance->OnActivationFailed.AddDynamic(this, &USimpleGameplayAbilityComponent::OnAbilityActivationFailed);
	NewAbilityInstance->OnAbilityEnded.AddDynamic(this, &USimpleGameplayAbilityComponent::OnAbilityEnded);
	NewAbilityInstance->OnAbilityCancelled.AddDynamic(this, &USimpleGameplayAbilityComponent::OnAbilityCancelled);
	
	InstancedAbilities.Add(NewAbilityInstance);
	
	return NewAbilityInstance;	
}

FAbilityState* USimpleGameplayAbilityComponent::GetAbilityStateByID(FGuid AbilityInstanceID)
{
	TArray<FAbilityState>& AbilityStates = HasAuthority() ? AuthorityAbilityStates.AbilityStates : LocalAbilityStates;

	for (FAbilityState& AbilityState : AbilityStates)
	{
		if (AbilityState.AbilityID == AbilityInstanceID)
		{
			return &AbilityState;
		}
	}

	return nullptr;
}

void USimpleGameplayAbilityComponent::CancelAbility(const FGuid AbilityInstanceID, const FInstancedStruct CancellationContext)
{
	if (USimpleGameplayAbility* AbilityInstance = GetAbilityInstanceByID(AbilityInstanceID))
	{
		if (!AbilityInstance->IsActive)
		{
			return;
		}
		
		AbilityInstance->CancelAbility(FGameplayTag::EmptyTag, CancellationContext);
	}
}

void USimpleGameplayAbilityComponent::CancelAbilityPredicted(FGuid AbilityInstanceID, FInstancedStruct CancellationContext)
{
	if (HasAuthority())
	{
		CancelAbility(AbilityInstanceID, CancellationContext);	
		return;
	}
	
	CancelAbility(AbilityInstanceID, CancellationContext);
	ServerCancelAbility(AbilityInstanceID, CancellationContext);
}

void USimpleGameplayAbilityComponent::ServerCancelAbility_Implementation(FGuid AbilityInstanceID, FInstancedStruct CancellationContext)
{
	CancelAbility(AbilityInstanceID, CancellationContext);
}

TArray<FGuid> USimpleGameplayAbilityComponent::CancelAbilitiesWithTags(const FGameplayTagContainer Tags, FInstancedStruct CancellationContext)
{
	TArray<FGuid> CancelledAbilities;

	// Iterate a copy: cancelling runs game code and removes MultipleInstances abilities from the array.
	const TArray<USimpleGameplayAbility*> Candidates = InstancedAbilities;
	for (USimpleGameplayAbility* AbilityInstance : Candidates)
	{
		if (AbilityInstance && AbilityInstance->IsActive && AbilityInstance->AbilityTags.HasAnyExact(Tags))
		{
			const FGuid CancelledID = AbilityInstance->AbilityID;
			AbilityInstance->CancelAbility(FGameplayTag::EmptyTag, CancellationContext);
			CancelledAbilities.Add(CancelledID);
		}
	}

	return CancelledAbilities;
}

TArray<FGuid> USimpleGameplayAbilityComponent::CancelAbilitiesWithTagsPredicted(FGameplayTagContainer Tags, FInstancedStruct CancellationContext)
{
	if (HasAuthority())
	{
		return CancelAbilitiesWithTags(Tags, CancellationContext);	
	}
	
	TArray<FGuid> CancelledAbilities = CancelAbilitiesWithTags(Tags, CancellationContext);
	ServerCancelAbilitiesWithTags(Tags, CancellationContext);
	return CancelledAbilities;
}


void USimpleGameplayAbilityComponent::ServerCancelAbilitiesWithTags_Implementation(FGameplayTagContainer Tags, FInstancedStruct CancellationContext)
{
	CancelAbilitiesWithTags(Tags, CancellationContext);
}

TArray<FGuid> USimpleGameplayAbilityComponent::CancelAbilitiesWithClass(TSubclassOf<USimpleGameplayAbility> AbilityClass, FInstancedStruct CancellationContext)
{
	TArray<FGuid> CancelledAbilities;

	// Iterate a copy: cancelling runs game code and removes MultipleInstances abilities from the array.
	const TArray<USimpleGameplayAbility*> Candidates = InstancedAbilities;
	for (USimpleGameplayAbility* AbilityInstance : Candidates)
	{
		if (AbilityInstance && AbilityInstance->IsActive && AbilityInstance->GetClass() == AbilityClass)
		{
			const FGuid CancelledID = AbilityInstance->AbilityID;
			AbilityInstance->CancelAbility(FGameplayTag::EmptyTag, CancellationContext);
			CancelledAbilities.Add(CancelledID);
		}
	}

	return CancelledAbilities;
}

TArray<FGuid> USimpleGameplayAbilityComponent::CancelAbilitiesWithClassPredicted(TSubclassOf<USimpleGameplayAbility> AbilityClass, FInstancedStruct CancellationContext)
{
	if (HasAuthority())
	{
		return CancelAbilitiesWithClass(AbilityClass, CancellationContext);	
	}
	
	TArray<FGuid> CancelledAbilities = CancelAbilitiesWithClass(AbilityClass, CancellationContext);
	ServerCancelAbilitiesWithClass(AbilityClass, CancellationContext);
	return CancelledAbilities;
}

void USimpleGameplayAbilityComponent::ServerCancelAbilitiesWithClass_Implementation(TSubclassOf<USimpleGameplayAbility> AbilityClass, FInstancedStruct CancellationContext)
{
	CancelAbilitiesWithClass(AbilityClass, CancellationContext);
}

bool USimpleGameplayAbilityComponent::IsAvatarActorOfType(TSubclassOf<AActor> AvatarClass) const
{
	if (AvatarActor && AvatarActor->IsA(AvatarClass))
	{
		return true;
	}

	return false;
}

void USimpleGameplayAbilityComponent::GrantAbility(const TSubclassOf<USimpleGameplayAbility> AbilityClass)
{
	GrantedAbilities.AddUnique(AbilityClass);
}

void USimpleGameplayAbilityComponent::RevokeAbility(const TSubclassOf<USimpleGameplayAbility> AbilityClass)
{
	GrantedAbilities.Remove(AbilityClass);
}

void USimpleGameplayAbilityComponent::AddGameplayAbilitySnapshot(const FGuid AbilityID, const int32 SnapshotCounter, FInstancedStruct SnapshotData)
{
	FAbilitySnapshot NewSnapshot;
	NewSnapshot.AbilityID = AbilityID;
	NewSnapshot.SnapshotCounter = SnapshotCounter;
	NewSnapshot.SnapshotData = MoveTemp(SnapshotData);
	NewSnapshot.TimeStamp = GetServerTime();

	if (HasAuthority())
	{
		// Mark the array element, not the local copy, so the element itself carries its replication ID.
		const int32 Index = AuthorityAbilitySnapshots.Snapshots.Add(MoveTemp(NewSnapshot));
		AuthorityAbilitySnapshots.MarkItemDirty(AuthorityAbilitySnapshots.Snapshots[Index]);
		return;
	}

	LocalPendingAbilitySnapshots.Add(MoveTemp(NewSnapshot));

	// The server's snapshot may have arrived first (an ability the server started before this client did).
	const FAbilitySnapshot* AuthoritySnapshot = AuthorityAbilitySnapshots.Snapshots.FindByPredicate(
		[&AbilityID, SnapshotCounter](const FAbilitySnapshot& Snapshot)
		{
			return Snapshot.AbilityID == AbilityID && Snapshot.SnapshotCounter == SnapshotCounter;
		});
	if (AuthoritySnapshot)
	{
		TryResolveSnapshot(*AuthoritySnapshot);
	}
}

/* Implementation of ISimpleAbilitySystemComponent interface */

USimpleAttributeComponent* USimpleGameplayAbilityComponent::GetSimpleAttributeComponent_Implementation()
{
	if (CachedAttributeComponent)
	{
		return CachedAttributeComponent;
	}
	
	// Start by checking if the attribute component is on the owner actor
	if (USimpleAttributeComponent* AttributeComp = GetOwner()->GetComponentByClass<USimpleAttributeComponent>())
	{
		CachedAttributeComponent = AttributeComp;
		return AttributeComp;
	}
	
	// Check if the attribute component is on the player state (assuming the owner is a pawn)
	if (const APawn* OwnerPawn = Cast<APawn>(GetOwner()))
	{
		if (const APlayerState* PS = OwnerPawn->GetPlayerState())
		{
			if (USimpleAttributeComponent* AttributeComp = PS->GetComponentByClass<USimpleAttributeComponent>())
			{
				CachedAttributeComponent = AttributeComp;
				return AttributeComp;
			}
		}
	}
	
	// Check if the attribute component is on the player state (assuming the owner is a player controller)
	if (const APlayerController* OwnerPC = Cast<APlayerController>(GetOwner()))
	{
		if (const APlayerState* PS = OwnerPC->PlayerState)
		{
			if (USimpleAttributeComponent* AttributeComp = PS->GetComponentByClass<USimpleAttributeComponent>())
			{
				CachedAttributeComponent = AttributeComp;
				return AttributeComp;
			}
		}
	}

	SIMPLE_LOG(GetOwner(), TEXT("[USimpleGameplayAbilityComponent::GetSimpleAttributeComponent_Implementation]: No SimpleAttributeComponent found on owner or player state! If you placed it on another actor, please override this function to return it correctly."));
	return nullptr;
}

/* Utility Functions */

USimpleGameplayAbility* USimpleGameplayAbilityComponent::GetAbilityInstanceByID(FGuid AbilityInstanceID)
{
	for (USimpleGameplayAbility* InstancedAbility : InstancedAbilities)
	{
		if (InstancedAbility->AbilityID == AbilityInstanceID)
		{
			return InstancedAbility;
		}
	}
	
	return nullptr;
}

USimpleTimeSynchronizer* USimpleGameplayAbilityComponent::GetTimeSynchronizerComponent_Implementation()
{
	// Default to assuming the owner actor has a time synchronizer component
	return GetOwner()->GetComponentByClass<USimpleTimeSynchronizerExtended>();
}

double USimpleGameplayAbilityComponent::GetServerTime()
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

bool USimpleGameplayAbilityComponent::HasAuthority() const
{
	if (GetOwner())
	{
		return GetOwner()->HasAuthority();
	}

	return false;
}

bool USimpleGameplayAbilityComponent::IsOwnedByLocalPlayer() const
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return false;
	}

	// Case 1: Component is on a Pawn
	if (APawn* OwnerPawn = Cast<APawn>(Owner))
	{
		return OwnerPawn->IsLocallyControlled();
	}

	// Case 2: Component is on a PlayerState
	if (APlayerState* OwnerPlayerState = Cast<APlayerState>(Owner))
	{
		if (APlayerController* PC = Cast<APlayerController>(OwnerPlayerState->GetOwner()))
		{
			return PC->IsLocalController();
		}
	}

	// Fallback: Use role-based check
	return Owner->GetLocalRole() == ROLE_AutonomousProxy;
}

bool USimpleGameplayAbilityComponent::IsAnyAbilityActive() const
{
	for (const USimpleGameplayAbility* AbilityInstance : InstancedAbilities)
	{
		if (AbilityInstance->IsActive)
		{
			return true;
		}
	}

	return false;
}

/* Ability Lifecycle */

void USimpleGameplayAbilityComponent::OnAbilityActivationSuccess(USimpleAbilityBase* AbilityInstance)
{
	USimpleGameplayAbility* Ability = Cast<USimpleGameplayAbility>(AbilityInstance);
	if (!Ability)
	{
		return;
	}

	// An instant ability has already ended inside OnActivate: its terminal state stands.
	if (FAbilityState* AbilityState = GetAbilityStateByID(Ability->AbilityID); AbilityState && AbilityState->AbilityStatus == PreActivation)
	{
		AbilityState->AbilityStatus = ActivationSuccess;

		if (HasAuthority())
		{
			MarkAbilityStateDirty(*AbilityState);
		}
	}

	// Start cooldown on activation if configured
	if (Ability->HasCooldown() && Ability->CooldownStartPolicy == ECooldownStartPolicy::OnActivation)
	{
		StartAbilityCooldownInternal(Ability->GetClass(), Ability->GetCooldownDuration(), !HasAuthority());
	}
}

void USimpleGameplayAbilityComponent::OnAbilityActivationFailed(USimpleAbilityBase* AbilityInstance)
{
	USimpleGameplayAbility* Ability = Cast<USimpleGameplayAbility>(AbilityInstance);
	if (!Ability)
	{
		return;
	}

	if (FAbilityState* AbilityState = GetAbilityStateByID(Ability->AbilityID))
	{
		AbilityState->AbilityStatus = ActivationFailed;
		AbilityState->FailureReason = EAbilityActivationFailureReason::RequirementsFailed;
		AbilityState->EndingTimeStamp = GetServerTime();

		if (HasAuthority())
		{
			MarkAbilityStateDirty(*AbilityState);
		}
	}

	if (Ability->InstancingPolicy == EAbilityInstancingPolicy::MultipleInstances)
	{
		InstancedAbilities.Remove(Ability);
	}
}

void USimpleGameplayAbilityComponent::OnAbilityEnded(USimpleAbilityBase* AbilityInstance, FGameplayTag EndStatus, FInstancedStruct EndingContext)
{
	USimpleGameplayAbility* Ability = Cast<USimpleGameplayAbility>(AbilityInstance);
	if (!Ability)
	{
		return;
	}

	RecordAbilityStopped(*Ability, Ended, EndingContext);

	// Start cooldown on end if configured (default behavior)
	if (Ability->HasCooldown() && Ability->CooldownStartPolicy == ECooldownStartPolicy::OnEnd)
	{
		StartAbilityCooldownInternal(Ability->GetClass(), Ability->GetCooldownDuration(), !HasAuthority());
	}
}

void USimpleGameplayAbilityComponent::OnAbilityCancelled(USimpleAbilityBase* AbilityInstance, FGameplayTag CancelStatus, FInstancedStruct CancellationContext)
{
	if (USimpleGameplayAbility* Ability = Cast<USimpleGameplayAbility>(AbilityInstance))
	{
		RecordAbilityStopped(*Ability, Cancelled, CancellationContext);
	}
}

void USimpleGameplayAbilityComponent::RecordAbilityStopped(USimpleGameplayAbility& Ability, const EAbilityStatus TerminalStatus, const FInstancedStruct& StopContext)
{
	if (FAbilityState* AbilityState = GetAbilityStateByID(Ability.AbilityID))
	{
		AbilityState->AbilityStatus = TerminalStatus;
		AbilityState->EndingContext = StopContext;
		AbilityState->EndingTimeStamp = GetServerTime();

		if (HasAuthority())
		{
			MarkAbilityStateDirty(*AbilityState);
		}
	}

	// A MultipleInstances ability lives for one activation only.
	if (Ability.InstancingPolicy == EAbilityInstancingPolicy::MultipleInstances)
	{
		InstancedAbilities.Remove(&Ability);
	}

	// AbilityState is not used past this point: the cleanup may remove array elements.
	CleanupOldAbilityStates();
}

void USimpleGameplayAbilityComponent::CleanupOldAbilityStates()
{
	if (!HasAuthority())
	{
		return;
	}

	const double CurrentTime = GetServerTime();
	const double CleanupThreshold = 5.0; // Remove states older than 5 seconds

	// Retain terminal states for replication/prediction reconciliation, then release them.
	// Rejected SingleInstance reactivations never receive an OnEnd callback.
	const int32 RemovedCount = AuthorityAbilityStates.AbilityStates.RemoveAll([CurrentTime, CleanupThreshold](const FAbilityState& State)
	{
		const bool bIsFinished = (State.AbilityStatus == Ended || State.AbilityStatus == Cancelled || State.AbilityStatus == ActivationFailed);
		const bool bIsOldEnough = (CurrentTime - State.EndingTimeStamp) > CleanupThreshold;
		return bIsFinished && bIsOldEnough;
	});
	if (RemovedCount > 0)
	{
		AuthorityAbilityStates.MarkArrayDirty();
	}

	// Also cleanup old snapshots
	const double SnapshotCleanupThreshold = 10.0; // Keep snapshots a bit longer for late clients
	const int32 RemovedSnapshotCount = AuthorityAbilitySnapshots.Snapshots.RemoveAll([CurrentTime, SnapshotCleanupThreshold](const FAbilitySnapshot& Snapshot)
	{
		return (CurrentTime - Snapshot.TimeStamp) > SnapshotCleanupThreshold;
	});
	if (RemovedSnapshotCount > 0)
	{
		AuthorityAbilitySnapshots.MarkArrayDirty();
	}
}

/* Replication */

void USimpleGameplayAbilityComponent::ClientOnAbilityStateAdded(const FAbilityState& NewAbilityState)
{
	ResolveLocalAbilityState(NewAbilityState);
}

void USimpleGameplayAbilityComponent::ClientOnAbilityStateChanged(const FAbilityState& ChangedAbilityState)
{
	ResolveLocalAbilityState(ChangedAbilityState);
}

FAbilityState* USimpleGameplayAbilityComponent::FindLocalAbilityState(const FGuid& AbilityID)
{
	return LocalAbilityStates.FindByPredicate([&AbilityID](const FAbilityState& AbilityState) { return AbilityState.AbilityID == AbilityID; });
}

void USimpleGameplayAbilityComponent::UpsertLocalAbilityState(const FAbilityState& AuthorityState)
{
	if (FAbilityState* LocalAbilityState = FindLocalAbilityState(AuthorityState.AbilityID))
	{
		*LocalAbilityState = AuthorityState;
	}
	else
	{
		LocalAbilityStates.Add(AuthorityState);
	}
}

void USimpleGameplayAbilityComponent::ResolveLocalAbilityState(const FAbilityState& UpdatedAbilityState)
{
	// Activating, ending and cancelling below run game code which may itself add local states (an ability that predicts
	// another one), reallocating LocalAbilityStates. So nothing here keeps a pointer into that array across such a call:
	// the facts are gathered up front and every write goes through UpsertLocalAbilityState.
	const FGuid AbilityID = UpdatedAbilityState.AbilityID;

	// True once this activation has been started on this machine, by local prediction or by an earlier revision.
	// It must then never be started a second time.
	bool bStartedHere = false;
	if (const FAbilityState* LocalAbilityState = FindLocalAbilityState(AbilityID))
	{
		// A duplicated or delayed update that isn't newer than the authoritative revision already applied is stale.
		// Local predictions have revision 0 and always accept the server's state.
		if (LocalAbilityState->Revision != 0
			&& !FAbilityState::IsRevisionNewer(UpdatedAbilityState.Revision, LocalAbilityState->Revision))
		{
			return;
		}
		bStartedHere = LocalAbilityState->AbilityStatus != PreActivation;
	}

	const TSubclassOf<USimpleGameplayAbility> AbilityClass = static_cast<TSubclassOf<USimpleGameplayAbility>>(UpdatedAbilityState.AbilityClass);
	if (!AbilityClass)
	{
		return;
	}

	// A SingleInstance ability has one (reused) instance. A MultipleInstances ability has one instance per running
	// activation, so it only exists while this activation runs here and is created on demand by StartLocally.
	const bool bIsSingleInstance = AbilityClass.GetDefaultObject()->InstancingPolicy == EAbilityInstancingPolicy::SingleInstance;
	USimpleGameplayAbility* AbilityInstance = GetAbilityInstanceByID(AbilityID);
	if (!AbilityInstance && bIsSingleInstance)
	{
		AbilityInstance = GetAbilityInstanceByClass(AbilityClass);
	}
	const bool bInstanceActive = AbilityInstance && AbilityInstance->IsActive;
	const bool bRunningThisActivation = bInstanceActive && AbilityInstance->AbilityID == AbilityID;
	const bool bRunningOtherActivation = bInstanceActive && !bRunningThisActivation;

	// The state is recorded before activating so the ability can read it while it starts; the lifecycle callbacks of
	// the local run then edit that copy, which is why callers record the server's state once more afterwards.
	auto StartLocally = [&]
	{
		if (!AbilityInstance)
		{
			AbilityInstance = GetAbilityInstanceByClass(AbilityClass);
		}
		UpsertLocalAbilityState(UpdatedAbilityState);
		AbilityInstance->Initialize(this, IAttributeComponentInterface::Execute_GetSimpleAttributeComponent(this), AbilityID, UpdatedAbilityState.ActivationTimeStamp);
		AbilityInstance->ActivateAbility(UpdatedAbilityState.ActivationContext);
	};

	switch (UpdatedAbilityState.AbilityStatus)
	{
		case Ended:
			if (bRunningThisActivation)
			{
				// The server ended the activation we are running.
				AbilityInstance->EndAbility(FGameplayTag::EmptyTag, UpdatedAbilityState.EndingContext);
			}
			// First seen already Ended: it activated and ended on the server between two updates. Replay it, unless that
			// would take over a running instance of a different activation.
			else if (!bStartedHere && !bRunningOtherActivation)
			{
				StartLocally();
				if (AbilityInstance->IsActive && AbilityInstance->AbilityID == AbilityID && AbilityInstance->EndsReplayObservedAsEnded())
				{
					AbilityInstance->EndAbility(FGameplayTag::EmptyTag, UpdatedAbilityState.EndingContext);
				}
			}
			break;

		case ActivationSuccess:
			if (bRunningThisActivation)
			{
				// Already running this activation (predicted here, or an earlier revision): let the instance reconcile
				// with the server's state (validated timestamp, corrected context).
				UpsertLocalAbilityState(UpdatedAbilityState);
				AbilityInstance->OnServerConfirmed(UpdatedAbilityState);
				return;
			}
			if (bStartedHere)
			{
				// It already ran here and has ended or been replaced locally. Never restart it, and never cancel a newer
				// local activation because of it.
				break;
			}
			if (bRunningOtherActivation)
			{
				// The server already accepted this activation, so the running instance must make way for it.
				AbilityInstance->HandleReactivationWhileActive(UpdatedAbilityState.ActivationContext, true);
				if (AbilityInstance->IsActive)
				{
					AbilityInstance->CancelAbility(FGameplayTag::EmptyTag, FInstancedStruct());
				}
			}
			StartLocally();
			break;

		// In these cases the ability is not running on the server and so if it is running locally we need to cancel it
		case PreActivation:
		case ActivationFailed:
		case Cancelled:
			// Cancel the local instance only if it is still running this AbilityID; a newer local activation of the same
			// class must survive a stale rejection.
			if (bRunningThisActivation)
			{
				AbilityInstance->CancelAbility(FGameplayTag::EmptyTag, UpdatedAbilityState.EndingContext);
			}
			// A refused prediction may have predicted an on-activation cooldown the server never started.
			if (UpdatedAbilityState.AbilityStatus == ActivationFailed && bStartedHere
				&& !AuthorityCooldowns.FindByClass(AbilityClass.Get()))
			{
				UClass* RefusedClass = AbilityClass.Get();
				LocalPredictedCooldowns.RemoveAll([RefusedClass](const FCooldownState& State) { return State.AbilityClass == RefusedClass; });
			}
			break;
	}

	// Whatever the local run recorded, the server's state wins.
	UpsertLocalAbilityState(UpdatedAbilityState);
}

void USimpleGameplayAbilityComponent::ClientOnAbilityStateRemoved(const FAbilityState& RemovedAbilityState)
{
	const FGuid AbilityID = RemovedAbilityState.AbilityID;
	LocalAbilityStates.RemoveAll([&AbilityID](const FAbilityState& AbilityState) { return AbilityState.AbilityID == AbilityID; });

	// The server has forgotten this activation: local snapshots it never answered will stay unanswered.
	LocalPendingAbilitySnapshots.RemoveAll([&AbilityID](const FAbilitySnapshot& Snapshot) { return Snapshot.AbilityID == AbilityID; });
}

void USimpleGameplayAbilityComponent::ClientOnAbilitySnapshotAdded(const FAbilitySnapshot& NewAbilitySnapshot)
{
	// If the local ability hasn't taken this snapshot yet, AddGameplayAbilitySnapshot resolves it when it does.
	TryResolveSnapshot(NewAbilitySnapshot);
}

void USimpleGameplayAbilityComponent::TryResolveSnapshot(const FAbilitySnapshot& AuthoritySnapshot)
{
	const int32 LocalIndex = LocalPendingAbilitySnapshots.IndexOfByPredicate(
		[&AuthoritySnapshot](const FAbilitySnapshot& Snapshot)
		{
			return Snapshot.AbilityID == AuthoritySnapshot.AbilityID && Snapshot.SnapshotCounter == AuthoritySnapshot.SnapshotCounter;
		});

	if (LocalIndex == INDEX_NONE)
	{
		return;
	}

	// Take the pending snapshot out before resolving: the resolve callback is game code and may take new snapshots.
	const FAbilitySnapshot LocalSnapshot = LocalPendingAbilitySnapshots[LocalIndex];
	LocalPendingAbilitySnapshots.RemoveAtSwap(LocalIndex);

	// The instance can be active or already ended. If it is gone, or reused by a newer activation, nobody is left to notify.
	if (USimpleGameplayAbility* LocalAbilityInstance = GetAbilityInstanceByID(LocalSnapshot.AbilityID))
	{
		LocalAbilityInstance->OnServerSnapshotReceived(AuthoritySnapshot.SnapshotCounter, AuthoritySnapshot.SnapshotData, LocalSnapshot.SnapshotData);
	}
}

void USimpleGameplayAbilityComponent::ClientOnAbilitySnapshotRemoved(const FAbilitySnapshot& RemovedAbilitySnapshot)
{
	LocalPendingAbilitySnapshots.RemoveAll([&RemovedAbilitySnapshot](const FAbilitySnapshot& Snapshot)
	{
		return Snapshot.AbilityID == RemovedAbilitySnapshot.AbilityID && Snapshot.SnapshotCounter == RemovedAbilitySnapshot.SnapshotCounter;
	});
}

/* Cooldown Functions */

bool USimpleGameplayAbilityComponent::IsAbilityOnCooldown(TSubclassOf<USimpleGameplayAbility> AbilityClass) const
{
	if (!AbilityClass)
	{
		return false;
	}

	const FCooldownState* CooldownState = GetLocalCooldownState(AbilityClass);
	if (!CooldownState)
	{
		return false;
	}

	return !CooldownState->IsExpired(const_cast<USimpleGameplayAbilityComponent*>(this)->GetServerTime());
}

float USimpleGameplayAbilityComponent::GetAbilityRemainingCooldown(TSubclassOf<USimpleGameplayAbility> AbilityClass) const
{
	if (!AbilityClass)
	{
		return 0.0f;
	}

	const FCooldownState* CooldownState = GetLocalCooldownState(AbilityClass);
	if (!CooldownState)
	{
		return 0.0f;
	}

	return CooldownState->GetRemainingTime(const_cast<USimpleGameplayAbilityComponent*>(this)->GetServerTime());
}

float USimpleGameplayAbilityComponent::GetAbilityCooldownProgress(TSubclassOf<USimpleGameplayAbility> AbilityClass) const
{
	if (!AbilityClass)
	{
		return 1.0f;
	}

	const FCooldownState* CooldownState = GetLocalCooldownState(AbilityClass);
	if (!CooldownState)
	{
		return 1.0f;
	}

	return CooldownState->GetProgress(const_cast<USimpleGameplayAbilityComponent*>(this)->GetServerTime());
}

bool USimpleGameplayAbilityComponent::GetAbilityCooldownState(TSubclassOf<USimpleGameplayAbility> AbilityClass, FCooldownState& OutCooldownState) const
{
	if (!AbilityClass)
	{
		return false;
	}

	const FCooldownState* CooldownState = GetLocalCooldownState(AbilityClass);
	if (!CooldownState)
	{
		return false;
	}

	OutCooldownState = *CooldownState;
	return true;
}

void USimpleGameplayAbilityComponent::ClearAbilityCooldown(TSubclassOf<USimpleGameplayAbility> AbilityClass)
{
	if (!AbilityClass || !HasAuthority())
	{
		return;
	}

	UClass* ClassPtr = AbilityClass.Get();
	AuthorityCooldowns.Cooldowns.RemoveAll([ClassPtr](const FCooldownState& State)
	{
		return State.AbilityClass == ClassPtr;
	});
	AuthorityCooldowns.MarkArrayDirty();
}

void USimpleGameplayAbilityComponent::StartAbilityCooldown(TSubclassOf<USimpleGameplayAbility> AbilityClass, float Duration)
{
	if (!AbilityClass || !HasAuthority() || Duration <= 0.0f)
	{
		return;
	}

	StartAbilityCooldownInternal(AbilityClass, Duration, false);
}

void USimpleGameplayAbilityComponent::StartAbilityCooldownInternal(TSubclassOf<USimpleGameplayAbility> AbilityClass, float Duration, bool bIsLocalPrediction)
{
	if (!AbilityClass || Duration <= 0.0f)
	{
		return;
	}

	const double CurrentServerTime = GetServerTime();
	const double ExpiryTime = CurrentServerTime + Duration;
	UClass* ClassPtr = AbilityClass.Get();

	FCooldownState NewCooldownState;
	NewCooldownState.AbilityClass = ClassPtr;
	NewCooldownState.ExpiryTime = ExpiryTime;
	NewCooldownState.Duration = Duration;

	if (bIsLocalPrediction)
	{
		// Client prediction - store locally
		FCooldownState* ExistingState = LocalPredictedCooldowns.FindByPredicate([ClassPtr](const FCooldownState& State)
		{
			return State.AbilityClass == ClassPtr;
		});

		if (ExistingState)
		{
			*ExistingState = NewCooldownState;
		}
		else
		{
			LocalPredictedCooldowns.Add(NewCooldownState);
		}
	}
	else
	{
		// Server authoritative - store in replicated container
		FCooldownState* ExistingState = AuthorityCooldowns.FindByClass(ClassPtr);

		if (ExistingState)
		{
			// Keep the element's replication ID so clients see a change, not a removal plus an add.
			AssignKeepingReplicationIdentity(*ExistingState, NewCooldownState);
			AuthorityCooldowns.MarkItemDirty(*ExistingState);
		}
		else
		{
			// Mark the array element, not the local copy, so the element itself carries its replication ID.
			const int32 Index = AuthorityCooldowns.Cooldowns.Add(NewCooldownState);
			AuthorityCooldowns.MarkItemDirty(AuthorityCooldowns.Cooldowns[Index]);
		}
	}
}

void USimpleGameplayAbilityComponent::CleanupExpiredCooldowns()
{
	if (!HasAuthority())
	{
		return;
	}

	// Periodic cleanup is essential when no subsequent ability ends after rejected reactivations.
	CleanupOldAbilityStates();

	const double CurrentServerTime = GetServerTime();

	// Remove expired cooldowns from the authoritative container
	const int32 RemovedCount = AuthorityCooldowns.Cooldowns.RemoveAll([CurrentServerTime](const FCooldownState& State)
	{
		return State.IsExpired(CurrentServerTime);
	});

	if (RemovedCount > 0)
	{
		AuthorityCooldowns.MarkArrayDirty();
	}
}

void USimpleGameplayAbilityComponent::ClientOnCooldownStateAdded(const FCooldownState& NewCooldownState)
{
	// When server cooldown arrives, update or replace local prediction
	FCooldownState* LocalState = LocalPredictedCooldowns.FindByPredicate([&NewCooldownState](const FCooldownState& State)
	{
		return State.AbilityClass == NewCooldownState.AbilityClass;
	});

	if (LocalState)
	{
		// Reconcile - snap to server authoritative state
		*LocalState = NewCooldownState;
	}
	else
	{
		// Server started a cooldown we didn't predict locally
		LocalPredictedCooldowns.Add(NewCooldownState);
	}
}

void USimpleGameplayAbilityComponent::ClientOnCooldownStateChanged(const FCooldownState& ChangedCooldownState)
{
	// Same as added - reconcile with server state
	ClientOnCooldownStateAdded(ChangedCooldownState);
}

void USimpleGameplayAbilityComponent::ClientOnCooldownStateRemoved(const FCooldownState& RemovedCooldownState)
{
	// The server's cooldown is over (expired or cleared): drop the local copy of it. A local cooldown that expires
	// later is a newer prediction, started after the removed one, and stays until the server answers that one.
	LocalPredictedCooldowns.RemoveAll([&RemovedCooldownState](const FCooldownState& State)
	{
		return State.AbilityClass == RemovedCooldownState.AbilityClass
			&& State.ExpiryTime <= RemovedCooldownState.ExpiryTime + UE_KINDA_SMALL_NUMBER;
	});
}

const FCooldownState* USimpleGameplayAbilityComponent::GetLocalCooldownState(TSubclassOf<USimpleGameplayAbility> AbilityClass) const
{
	UClass* ClassPtr = AbilityClass.Get();

	// On client, prefer local predicted cooldowns for immediate feedback
	// On server, use authoritative cooldowns
	if (HasAuthority())
	{
		return AuthorityCooldowns.FindByClass(ClassPtr);
	}

	// Check local prediction first (for client responsiveness)
	const FCooldownState* LocalState = LocalPredictedCooldowns.FindByPredicate([ClassPtr](const FCooldownState& State)
	{
		return State.AbilityClass == ClassPtr;
	});

	return LocalState;
}

void USimpleGameplayAbilityComponent::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(USimpleGameplayAbilityComponent, GrantedAbilities, COND_OwnerOnly);

	DOREPLIFETIME(USimpleGameplayAbilityComponent, AvatarActor);
	DOREPLIFETIME(USimpleGameplayAbilityComponent, AuthorityAbilityStates);
	DOREPLIFETIME(USimpleGameplayAbilityComponent, AuthorityAbilitySnapshots);
	DOREPLIFETIME(USimpleGameplayAbilityComponent, AuthorityCooldowns);
}


