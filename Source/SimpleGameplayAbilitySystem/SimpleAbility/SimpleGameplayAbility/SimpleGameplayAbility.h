#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "SimpleGameplayAbilitySystem/SimpleAbility/SimpleAbilityBase/SimpleAbilityBase.h"
#include "SimpleGameplayAbility.generated.h"

class USimpleSubAbility;
class USimpleAttributeComponent;
class USimpleGameplayAbilityComponent;
struct FAbilityState;

UCLASS(Blueprintable, Abstract, BlueprintType)
class SIMPLEGAMEPLAYABILITYSYSTEM_API USimpleGameplayAbility : public USimpleAbilityBase
{
	GENERATED_BODY()

public:
	/* Properties */

	UPROPERTY(BlueprintReadOnly, Category = "SimpleAbility|State")
	FGuid AbilityID;
	
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SimpleAbility")
	EAbilityInstancingPolicy InstancingPolicy = EAbilityInstancingPolicy::SingleInstance;

	/* If true, the owning ability component must have this ability granted to it for this ability to activate. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SimpleAbility|Requirements")
	bool RequireGrantToActivate = true;
	
	/* These tags must be present on the AttributeComponent for this ability to activate. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SimpleAbility|Requirements")
	FGameplayTagContainer ActivationRequiredTags;

	/* These tags must NOT be present on the AttributeComponent for this ability to activate. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SimpleAbility|Requirements")
	FGameplayTagContainer ActivationBlockingTags;
	
	/*
	 * If set, this ability will only activate if ActivationContext contains this struct type.
	 * If left null, the ability will activate with any payload.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SimpleAbility|Requirements")
	UScriptStruct* RequiredContextType;

	/**
	 * This ability will fail to activate if the avatar actor of the ability component is not one of these types.
	 * If left empty any (or no) avatar actor will be allowed.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "SimpleAbility|Requirements")
	TArray<TSubclassOf<AActor>> AvatarTypeFilter;

	/*
	 * Tags that can be used to classify this ability. e.g. "Melee", "Ranged", "AOE", etc.
	 * This is used in USimpleAbilityComponent::CancelAbilitiesWithTags
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SimpleAbility|Tags")
	FGameplayTagContainer AbilityTags;

	/* These tags are added to the AttributeComponent when this ability is activated and removed when it ends. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SimpleAbility|Tags")
	FGameplayTagContainer TemporarilyAppliedTags;

	/* These tags are added to the AttributeComponent when this ability is activated and not automatically removed when it ends. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SimpleAbility|Tags")
	FGameplayTagContainer PermanentlyAppliedTags;

	/* Cooldown Configuration */

