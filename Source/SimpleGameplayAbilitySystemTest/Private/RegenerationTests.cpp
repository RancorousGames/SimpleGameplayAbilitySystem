#include "AttributesTest.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleGameplayAbilityComponent/SimpleGameplayAbilityComponent.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeComponent.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeComponentTypes.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeModifier/ModifierActions/ChangeFloatAttributeAction/FloatAttributeActionTypes.h"
#include "MockClasses/AttributeEventReceiver.h"
#include "SGASCommonTestSetup.cpp"

#define RegenTestPrefix "GameTests.SGAS.Regen"

// Helper context same as Attributes tests
class FRegenTestContext
{
public:
	FRegenTestContext(FName TestNameSuffix)
		: TestFixture(FName(*(FString(RegenTestPrefix) + TestNameSuffix.ToString()))),
		  Character(nullptr),
		  SGASComponent(nullptr),
		  AttributeComponent(nullptr)
	{
		World = TestFixture.GetWorld();
		if (World)
		{
			Character = World->SpawnActor<ACharacter>();
			if (Character)
			{
				AttributeComponent = NewObject<USimpleAttributeComponent>(Character, TEXT("TestAttributeComponent"));
				if (AttributeComponent)
				{
					Character->AddOwnedComponent(AttributeComponent);
					AttributeComponent->RegisterComponent();
				}

				SGASComponent = NewObject<USimpleGameplayAbilityComponent>(Character, TEXT("TestSGASComponent"));
				if (SGASComponent)
				{
					Character->AddOwnedComponent(SGASComponent);
					SGASComponent->RegisterComponent();
					USimpleAttributeComponent* FetchedComp = IAttributeComponentInterface::Execute_GetSimpleAttributeComponent(SGASComponent);
					if (FetchedComp)
					{
						AttributeComponent = FetchedComp;
					}
				}
			}
		}
	}

	~FRegenTestContext()
	{
		if (Character)
		{
			Character->Destroy();
			Character = nullptr;
		}
	}

	// utility: tick world for duration
	void TickSeconds(float Seconds, float Step = 0.25f)
	{
		if (!World) return;
		float Acc = 0.f;
		while (Acc < Seconds)
		{
			World->Tick(LEVELTICK_All, Step);
			Acc += Step;
		}
	}

	FTestFixture TestFixture;
	UWorld* World = nullptr;
	ACharacter* Character = nullptr;
	USimpleGameplayAbilityComponent* SGASComponent = nullptr;
	USimpleAttributeComponent* AttributeComponent = nullptr;
};

