// Copyright Rancorous Games, 2024

#include "AttributesTest.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleGameplayAbilityComponent/SimpleGameplayAbilityComponent.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeComponentTypes.h"
#include "Framework/DebugTestResult.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeModifier/ModifierActions/ChangeFloatAttributeAction/FloatAttributeActionTypes.h"
#include "MockClasses/AttributeEventReceiver.h"
#include "SimpleGameplayAbilitySystem/Components/SimpleAttributeComponent/SimpleAttributeComponent.h"
#include "SGASCommonTestSetup.cpp"

#define TestNamePrefix "GameTests.SGAS.Attributes"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAttributesTest_BasicManipulation, TestNamePrefix ".BasicManipulation",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Helper class to manage the test environment setup and teardown.
class FAttributesTestContext
{
public:
	FAttributesTestContext(FName TestNameSuffix)
		: TestFixture(FName(*(FString(TestNamePrefix) + TestNameSuffix.ToString()))),
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
					// Manually call BeginPlay to initialize the component for testing
					SGASComponent->RegisterComponent();
					// We already created the attribute component, but let's ensure the context has the correct pointer
					USimpleAttributeComponent* FetchedComp = IAttributeComponentInterface::Execute_GetSimpleAttributeComponent(SGASComponent);
					if (FetchedComp)
					{
						AttributeComponent = FetchedComp;
					}
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
	USimpleAttributeComponent* AttributeComponent;
};


