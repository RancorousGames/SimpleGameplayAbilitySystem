#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "SimpleGameplayAbilitySystem/SimpleEventSubsystem/SimpleEventSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "SGASCommonTestSetup.cpp"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSGASFixtureTeardownTest,
    "GameTests.SGAS.Fixture.Teardown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSGASFixtureTeardownTest::RunTest(const FString& Parameters)
{
    if (!TestNotNull(TEXT("engine"), GEngine)) return false;
    const int32 InitialContextCount = GEngine->GetWorldContexts().Num();

    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        TStrongObjectPtr<UWorld> RetainedWorld;
        TStrongObjectPtr<UGameInstance> RetainedInstance;
        {
            FTestFixture Fixture(FName(*FString::Printf(TEXT("SGASFixtureTeardown%d"), Pass)));
            UWorld* World = Fixture.GetWorld();
            if (!TestNotNull(TEXT("test world"), World)) return false;
            RetainedWorld.Reset(World);
            RetainedInstance.Reset(World->GetGameInstance());
            TestNotNull(TEXT("owned context exists while fixture is alive"),
                GEngine->GetWorldContextFromWorld(World));
            TestNotNull(TEXT("event subsystem initialized"), Fixture.GetSubsystem());
        }

        TestEqual(TEXT("world context count restored after destruction"),
            GEngine->GetWorldContexts().Num(), InitialContextCount);
        TestNull(TEXT("fixture removed its world context"),
            GEngine->GetWorldContextFromWorld(RetainedWorld.Get()));
        TestNull(TEXT("fixture shut down its game instance subsystem"),
            RetainedInstance->GetSubsystem<USimpleEventSubsystem>());
    }
    return true;
}

#endif
