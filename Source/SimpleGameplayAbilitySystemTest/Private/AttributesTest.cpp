#include "AttributesTest.h"

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "SimpleGameplayAbilitySystem/SimpleGameplayAbilityComponent/SimpleGameplayAbilityComponent.h"
#include "SimpleGameplayAbilitySystem/SimpleGameplayAbilityComponent/SimpleAbilityComponentTypes.h"
#include "SimpleGameplayAbilitySystem/SimpleEventSubsystem/SimpleEventSubsystem.h"
#include "SimpleGameplayAbilitySystem/DefaultTags/DefaultTags.h"
#include "NativeGameplayTags.h"
#include "Framework/DebugTestResult.h"

#include "SGASCommonTestSetup.cpp"
#include "MockClasses/AttributeEventReceiver.h"

#define TestNamePrefix "GameTests.SGAS.Attributes"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAttributesTest_BasicManipulation, TestNamePrefix ".BasicManipulation",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAttributesTest_Regeneration, TestNamePrefix ".Regeneration",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)



class FAttributesTestContext
{
public:
	FAttributesTestContext(FName TestNameSuffix)
		: TestFixture(FName(*(FString(TestNamePrefix) + TestNameSuffix.ToString()))),
		  Character(nullptr),
		  SGASComponent(nullptr)
	{
		World = TestFixture.GetWorld();
		if (World)
		{
			Character = World->SpawnActor<ACharacter>();
			if (Character)
			{
				SGASComponent = NewObject<USimpleGameplayAbilityComponent>(Character, TEXT("TestSGASComponent"));
				if (SGASComponent)
				{
					SGASComponent->RegisterComponent();
				}
			}
		}
	}

	~FAttributesTestContext()
	{
		if (Character)
		{
			Character->Destroy();
			Character = nullptr;
		}
	}

	FTestFixture TestFixture;
	UWorld* World;
	ACharacter* Character;
	USimpleGameplayAbilityComponent* SGASComponent;
};


class FAttributesTestScenarios
{
public:
	FAutomationTestBase* Test;

	FAttributesTestScenarios(FAutomationTestBase* InTest)
		: Test(InTest)
	{
	}