bool FAttributesTest_BasicManipulation::RunTest(const FString& Parameters)
{
	const FName TestContextName = TEXT(".BasicManipulationScenario");
	FAttributesTestContext Context(TestContextName);
	FDebugTestResult Res;
	const float Tolerance = 0.001f;

	// --- Initial Setup Checks ---
	Res &= TestNotNull(TEXT("BasicManipulation: World should be created"), Context.World);
	Res &= TestNotNull(TEXT("BasicManipulation: Character should be spawned"), Context.Character);
	Res &= TestNotNull(TEXT("BasicManipulation: SGASComponent should be created"), Context.SGASComponent);
	
	Res &= TestTrue(TEXT("BasicManipulation: Component should have authority for this test"),Context.SGASComponent->HasAuthority());

	// --- Attribute Definition ---
	FFloatAttribute TestAttr;
	TestAttr.AttributeName = TEXT("TestHealth");
	TestAttr.AttributeTag = TestAttributeTag;
	TestAttr.BaseValue = 100.0f;
	TestAttr.CurrentValue = 80.0f;
	TestAttr.ValueLimits.MaxBaseValue = 200;
	TestAttr.ValueLimits.MinBaseValue = 0;
	TestAttr.ValueLimits.MaxCurrentValue = 200;
	TestAttr.ValueLimits.MinCurrentValue = 0;
	TestAttr.ValueLimits.UseMaxBaseValue = true;
	TestAttr.ValueLimits.UseMinBaseValue = true;
	TestAttr.ValueLimits.UseMaxCurrentValue = true;
	TestAttr.ValueLimits.UseMinCurrentValue = true;

	// --- Test AddFloatAttribute ---
	Context.AttributeComponent->AddFloatAttribute(TestAttr);
	Res &= TestTrue(
		TEXT("BasicManipulation: HasFloatAttribute should be true after add"),
		Context.AttributeComponent->HasFloatAttribute(TestAttributeTag));

	// --- Test GetFloatAttributeValue ---
	bool bWasFound = false;
	float Value = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, TestAttributeTag, bWasFound);
	Res &= TestTrue(TEXT("BasicManipulation: Attribute should be found after add"), bWasFound);
	Res &= TestNearlyEqual(
		TEXT("BasicManipulation: CurrentValue after add should be 80.0f"), Value, 80.0f, Tolerance);

		// --- Test SetFloatAttributeValue (CurrentValue) and Event ---
		UAttributeEventReceiver* SetEventReceiver = NewObject<UAttributeEventReceiver>();
	
		if (Context.AttributeComponent)
		{
			Context.AttributeComponent->OnFloatAttributeCurrentValueChanged.AddDynamic(SetEventReceiver, &UAttributeEventReceiver::HandleFloatAttributeChanged);
		}
		
		float Overflow = 0.f;
		Context.AttributeComponent->SetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, TestAttributeTag, 90.0f, Overflow);
		
		if (Context.AttributeComponent)
		{
			Res &= TestTrue(TEXT("BasicManipulation: AttributeCurrentValueChangedEvent for Set should have fired"), SetEventReceiver->bEventFired);
			Context.AttributeComponent->OnFloatAttributeCurrentValueChanged.RemoveDynamic(SetEventReceiver, &UAttributeEventReceiver::HandleFloatAttributeChanged);
		}	
	Value = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, TestAttributeTag, bWasFound);
	Res &= TestTrue(TEXT("BasicManipulation: Attribute should be found after set"), bWasFound);
	Res &= TestNearlyEqual(
		TEXT("BasicManipulation: CurrentValue after set should be 90.0f"), Value, 90.0f, Tolerance);

	// --- Test SetFloatAttributeValue with Clamping (MaxValue) ---
	TestAttr.ValueLimits.UseMaxCurrentValue = true;
	TestAttr.ValueLimits.MaxCurrentValue = 100.0f;
	TestAttr.CurrentValue = 95.0f; // Set current value to a known state before testing clamp
	Context.AttributeComponent->OverrideFloatAttribute(TestAttributeTag, TestAttr);

	Context.AttributeComponent->SetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, TestAttributeTag, 120.0f, Overflow);
	Value = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, TestAttributeTag, bWasFound);
	Res &= TestNearlyEqual(
		TEXT("BasicManipulation: CurrentValue after set above max should be clamped to 100.0f"), Value, 100.0f, Tolerance);
	Res &= TestNearlyEqual(
		TEXT("BasicManipulation: Overflow after set above max should be 20.0f"), Overflow, 20.0f, Tolerance);

	// --- Test RemoveFloatAttribute ---
	Context.AttributeComponent->RemoveFloatAttribute(TestAttributeTag);
	Res &= TestFalse(
		TEXT("BasicManipulation: HasFloatAttribute should be false after remove"),
		Context.AttributeComponent->HasFloatAttribute(TestAttributeTag));
	
	return Res;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAttributesTest_CurrentValueRatio_ReadWrite, TestNamePrefix ".CurrentValueRatio.ReadWrite", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAttributesTest_CurrentValueRatio_ReadWrite::RunTest(const FString& Parameters)
{
	FAttributesTestContext Context(TEXT(".CurrentValueRatio.ReadWrite"));
	FDebugTestResult Res;
	if (!Context.AttributeComponent) return false;

	// Ensure attribute exists with max current 100
	FFloatAttribute A;
	A.AttributeName = TEXT("RatioAttr");
	A.AttributeTag = TestAttributeTag;
	A.BaseValue = 100.f;
	A.CurrentValue = 50.f;
	A.ValueLimits.UseMaxCurrentValue = true;
	A.ValueLimits.MaxCurrentValue = 100.f;
	Context.AttributeComponent->AddFloatAttribute(A, /*Override*/true);

	bool bFound = false;
	const float Ratio = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValueRatio, TestAttributeTag, bFound);
	Res &= TestTrue(TEXT("CVR: Attribute found"), bFound);
	Res &= TestNearlyEqual(TEXT("CVR: Initial ratio is 0.5"), Ratio, 0.5f, 0.001f);

	// Set ratio to 0.8 -> current should become 80
	float Overflow = 0.f;
	const bool bSetOk = Context.AttributeComponent->SetFloatAttributeValue(EFloatAttributeValueType::CurrentValueRatio, TestAttributeTag, 0.8f, Overflow);
	Res &= TestTrue(TEXT("CVR: Set ratio ok"), bSetOk);
	Res &= TestNearlyEqual(TEXT("CVR: No ratio overflow on 0.8"), Overflow, 0.0f, 0.0001f);
	const float Curr = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, TestAttributeTag, bFound);
	Res &= TestNearlyEqual(TEXT("CVR: Current now 80"), Curr, 80.f, 0.001f);

	// Set ratio to 1.2 -> clamps to 1.0, overflow positive ~0.2
	Overflow = 0.f;
	const bool bSetClamp = Context.AttributeComponent->SetFloatAttributeValue(EFloatAttributeValueType::CurrentValueRatio, TestAttributeTag, 1.2f, Overflow);
	Res &= TestTrue(TEXT("CVR: Set ratio >1 ok (clamped)"), bSetClamp);
	Res &= TestNearlyEqual(TEXT("CVR: Overflow ratio ~0.2"), Overflow, 0.2f, 0.01f);
	const float Curr2 = Context.AttributeComponent->GetFloatAttributeValue(EFloatAttributeValueType::CurrentValue, TestAttributeTag, bFound);
	Res &= TestNearlyEqual(TEXT("CVR: Current now 100"), Curr2, 100.f, 0.001f);

	return Res;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAttributesTest_CurrentValueRatio_NoMax, TestNamePrefix ".CurrentValueRatio.NoMax", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAttributesTest_CurrentValueRatio_NoMax::RunTest(const FString& Parameters)
{
	FAttributesTestContext Context(TEXT(".CurrentValueRatio.NoMax"));
	FDebugTestResult Res;
	if (!Context.AttributeComponent) return false;

	FFloatAttribute A;
	A.AttributeName = TEXT("NoMaxAttr");
	A.AttributeTag = TestAttributeTag;
	A.BaseValue = 100.f;
	A.CurrentValue = 50.f;
	A.ValueLimits.UseMaxCurrentValue = false; // no max
	Context.AttributeComponent->AddFloatAttribute(A, /*Override*/true);

	float Overflow = 0.f;
	const bool bSetOk = Context.AttributeComponent->SetFloatAttributeValue(EFloatAttributeValueType::CurrentValueRatio, TestAttributeTag, 0.5f, Overflow);
	Res &= TestFalse(TEXT("CVR: Setting ratio without max should fail"), bSetOk);

	return Res;
}