	/** Determines how cooldown is calculated for this ability */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "SimpleAbility|Cooldown")
	ECooldownType CooldownType = ECooldownType::NoCooldown;

	/** Fixed cooldown duration in seconds (used when CooldownType is StaticCooldown) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "SimpleAbility|Cooldown",
		meta = (EditCondition = "CooldownType == ECooldownType::StaticCooldown", EditConditionHides, ClampMin = "0.0", Units = "s"))
	float CooldownDuration = 1.0f;

	/** Determines when the cooldown timer starts */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "SimpleAbility|Cooldown",
		meta = (EditCondition = "CooldownType != ECooldownType::NoCooldown", EditConditionHides))
	ECooldownStartPolicy CooldownStartPolicy = ECooldownStartPolicy::OnEnd;

	/* Callable Functions */

	// Called by the AbilityComponent before every activation of this instance. InActivationTime is the (validated or
	// replicated) server time the activation is stamped with; negative means "now".
	void Initialize(USimpleGameplayAbilityComponent* ActivatingAbilityComponent, USimpleAttributeComponent* ActivatingAttributeComponent, FGuid NewAbilityID, double InActivationTime = -1.0);

	/**
	 * Use this function to activate abilities within this ability. SubAbilities don't support replication, you can
	 * think of them as the smallest unit of work an ability can do. For example, a good candidate for a sub ability is
	 * a montage playing ability. The input context would be the montage to play + any other data needed to play the montage.
	 * The output would be if the montage completed successfully or not. In the parent ability you could then use the end result
	 * for a StateSnapshot comparison between Server and Client.
	 * @param AbilityClass The class of the ability to activate
	 * @param ActivationContext Context to pass to the ability
	 * @return The activated sub ability instance
	 */
	UFUNCTION(BlueprintCallable, Meta = (ExpandEnumAsExecs = "ActivationResult"))
	USimpleSubAbility* ActivateSubAbility(TSubclassOf<USimpleSubAbility> AbilityClass, FInstancedStruct ActivationContext, EAbilityActivationResult& ActivationResult);

	USimpleSubAbility* GetSubAbilityInstance(TSubclassOf<USimpleSubAbility> AbilityClass);
	
	/* Overridable Functions */

	UFUNCTION(BlueprintNativeEvent, Category = "Ability")
	void OnAbilityInitialized();
	virtual void OnAbilityInitialized_Implementation() {}

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Ability")
	bool CanActivate(const FInstancedStruct& ActivationContext);
	virtual bool CanActivate_Implementation(const FInstancedStruct& ActivationContext) { return true; }

	/**
	 * Returns the cooldown duration for this ability.
	 * Override this in Blueprint for dynamic cooldown calculations based on attributes, context, etc.
	 * Only called when CooldownType is StaticCooldown or DynamicCooldown.
	 * @return The cooldown duration in seconds
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Ability|Cooldown")
	float GetCooldownDuration() const;
	virtual float GetCooldownDuration_Implementation() const;

	/** Returns true if this ability has a cooldown configured */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ability|Cooldown")
	bool HasCooldown() const { return CooldownType != ECooldownType::NoCooldown; }

	/**
	 * Called on an active SingleInstance ability when its class is activated again, before the instance is re-initialized
	 * for the new activation. Return false to reject the new activation and keep this one running; this is ignored when
	 * bMustReplace is true (the server already accepted the new activation). Overrides may end this instance themselves
	 * with a more specific status; if it is still active afterwards the component cancels it.
	 * The default cancels this instance (the previous behaviour).
	 */
	virtual bool HandleReactivationWhileActive(const FInstancedStruct& NewActivationContext, bool bMustReplace);

		/** Complete preflight against incoming context; restores the running activation context. */
		bool PreflightReactivation(const FInstancedStruct& NewActivationContext);

	/**
	 * Client only: the server confirmed or revised the activation this instance is running (an ActivationSuccess state
	 * with this AbilityID and a newer revision). Authoritative may carry a validated timestamp or a corrected activation
	 * context that differs from the prediction; override to reconcile. The default does nothing.
	 */
	virtual void OnServerConfirmed(const FAbilityState& Authoritative) {}

	/**
	 * Client only: this activation was first observed already Ended (it activated and ended on the server between two
	 * updates) and has just been replayed locally. Return true to end the replay at once with the server's ending
	 * context. The default lets the replay run its own course (instant abilities end by themselves).
	 */
	virtual bool EndsReplayObservedAsEnded() const { return false; }

	/**
	 * Server, called on the class default object: the activation time to use for a client's request stamped ClientTime.
	 * OwnerRTT is the requesting connection's round-trip time in seconds (0 if unknown). The default accepts the stamp,
	 * past or future, unless it is more than 2 s away from ServerNow (then ServerNow is used).
	 */
	virtual double ValidateClientActivationTime(double ClientTime, double ServerNow, float OwnerRTT) const;

	virtual bool CanActivateInternal() override;
	virtual void PreActivateInternal() override;
	virtual void AbilityEndedInternal(FInstancedStruct EndingContext, bool WasCancelled) override;
	
	/* Utility functions */
	UFUNCTION(BlueprintCallable, BlueprintPure)
	USimpleGameplayAbilityComponent* GetAbilityComponent() const { return AbilityComponent; }

	UFUNCTION(BlueprintCallable, BlueprintPure)
	AActor* GetAvatarActor() const;

	UFUNCTION(BlueprintCallable, meta = (DeterminesOutputType = "AvatarClass", HideSelfPin, ExpandEnumAsExecs = "Result"))
	AActor* GetAvatarActorAs(TSubclassOf<AActor> AvatarClass, EGetAvatarActorResult& Result);

	UFUNCTION(BlueprintCallable, BlueprintPure)
	EAbilityNetworkRole GetNetworkRole() const;

	UFUNCTION(BlueprintCallable, BlueprintPure)
	bool HasAuthority() const;

	UFUNCTION(BlueprintCallable, Category = "SimpleAbility|Snapshot")
	void TakeStateSnapshot(FInstancedStruct SnapshotData, const FOnSnapshotResolved& OnResolved);
	// Called by the AbilityComponent on the client version of this ability when a server snapshot is replicated
	void OnServerSnapshotReceived(const int32 SnapshotCounter, const FInstancedStruct& AuthoritySnapshotData, const FInstancedStruct& LocalSnapshotData);
	
	/**
	 * Returns the server time this ability was activated at.
	 * If called from the Server Initiated ability it returns the authoritative time.
	 * If called from a Client Predicted ability it returns the clients estimation of the server time.
	 * @return The server time this ability was activated.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure)
	double GetActivationTime() const;

	/**
	 * Returns the difference in time between when this ability was activated and now (in server time)
	 * You can use this value to help "fast-forward" predicted/replicated abilities.
	 * e.g. A client predicted ability starts by playing an anim montage. When the client calls this function it returns
	 * 0 at the start of the ability. When the server activates this ability it will be > 0 because the timestamp when the
	 * ability was activated will be different from the time on the server currently. You then use this time delay amount to
	 * offset your animation montage on the server, bringing it closer in sync with the client.
	 * @return The activation delay of the ability
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure)
	double GetActivationDelay() const;

protected:
	// Used to keep track of which snapshots need to be resolved
	TMap<int32, FOnSnapshotResolved> PendingSnapshots;
	
	virtual bool IsTickable() const override { return CanTick && IsActive && GetWorld(); }
	
	// Delegate handlers for sub-ability lifecycle
	UFUNCTION()
	void OnSubAbilityEnded(USimpleAbilityBase* AbilityInstance, FGameplayTag StopStatus, FInstancedStruct StopContext);

	UFUNCTION()
	void OnSubAbilityCancelled(USimpleAbilityBase* AbilityInstance, FGameplayTag StopStatus, FInstancedStruct StopContext);

	// Helper to remove a tracked sub-ability by its instance pointer
	void RemoveTrackedSubAbilityByInstance(USimpleAbilityBase* AbilityInstance);
	
private:
	UPROPERTY()
	USimpleGameplayAbilityComponent* AbilityComponent;

	UPROPERTY()
	USimpleAttributeComponent* AttributeComponent;

	// Set every time we activate this ability
	UPROPERTY()
	double ActivationTime = 0;
	
	// Next counter handed to TakeStateSnapshot. Per activation, so client and server number their snapshots identically
	// no matter when either side prunes or resolves earlier ones.
	int32 NextSnapshotCounter = 0;

	// Used to keep track of sub abilities which this ability has created which need to be ended when this ability ends/cancels.
	// A UPROPERTY so running sub abilities (outered to this ability, referenced nowhere else) survive garbage collection.
	UPROPERTY()
	TArray<FActivatedSubAbility> SubAbilityInstances;
};