// 1) Lazy evaluation: effective value increases over time without changing stored CurrentValue until materialized
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegenTest_LazyEvaluation, RegenTestPrefix ".LazyEvaluation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRegenTest_LazyEvaluation::RunTest(const FString& Parameters)
{
	FRegenTestContext Context(TEXT(".LazyEvalScenario"));
	FDebugTestResult Res;
	const float Tolerance = 0.01f;
	Res &= TestNotNull(TEXT("World should be created"), Context.World);
	Res &= TestNotNull(TEXT("AttributeComponent should be created"), Context.AttributeComponent);
	if (!Context.AttributeComponent) return false;

	// Define attributes: Health and HealthRegenRate
	const FGameplayTag HealthTag = FGameplayTag::RequestGameplayTag(TEXT("Test.SGAS.Attributes.Health"));
	const FGameplayTag HealthRegenRateTag = FGameplayTag::RequestGameplayTag(TEXT("Test.SGAS.Attributes.HealthRegenRate"));

	FFloatAttribute Health;
	Health.AttributeName = TEXT("Health");
	Health.AttributeTag = HealthTag;
	Health.BaseValue = 100.f;
	Health.CurrentValue = 50.f;
	Health.ValueLimits.UseMaxCurrentValue = true;
	Health.ValueLimits.MaxCurrentValue = 100.f;
	Context.AttributeComponent->AddFloatAttribute(Health);

	FFloatAttribute RegenRate;
	RegenRate.AttributeName = TEXT("HealthRegenRate");
	RegenRate.AttributeTag = HealthRegenRateTag;
	RegenRate.BaseValue = 0.f;
	RegenRate.CurrentValue = 5.f; // 5 units per second
	Context.AttributeComponent->AddFloatAttribute(RegenRate);

	// Wire regen rate from attribute and start regen
	{
	float Overflow = 0.f;
	Context.AttributeComponent->SetFloatAttributeValue(EFloatAttributeValueType::RegenRate, HealthTag, 5.f, Overflow);
}
	Context.AttributeComponent->StartFloatRegen(HealthTag);

	// Immediately read raw current (should be 50)
	bool bFound = false;
	float RawCurrent = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, HealthTag, bFound);
	Res &= TestTrue(TEXT("Health attribute found"), bFound);
	Res &= TestNearlyEqual(TEXT("Raw current remains 50 before materialization"), RawCurrent, 50.f, Tolerance);

	// Advance simulated time by 2 seconds
	Context.TickSeconds(2.0f, 0.25f);

	// Read effective value (regen applied) without materializing
	float Effective = Context.AttributeComponent->GetEffectiveFloatCurrentValue(HealthTag, bFound, /*bIgnoreRegen*/false);
	Res &= TestTrue(TEXT("Health attribute found for effective"), bFound);
	Res &= TestNearlyEqual(TEXT("Effective value applies regen rate (approx 60)"), Effective, 60.f, 0.11f); // allow small timing jitter

	// Raw still unchanged
	RawCurrent = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, HealthTag, bFound);
	Res &= TestNearlyEqual(TEXT("Raw current still unchanged before materialization"), RawCurrent, 50.f, Tolerance);

	// Materialize via StopFloatRegen (which materializes then stops)
	UAttributeEventReceiver* EventReceiver = NewObject<UAttributeEventReceiver>();
	Context.AttributeComponent->OnFloatAttributeCurrentValueChanged.AddDynamic(EventReceiver, &UAttributeEventReceiver::HandleFloatAttributeChanged);
	Context.AttributeComponent->StopFloatRegen(HealthTag);
	Res &= TestTrue(TEXT("Change event fired on materialization"), EventReceiver->bEventFired);
	Context.AttributeComponent->OnFloatAttributeCurrentValueChanged.RemoveDynamic(EventReceiver, &UAttributeEventReceiver::HandleFloatAttributeChanged);

	// Raw now updated close to Effective
	RawCurrent = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, HealthTag, bFound);
	Res &= TestNearlyEqual(TEXT("Raw current updated after materialization"), RawCurrent, Effective, 0.15f);

	return Res;
}

// 2) Clamping: effective value should not exceed MaxCurrentValue
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRegenTest_Clamping, RegenTestPrefix ".Clamping", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRegenTest_Clamping::RunTest(const FString& Parameters)
{
	FRegenTestContext Context(TEXT(".ClampingScenario"));
	FDebugTestResult Res;
	const float Tolerance = 0.01f;
	if (!Context.AttributeComponent) return false;

	const FGameplayTag AttrTag = FGameplayTag::RequestGameplayTag(TEXT("Test.SGAS.Attributes.Energy"));
	const FGameplayTag RateTag = FGameplayTag::RequestGameplayTag(TEXT("Test.SGAS.Attributes.EnergyRegenRate"));

	FFloatAttribute Energy;
	Energy.AttributeName = TEXT("Energy");
	Energy.AttributeTag = AttrTag;
	Energy.BaseValue = 100.f;
	Energy.CurrentValue = 50.f;
	Energy.ValueLimits.UseMaxCurrentValue = true;
	Energy.ValueLimits.MaxCurrentValue = 55.f; // strict cap
	Context.AttributeComponent->AddFloatAttribute(Energy);

	FFloatAttribute Rate;
	Rate.AttributeName = TEXT("EnergyRegenRate");
	Rate.AttributeTag = RateTag;
	Rate.CurrentValue = 10.f; // 10 per second
	Context.AttributeComponent->AddFloatAttribute(Rate);

	{
	float Overflow = 0.f;
	Context.AttributeComponent->SetFloatAttributeValue(EFloatAttributeValueType::RegenRate, AttrTag, 10.f, Overflow);
}
	Context.AttributeComponent->StartFloatRegen(AttrTag);

	Context.TickSeconds(2.0f, 0.25f);

	bool bFound = false;
	const float Effective = Context.AttributeComponent->GetEffectiveFloatCurrentValue(AttrTag, bFound, /*bIgnoreRegen*/false);
	Res &= TestTrue(TEXT("Attr found"), bFound);
	Res &= TestNearlyEqual(TEXT("Effective clamped to MaxCurrentValue (55)"), Effective, 55.f, Tolerance);

	return Res;
}