// Test case for Struct Attributes
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAttributesTest_StructManipulation, TestNamePrefix ".StructManipulation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAttributesTest_StructManipulation::RunTest(const FString& Parameters)
{
	auto Test = this;
	
    FAttributesTestContext Context(TEXT(".StructManipulationScenario"));
    FDebugTestResult Res;
    if (!Context.SGASComponent) return false;
    
    Res &= Test->TestTrue(TEXT("StructManipulation: Component should have authority for this test"),Context.SGASComponent->HasAuthority());

    UScriptStruct* TestStructType = FFloatAttributeModification::StaticStruct();
    const FGameplayTag StructAttrTag = TestAttributeTag;

    // --- Test AddStructAttribute ---
    FStructAttribute TestStructAttr;
    TestStructAttr.AttributeName = TEXT("TestStruct");
    TestStructAttr.AttributeTag = StructAttrTag;
    TestStructAttr.StructType = TestStructType;
	// The handler is optional, but we can add it to ensure it doesn't break anything.
    TestStructAttr.StructAttributeHandler = UMockAttributeHandler::StaticClass();

    FFloatAttributeModification InitialStructData;
    InitialStructData.NewValue = 123.45f;
    TestStructAttr.AttributeValue.InitializeAs(TestStructType, reinterpret_cast<const uint8*>(&InitialStructData));

    Context.AttributeComponent->AddStructAttribute(TestStructAttr);
    
    // Debug info
    bool bHasAuthority = Context.AttributeComponent->HasAuthority();
    int32 AttrCount = Context.AttributeComponent->HasAuthority() ? Context.AttributeComponent->AuthorityStructAttributes.Attributes.Num() : Context.AttributeComponent->LocalStructAttributes.Num();
    
    Res &= TestNotNull(TEXT("StructManipulation: AttributeComponent should not be null"), Context.AttributeComponent);
    if (!Context.AttributeComponent) return false;
    Res &= Test->TestTrue(TEXT("StructManipulation: HasStructAttribute should be true after add"), Context.AttributeComponent->HasStructAttribute(StructAttrTag));
    
    if (!Context.AttributeComponent->HasStructAttribute(StructAttrTag))
    {
        UE_LOG(LogTemp, Error, TEXT("StructManipulation Debug: Tag=%s, HasAuthority=%d, Count=%d"), *StructAttrTag.ToString(), bHasAuthority, AttrCount);
    }

    // --- Test GetStructAttributeValue ---
    bool bWasFound = false;
    FInstancedStruct RetrievedInstancedStruct = Context.AttributeComponent->GetStructAttributeValue(StructAttrTag, bWasFound);
    Res &= Test->TestTrue(TEXT("StructManipulation: Attribute should be found after add"), bWasFound);
    Res &= Test->TestTrue(TEXT("StructManipulation: Retrieved struct should be valid"), RetrievedInstancedStruct.IsValid());

    if (RetrievedInstancedStruct.IsValid() && RetrievedInstancedStruct.GetScriptStruct() == FFloatAttributeModification::StaticStruct())
    {
	    const FFloatAttributeModification& RetrievedStructData = RetrievedInstancedStruct.Get<const FFloatAttributeModification>();
	    Res &= Test->TestNearlyEqual(TEXT("StructManipulation: Retrieved struct data should match initial data"), RetrievedStructData.NewValue, 123.45f, 0.001f);
    }
    else if (RetrievedInstancedStruct.IsValid())
    {
        Res &= Test->TestFalse(TEXT("StructManipulation: Retrieved struct has incorrect type"), true);
    }
	
    // --- Test SetStructAttributeValue and its standard event ---
    UAttributeEventReceiver* EventReceiver = NewObject<UAttributeEventReceiver>();

    if (Context.AttributeComponent)
    {
	    Context.AttributeComponent->OnStructAttributeChanged.AddDynamic(EventReceiver, &UAttributeEventReceiver::HandleStructAttributeChanged);
    }
    
    FFloatAttributeModification ModifiedStructData;
    ModifiedStructData.NewValue = 543.21f;
    FInstancedStruct ModifiedInstancedStruct;
    ModifiedInstancedStruct.InitializeAs(TestStructType, reinterpret_cast<const uint8*>(&ModifiedStructData));

    Context.AttributeComponent->SetStructAttributeValue(StructAttrTag, ModifiedInstancedStruct);

    Res &= Test->TestTrue(TEXT("StructManipulation: StructAttributeValueChanged event should have fired"), EventReceiver->bEventFired);
    if (Context.AttributeComponent)
    {
	    Context.AttributeComponent->OnStructAttributeChanged.RemoveDynamic(EventReceiver, &UAttributeEventReceiver::HandleStructAttributeChanged);
    }

    // Verify the data was actually set
    RetrievedInstancedStruct = Context.AttributeComponent->GetStructAttributeValue(StructAttrTag, bWasFound);
    Res &= Test->TestTrue(TEXT("StructManipulation: Modified attribute should be found"), bWasFound);
    Res &= Test->TestTrue(TEXT("StructManipulation: Retrieved modified struct should be valid"), RetrievedInstancedStruct.IsValid());
    
    if (RetrievedInstancedStruct.IsValid() && RetrievedInstancedStruct.GetScriptStruct() == FFloatAttributeModification::StaticStruct())
    {
        const FFloatAttributeModification& FinalStructData = RetrievedInstancedStruct.Get<const FFloatAttributeModification>();
        Res &= Test->TestNearlyEqual(TEXT("StructManipulation: Retrieved struct data should match modified data"), FinalStructData.NewValue, 543.21f, 0.001f);
    }
    else if (RetrievedInstancedStruct.IsValid())
    {
        Res &= Test->TestFalse(TEXT("StructManipulation: Retrieved modified struct has incorrect type"), true);
    }

    return Res;
}