	bool TestBasicAttributeManipulation() const
	{
		const FName TestContextName = TEXT(".BasicManipulationScenario");
		FAttributesTestContext Context(TestContextName);
		FDebugTestResult Res;
		const float Tolerance = 0.001f;

		// --- Initial Setup Checks ---
		Res &= Test->TestNotNull(TEXT("BasicManipulation: World should be created"), Context.World);
		if (!Context.World) return Res;

		Res &= Test->TestNotNull(TEXT("BasicManipulation: Character should be spawned"), Context.Character);
		if (!Context.Character) return Res;

		Res &= Test->TestNotNull(TEXT("BasicManipulation: SGASComponent should be created"), Context.SGASComponent);
		if (!Context.SGASComponent) return Res;

		// For basic tests, HasAuthority should ideally be true.
		Res &= Test->TestTrue(
			TEXT("BasicManipulation: Component should have authority for this test"),
			Context.SGASComponent->HasAuthority());
		if (!Context.SGASComponent->HasAuthority())
		{
			UE_LOG(LogTemp, Warning,
			       TEXT("BasicManipulation: Authority check failed. Test assumes server-side operations."));
		}

		// --- Attribute Definition (No Regen parameters relevant here) ---
		FFloatAttribute TestAttr;
		TestAttr.AttributeName = TEXT("TestHealth");
		TestAttr.AttributeTag = TestAttributeTag;
		TestAttr.BaseValue = 100.0f;
		TestAttr.CurrentValue = 80.0f;

		// --- Test AddFloatAttribute ---
		FGameplayTag DomainTag = TestAttributeTag; // Here we use the attribute id for the domain tag but below we use Authority
		UAttributeEventReceiver* AddEventReceiver = NewObject<UAttributeEventReceiver>();
		AddEventReceiver->ExpectedEventTag = FDefaultTags::FloatAttributeAdded();
		AddEventReceiver->ExpectedDomainTag = DomainTag;
		AddEventReceiver->ExpectedSenderActor = Context.SGASComponent ? Context.SGASComponent->GetOwner() : nullptr;

		USimpleEventSubsystem* EventSubsystem = Context.TestFixture.GetSubsystem();
		FGuid AddEventSubID;

		
		if (EventSubsystem && AddEventReceiver->ExpectedSenderActor)
		{
			FSimpleEventDelegate Delegate;
			Delegate.BindDynamic(AddEventReceiver, &UAttributeEventReceiver::HandleEvent);
			TArray<UObject*> SenderFilter = {AddEventReceiver->ExpectedSenderActor};
			AddEventSubID = EventSubsystem->ListenForEvent(AddEventReceiver, true, // Listen once
			                                               FGameplayTagContainer(FDefaultTags::FloatAttributeAdded()),
			                                               FGameplayTagContainer(DomainTag),
			                                               Delegate, {}, SenderFilter);
		}

		Context.SGASComponent->AddFloatAttribute(TestAttr);

		if (EventSubsystem)
		{
			Res &= Test->TestTrue(
				TEXT("BasicManipulation: AttributeAddedEvent should have fired"), AddEventReceiver->bEventFired);
		}
		if (AddEventSubID.IsValid() && EventSubsystem) EventSubsystem->StopListeningForEventSubscriptionByID(
			AddEventSubID);


		// --- Test GetFloatAttributeValue (CurrentValue and BaseValue) ---
		bool bWasFound = false;
		float Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag,
		                                                            bWasFound);
		Res &= Test->TestTrue(TEXT("BasicManipulation: Attribute should be found after add (CurrentValue)"), bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: CurrentValue after add should be 80.0f"), Value, 80.0f, Tolerance);

		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::BaseValue, TestAttributeTag,
		                                                      bWasFound);
		Res &= Test->TestTrue(TEXT("BasicManipulation: Attribute should be found after add (BaseValue)"), bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: BaseValue after add should be 100.0f"), Value, 100.0f, Tolerance);

		// --- Test HasFloatAttribute ---
		Res &= Test->TestTrue(
			TEXT("BasicManipulation: HasFloatAttribute should be true after add"),
			Context.SGASComponent->HasFloatAttribute(TestAttributeTag));

		// --- Test SetFloatAttributeValue (CurrentValue) ---
		DomainTag = FDefaultTags::AuthorityAttributeDomain(); // Why are we using AuthorityAttributeDomain here but the attributeid elsewhere
		UAttributeEventReceiver* SetEventReceiver = NewObject<UAttributeEventReceiver>();
		SetEventReceiver->ExpectedEventTag = FDefaultTags::FloatAttributeCurrentValueChanged();
		SetEventReceiver->ExpectedDomainTag = DomainTag;
		SetEventReceiver->ExpectedSenderActor = Context.SGASComponent ? Context.SGASComponent->GetOwner() : nullptr;
		FGuid SetEventSubID;

		if (EventSubsystem && SetEventReceiver->ExpectedSenderActor)
		{
			FSimpleEventDelegate Delegate;
			Delegate.BindDynamic(SetEventReceiver, &UAttributeEventReceiver::HandleEvent);
			TArray<UObject*> SenderFilter = {SetEventReceiver->ExpectedSenderActor};
			SetEventSubID = EventSubsystem->ListenForEvent(SetEventReceiver, true,
			                                               FGameplayTagContainer(
				                                               FDefaultTags::FloatAttributeCurrentValueChanged()),
			                                               //FGameplayTagContainer(TestAttr.AttributeTag),
			                                               FGameplayTagContainer(
				                                               DomainTag),
			                                               Delegate, {}, SenderFilter);
		}

		float Overflow = 0.f;
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 90.0f,
		                                              Overflow);

		if (EventSubsystem)
		{
			Res &= Test->TestTrue(
				TEXT("BasicManipulation: AttributeCurrentValueChangedEvent for Set should have fired"),
				SetEventReceiver->bEventFired);
		}
		if (SetEventSubID.IsValid() && EventSubsystem) EventSubsystem->StopListeningForEventSubscriptionByID(
			SetEventSubID);


		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag,
		                                                      bWasFound);
		Res &= Test->TestTrue(TEXT("BasicManipulation: Attribute should be found after set"), bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: CurrentValue after set should be 90.0f"), Value, 90.0f, Tolerance);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: Overflow after non-overflowing set should be 0.0f"), Overflow, 0.0f, Tolerance);


		// --- Test IncrementFloatAttributeValue (CurrentValue) ---
		// Event receiver setup for CurrentValue Change (from increment)
		UAttributeEventReceiver* IncEventReceiver = NewObject<UAttributeEventReceiver>();
		IncEventReceiver->ExpectedEventTag = FDefaultTags::FloatAttributeCurrentValueChanged();
		IncEventReceiver->ExpectedDomainTag = DomainTag;
		IncEventReceiver->ExpectedSenderActor = Context.SGASComponent ? Context.SGASComponent->GetOwner() : nullptr;
		FGuid IncEventSubID;

		if (EventSubsystem && IncEventReceiver->ExpectedSenderActor)
		{
			FSimpleEventDelegate Delegate;
			Delegate.BindDynamic(IncEventReceiver, &UAttributeEventReceiver::HandleEvent);
			TArray<UObject*> SenderFilter = {IncEventReceiver->ExpectedSenderActor};
			IncEventSubID = EventSubsystem->ListenForEvent(IncEventReceiver, true,
			                                               FGameplayTagContainer(
				                                               FDefaultTags::FloatAttributeCurrentValueChanged()),
			                                               FGameplayTagContainer(DomainTag),
			                                               Delegate, {}, SenderFilter);
		}

		Context.SGASComponent->IncrementFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 5.0f,
		                                                    Overflow); // 90 + 5 = 95

		if (EventSubsystem)
		{
			Res &= Test->TestTrue(
				TEXT("BasicManipulation: AttributeCurrentValueChangedEvent for Increment should have fired"),
				IncEventReceiver->bEventFired);
		}
		if (IncEventSubID.IsValid() && EventSubsystem) EventSubsystem->StopListeningForEventSubscriptionByID(
			IncEventSubID);


		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag,
		                                                      bWasFound);
		Res &= Test->TestTrue(TEXT("BasicManipulation: Attribute should be found after increment"), bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: CurrentValue after increment should be 95.0f"), Value, 95.0f, Tolerance);

		// --- Test SetFloatAttributeValue with Clamping (MaxValue) ---
		TestAttr.ValueLimits.UseMaxCurrentValue = true;
		TestAttr.ValueLimits.MaxCurrentValue = 100.0f;
		Context.SGASComponent->OverrideFloatAttribute(TestAttributeTag, TestAttr); // Re-add with MaxValue limit

		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 120.0f,
		                                              Overflow); // Try to set above max
		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag,
		                                                      bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: CurrentValue after set above max should be clamped to 100.0f"), Value, 100.0f,
			Tolerance);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: Overflow after set above max should be 20.0f"), Overflow, 20.0f, Tolerance);

		// --- Test SetFloatAttributeValue with Clamping (MinValue) ---
		TestAttr.ValueLimits.UseMinCurrentValue = true;
		TestAttr.ValueLimits.MinCurrentValue = 10.0f;
		Context.SGASComponent->OverrideFloatAttribute(TestAttributeTag, TestAttr); // Re-add with MinValue limit

		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 5.0f,
		                                              Overflow); // Try to set below min
		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag,
		                                                      bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: CurrentValue after set below min should be clamped to 10.0f"), Value, 10.0f,
			Tolerance);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: Overflow after set below min should be -5.0f (Value - Min)"), Overflow, -5.0f,
			Tolerance); // Overflow can be negative
	
		// --- Test GetFloatAttributeValue (CurrentValueRatio) ---
		// CurrentValue is 10.0f, Min = 10.0f, Max = 100.0f → ratio = (10 - 10) / (100 - 10) = 0.0
		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValueRatio, TestAttributeTag, bWasFound);
		Res &= Test->TestTrue(TEXT("BasicManipulation: Attribute should be found for ratio read"), bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: Ratio should be 0.0f when CurrentValue is at Min"), Value, 0.0f, Tolerance);

		// --- Test SetFloatAttributeValue (CurrentValueRatio) to midpoint ---
		// Setting ratio = 0.5 → Expected CurrentValue = 10 + 0.5 * (100 - 10) = 55.0
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValueRatio, TestAttributeTag, 0.5f, Overflow);
		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bWasFound);
		Res &= Test->TestTrue(TEXT("BasicManipulation: Attribute should be found after ratio set"), bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: CurrentValue after ratio set to 0.5 should be 55.0f"), Value, 55.0f, Tolerance);

		// --- Test SetFloatAttributeValue (CurrentValueRatio) above 1.0 (should clamp) ---
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValueRatio, TestAttributeTag, 1.5f, Overflow);
		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: Ratio set to 1.5 should clamp CurrentValue to Max (100.0f)"), Value, 100.0f, Tolerance);

		// --- Test SetFloatAttributeValue (CurrentValueRatio) below 0.0 (should clamp) ---
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValueRatio, TestAttributeTag, -1.0f, Overflow);
		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bWasFound);
		Res &= Test->TestNearlyEqual(
			TEXT("BasicManipulation: Ratio set to -1.0 should clamp CurrentValue to Min (10.0f)"), Value, 10.0f, Tolerance);
		
		// --- Test RemoveFloatAttribute ---
		DomainTag = TestAttributeTag; // Again here the domain is the attribute id
		UAttributeEventReceiver* RemoveEventReceiver = NewObject<UAttributeEventReceiver>();
		RemoveEventReceiver->ExpectedEventTag = FDefaultTags::FloatAttributeRemoved();
		RemoveEventReceiver->ExpectedDomainTag = DomainTag;
		RemoveEventReceiver->ExpectedSenderActor = Context.SGASComponent ? Context.SGASComponent->GetOwner() : nullptr;
		FGuid RemoveEventSubID;

		if (EventSubsystem && RemoveEventReceiver->ExpectedSenderActor)
		{
			FSimpleEventDelegate Delegate;
			Delegate.BindDynamic(RemoveEventReceiver, &UAttributeEventReceiver::HandleEvent);
			TArray<UObject*> SenderFilter = {RemoveEventReceiver->ExpectedSenderActor};
			RemoveEventSubID = EventSubsystem->ListenForEvent(RemoveEventReceiver, true,
			                                                  FGameplayTagContainer(
				                                                  FDefaultTags::FloatAttributeRemoved()),
			                                                  FGameplayTagContainer(DomainTag),
			                                                  Delegate, {}, SenderFilter);
		}

		Context.SGASComponent->RemoveFloatAttribute(TestAttributeTag);

		if (EventSubsystem)
		{
			Res &= Test->TestTrue(
				TEXT("BasicManipulation: AttributeRemovedEvent should have fired"), RemoveEventReceiver->bEventFired);
		}
		if (RemoveEventSubID.IsValid() && EventSubsystem) EventSubsystem->StopListeningForEventSubscriptionByID(
			RemoveEventSubID);

		Res &= Test->TestFalse(
			TEXT("BasicManipulation: HasFloatAttribute should be false after remove"),
			Context.SGASComponent->HasFloatAttribute(TestAttributeTag));
		Value = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag,
		                                                      bWasFound);
		Res &= Test->TestFalse(TEXT("BasicManipulation: Attribute should NOT be found after remove"), bWasFound);

		return Res;
	}

	bool TestRegeneration() const
	{
		constexpr float Tolerance = 0.1f;
		FAttributesTestContext Context(TEXT(".ComplexRegenScenario_BaseCurrent"));
		FDebugTestResult Res;
		float Overflow = 0.f;
		bool bFound = false;

		Res &= Test->TestNotNull(TEXT("RegenNew: World should be created"), Context.World);
		if (!Context.World) return Res;
		Res &= Test->TestNotNull(TEXT("RegenNew: Character should be spawned"), Context.Character);
		if (!Context.Character) return Res;
		Res &= Test->TestNotNull(TEXT("RegenNew: SGASComponent should be created"), Context.SGASComponent);
		if (!Context.SGASComponent) return Res;
		Res &= Test->TestTrue(TEXT("RegenNew: Component should have authority"), Context.SGASComponent->HasAuthority());
		if (!Context.SGASComponent->HasAuthority()) return Res;

		// 1. Initial Setup
		FFloatAttribute StaminaAttribute;
		StaminaAttribute.AttributeName = TEXT("Stamina");
		StaminaAttribute.AttributeTag = TestAttributeTag;
		StaminaAttribute.BaseValue = 200.0f;
		StaminaAttribute.CurrentValue = 50.0f;
		StaminaAttribute.ValueLimits.UseMaxCurrentValue = true;
		StaminaAttribute.ValueLimits.MaxCurrentValue = 150.0f;
		StaminaAttribute.BaseRegenRate = 2.0f;
		StaminaAttribute.CurrentRegenRate = 0.f;
		StaminaAttribute.bIsRegenerating = false;
		StaminaAttribute.LastRegenParamsUpdateTime_Server = Context.SGASComponent->GetServerTime();

		Context.SGASComponent->AddFloatAttribute(StaminaAttribute);
		
		float CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false); 
		Res &= Test->TestTrue(TEXT("RegenNew: Initial Stamina found"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Initial Stamina value"), CurrentStamina, 50.0f, Tolerance);
		float InitialBaseRegen = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Initial BaseRegen found"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Initial BaseRegen value"), InitialBaseRegen, 2.0f, Tolerance);
		float InitialCurrentRegen = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Initial CurrentRegen found"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Initial CurrentRegen value"), InitialCurrentRegen, 0.0f, Tolerance);


		// --- Part 1: Start Regeneration and Advance Time ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 1 - Start Regen - Setup"), true);
		const float RegenRate1 = 10.0f; // 10 stamina per second
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, RegenRate1, Overflow);
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);

		const float DeltaTime1 = 2.0f;
		double TimeBeforeTick1 = Context.SGASComponent->GetServerTime();
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime1);
		double TimeAfterTick1 = Context.SGASComponent->GetServerTime();
		Res &= Test->TestTrue(TEXT("RegenNew: Time after tick 1"), FMath::IsNearlyEqual(TimeAfterTick1, TimeBeforeTick1 + DeltaTime1, Tolerance));

		float ExpectedStamina1 = 50.0f + (RegenRate1 * DeltaTime1); // 50 + 10*2 = 70
		
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after 1st regen"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after 1st regen"), CurrentStamina, ExpectedStamina1, Tolerance);

		// --- Part 2: Change CurrentRegenRate and Advance More Time ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 2 - Change CurrentRegen Rate - Setup"), true);
		const float RegenRate2 = 20.0f; // New rate
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, RegenRate2, Overflow);

		const float DeltaTime2 = 3.0f;
		double TimeBeforeTick2 = Context.SGASComponent->GetServerTime();
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime2);
		double TimeAfterTick2 = Context.SGASComponent->GetServerTime();
		Res &= Test->TestTrue(TEXT("RegenNew: Time after 2nd tick"), FMath::IsNearlyEqual(TimeAfterTick2, TimeBeforeTick2 + DeltaTime2, Tolerance));

		float ExpectedStamina2 = ExpectedStamina1 + (RegenRate2 * DeltaTime2); // 70 + 20*3 = 130
		
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after 2nd regen"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after 2nd regen"), CurrentStamina, ExpectedStamina2, Tolerance);

		// --- Part 3: Stop Regeneration and Advance Time ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 3 - Stop Regen - Setup"), true);
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);
		float StaminaBeforeStopFinalized = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false); 

		const float DeltaTime3 = 2.0f;
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime3);

		float ExpectedStamina3 = StaminaBeforeStopFinalized; 
		
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after stopping regen"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after stopping regen"), CurrentStamina, ExpectedStamina3, Tolerance);
		
		// --- Part 4: Test MaxValue Clamping ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 4 - MaxValue Clamping - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 145.0f, Overflow);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 10.0f, Overflow);
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);

		const float DeltaTime4 = 2.0f;
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime4);
		
		float ExpectedStamina4 = 150.0f; 
		
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after clamping test"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after clamping"), CurrentStamina, ExpectedStamina4, Tolerance);
		
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag); 

		// --- Part 5: Discrete SetFloatAttributeValue during Active Regeneration ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 5 - Discrete Set during Regen - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 30.0f, Overflow); 
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 15.0f, Overflow); 
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);

		const float DeltaTime5_1 = 1.0f;
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime5_1); // Should be 30 + 15*1 = 45

		const float DiscreteSetValue = 100.0f;
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, DiscreteSetValue, Overflow);
		// After set, current value should be 100. LastRegenTime updated.
		
		float StaminaAfterDiscreteSet = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after discrete set"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina immediately after discrete set"), StaminaAfterDiscreteSet, DiscreteSetValue, Tolerance);

		const float DeltaTime5_2 = 1.0f;
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime5_2);

		float ExpectedStamina5 = DiscreteSetValue + (15.0f * DeltaTime5_2); // 100 + 15*1 = 115
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after regen post-discrete-set"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after regen post-discrete-set"), CurrentStamina, ExpectedStamina5, Tolerance);

		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);

		// --- Part 6: Discrete IncrementFloatAttributeValue during Active Regeneration ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 6 - Discrete Increment during Regen - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 20.0f, Overflow); 
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 10.0f, Overflow);
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);

		const float DeltaTime6_1 = 1.0f; // Stamina becomes 20 + 10*1 = 30
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime6_1);

		const float IncrementAmount = 25.0f; // Stamina becomes 30 + 25 = 55
		Context.SGASComponent->IncrementFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, IncrementAmount, Overflow);
		
		float StaminaAfterDiscreteIncrement = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after discrete increment"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina immediately after discrete increment"), StaminaAfterDiscreteIncrement, 55.0f, Tolerance);

		const float DeltaTime6_2 = 1.0f; // Stamina becomes 55 + 10*1 = 65
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime6_2);

		float ExpectedStamina6 = 55.0f + (10.0f * DeltaTime6_2);
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after regen post-discrete-increment"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after regen post-discrete-increment"), CurrentStamina, ExpectedStamina6, Tolerance);

		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);

		// --- Part 7: Setting CurrentRegenRate to Zero ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 7 - Set CurrentRegenRate to Zero - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 70.0f, Overflow); 
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 50.0f, Overflow); 
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);

		const float DeltaTime7_1 = 0.5f; // Stamina becomes 70 + 50*0.5 = 95
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime7_1);

		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 0.0f, Overflow);
		float StaminaWhenRateZeroed = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after rate zeroed"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina immediately after rate zeroed"), StaminaWhenRateZeroed, 95.0f, Tolerance); // Value should be trued up
		
		FFloatAttribute* InternalAttr7 = Context.SGASComponent->GetFloatAttribute(TestAttributeTag);
		if (InternalAttr7) {
			Res &= Test->TestNearlyEqual(TEXT("RegenNew: Internal CurrentRegenRate after zeroed"), InternalAttr7->CurrentRegenRate, 0.0f, Tolerance);
			Res &= Test->TestTrue(TEXT("RegenNew: Internal bIsRegenerating should still be true after rate zeroed"), InternalAttr7->bIsRegenerating); // bIsRegenerating not changed by SetFloatAttributeValue
		}

		const float DeltaTime7_2 = 1.0f;
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime7_2);
		
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after rate zeroed and time advanced"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after rate zeroed and time advanced (should be unchanged)"), CurrentStamina, StaminaWhenRateZeroed, Tolerance);

		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag); // Now explicitly stop

		// --- Part 8: Negative Regeneration (Degeneration) ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 8 - Degeneration - Setup"), true);
		FFloatAttribute StaminaAttributeWithMin = StaminaAttribute; // Make a copy from initial setup
		StaminaAttributeWithMin.ValueLimits.UseMinCurrentValue = true; 
		StaminaAttributeWithMin.ValueLimits.MinCurrentValue = 10.0f;
		Context.SGASComponent->OverrideFloatAttribute(TestAttributeTag, StaminaAttributeWithMin); 
		
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 100.0f, Overflow); 
		const float DegenRate = -20.0f;
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, DegenRate, Overflow);
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);

		const float DeltaTime8_1 = 2.0f; // Stamina becomes 100 - 20*2 = 60
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime8_1);

		float ExpectedStamina8_1 = 60.0f;
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after 1st degen"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after 1st degen"), CurrentStamina, ExpectedStamina8_1, Tolerance);
		
		const float DeltaTime8_2 = 3.0f; // Stamina becomes 60 - 20*3 = 0, clamped to 10
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime8_2);

		float ExpectedStamina8_2 = 10.0f; 
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after degen to min clamp"), bFound);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after degen to min clamp"), CurrentStamina, ExpectedStamina8_2, Tolerance);

		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);

		// --- Part 9: Starting and Stopping Rapidly / No Time Passed ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 9 - Rapid Start/Stop - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 50.0f, Overflow); 
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 1000.0f, Overflow); 

		double TimeBeforeRapidOps = Context.SGASComponent->GetServerTime();
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag); 
		double TimeAfterRapidOps = Context.SGASComponent->GetServerTime();

		float TimeDiffRapid = static_cast<float>(TimeAfterRapidOps - TimeBeforeRapidOps);
		Res &= Test->TestTrue(TEXT("RegenNew: Time diff for rapid ops very small"), TimeDiffRapid < 0.01f); 
		
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestTrue(TEXT("RegenNew: Stamina found after rapid start/stop"), bFound);
		// If StopFloatAttributeRegeneration correctly trues up CurrentValue for the tiny elapsed time, 
		// it might be slightly more than 50.0f if 1000.0f/s is the rate.
		// However, if GetServerTime() has low resolution or operations are fast enough, it might be ~0.
		// Let's expect it to be very close to 50.0f.
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: Stamina after rapid start/stop"), CurrentStamina, 50.0f, Tolerance * 2.0f); 

		// --- NEW TESTS: BaseRegenRate vs CurrentRegenRate ---

		// --- Part 10: Changing BaseRegenRate does not affect active CurrentRegenRate or CurrentValue ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 10 - BaseRegen Change (Active Regen) - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 50.0f, Overflow);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, 5.0f, Overflow);
		const float ActiveCurrentRateP10 = 15.0f;
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, ActiveCurrentRateP10, Overflow);
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);

		const float DeltaTime10_1 = 1.0f; // Stamina = 50 + 15*1 = 65
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime10_1);
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P10 Stamina after 1st tick"), CurrentStamina, 65.0f, Tolerance);

		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, 1.0f, Overflow); // Change BaseRegen
		float NewBaseRegenP10 = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P10 BaseRegen updated"), NewBaseRegenP10, 1.0f, Tolerance);
		float CurrentRegenP10 = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P10 CurrentRegen unchanged by BaseRegen change"), CurrentRegenP10, ActiveCurrentRateP10, Tolerance);
		float CurrentStaminaP10AfterBaseChange = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P10 CurrentValue unchanged by BaseRegen change"), CurrentStaminaP10AfterBaseChange, 65.0f, Tolerance);


		const float DeltaTime10_2 = 1.0f; // Stamina = 65 + 15*1 = 80 (still using ActiveCurrentRateP10)
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime10_2);
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P10 Stamina after 2nd tick (BaseRegen had no effect)"), CurrentStamina, 80.0f, Tolerance);
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);

		// --- Part 11: Starting Regen with specific CurrentRegenRate, BaseRegenRate is separate ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 11 - Start with explicit CurrentRegen - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 50.0f, Overflow);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, 5.0f, Overflow); // Base is 5
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 0.0f, Overflow);   // Current is 0

		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag); // Regen starts with CurrentRegenRate = 0
		const float DeltaTime11_1 = 2.0f;
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime11_1);
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P11 Stamina unchanged (CurrentRegenRate was 0)"), CurrentStamina, 50.0f, Tolerance);
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);

		// Now, explicitly set CurrentRegenRate from BaseRegenRate
		float BaseRegenToUseP11 = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, bFound, false);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, BaseRegenToUseP11, Overflow);
		float CurrentRegenAfterSetFromBaseP11 = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P11 CurrentRegen set from BaseRegen"), CurrentRegenAfterSetFromBaseP11, 5.0f, Tolerance);

		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag); // Regen starts with CurrentRegenRate = 5
		const float DeltaTime11_2 = 2.0f; // Stamina = 50 + 5*2 = 60
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime11_2);
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P11 Stamina regenerated using BaseRegen value"), CurrentStamina, 60.0f, Tolerance);
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);

		// --- Part 12: Stop, Change Base, Set Current from New Base, Start ---
		Res &= Test->TestTrue(TEXT("RegenNew: Part 12 - Stop, Change Base, Restart - Setup"), true);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, 50.0f, Overflow);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, 10.0f, Overflow);
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, 10.0f, Overflow); // Match Base initially
		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);
		
		const float DeltaTime12_1 = 1.0f; // Stamina = 50 + 10*1 = 60
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime12_1);
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P12 Stamina after 1st regen phase"), CurrentStamina, 60.0f, Tolerance);


		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, 3.0f, Overflow); // New BaseRegen
		float NewBaseRegenP12 = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::BaseRegeneration, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P12 BaseRegen updated"), NewBaseRegenP12, 3.0f, Tolerance);
		
		// Explicitly update CurrentRegenRate from the new BaseRegenRate
		Context.SGASComponent->SetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, NewBaseRegenP12, Overflow);
		float CurrentRegenAfterUpdateP12 = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentRegeneration, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P12 CurrentRegen updated from new BaseRegen"), CurrentRegenAfterUpdateP12, 3.0f, Tolerance);

		Context.SGASComponent->StartFloatAttributeRegeneration(TestAttributeTag);
		constexpr float DeltaTime12_2 = 2.0f; // Stamina = 60 + 3*2 = 66
		Context.World->Tick(ELevelTick::LEVELTICK_All, DeltaTime12_2);
		CurrentStamina = Context.SGASComponent->GetFloatAttributeValue(EAttributeValueType::CurrentValue, TestAttributeTag, bFound, false);
		Res &= Test->TestNearlyEqual(TEXT("RegenNew: P12 Stamina after 2nd regen phase (new rate)"), CurrentStamina, 66.0f, Tolerance);
		Context.SGASComponent->StopFloatAttributeRegeneration(TestAttributeTag);


		// Final check of bIsRegenerating state
		if (FFloatAttribute* FinalInternalAttr = Context.SGASComponent->GetFloatAttribute(TestAttributeTag))
		{
			Res &= Test->TestFalse(TEXT("RegenNew: Final bIsRegenerating state should be false"), FinalInternalAttr->bIsRegenerating);
		}

		return Res;
	}
};


bool FAttributesTest_BasicManipulation::RunTest(const FString& Parameters)
{
    FAttributesTestScenarios TestScenarios(this);
    return TestScenarios.TestBasicAttributeManipulation(); 
}

bool FAttributesTest_Regeneration::RunTest(const FString& Parameters)
{
    FAttributesTestScenarios TestScenarios(this);
    return TestScenarios.TestRegeneration();
}