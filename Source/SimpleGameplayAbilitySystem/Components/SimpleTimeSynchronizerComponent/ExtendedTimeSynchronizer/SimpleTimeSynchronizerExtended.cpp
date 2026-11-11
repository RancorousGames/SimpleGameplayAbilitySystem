#include "SimpleTimeSynchronizerExtended.h"

#include "GameFramework/GameStateBase.h"

USimpleTimeSynchronizerExtended::USimpleTimeSynchronizerExtended()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(true);
}

bool USimpleTimeSynchronizerExtended::IsSyncingClient() const
{
	// The sync RPCs only travel on the owning connection. On a simulated proxy the request would just be dropped.
	const AActor* Owner = GetOwner();
	return Owner && !Owner->HasAuthority() && Owner->HasLocalNetOwner();
}

void USimpleTimeSynchronizerExtended::BeginPlay()
{
	Super::BeginPlay();
	SetIsReplicated(true);

	// Initialize sync interval
	CurrentSyncInterval = BaseSyncInterval;
	TimeSinceLastSync = 0.0f;
	TotalSamplesTaken = 0;

	// Reserve space for sample buffer
	SyncSamples.Reserve(SampleBufferSize);
}

void USimpleTimeSynchronizerExtended::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// Only the owning client syncs (the server is the reference, and a proxy has no connection to ask on)
	if (!IsSyncingClient())
	{
		return;
	}

	TimeSinceLastSync += DeltaTime;

	// Determine if we should sync based on burst mode or adaptive interval
	bool bShouldSync;

	if (TotalSamplesTaken < InitialBurstSamples)
	{
		// Burst mode: sync rapidly after joining
		bShouldSync = TimeSinceLastSync >= BurstSampleInterval;
	}
	else
	{
		// Normal mode: use adaptive interval
		bShouldSync = TimeSinceLastSync >= CurrentSyncInterval;
	}

	if (bShouldSync)
	{
		RequestTimeSync();
		TimeSinceLastSync = 0.0f;
	}
}

double USimpleTimeSynchronizerExtended::GetServerTime_Implementation()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0; // World not ready
	}

	// On server, return authoritative time
	if (GetOwnerRole() == ROLE_Authority)
	{
		AGameStateBase* GameState = World->GetGameState();
		if (GameState)
		{
			return GameState->GetServerWorldTimeSeconds();
		}
		return World->GetTimeSeconds(); // Fallback to local time if GameState not ready
	}

	// Until the first sample arrives (and always on a simulated proxy, which cannot sync) there is no offset
	// estimate; the engine's replicated game state time is the best one available.
	if (TotalSamplesTaken == 0)
	{
		return Super::GetServerTime_Implementation();
	}

	// On client, return estimated server time (local time + offset)
	return GetLocalTime() + CurrentClockOffset;
}

float USimpleTimeSynchronizerExtended::GetSyncQuality() const
{
	if (SyncSamples.Num() == 0)
	{
		return 0.0f;
	}

	// Quality metric based on:
	// 1. Number of valid samples (more is better)
	// 2. RTT variance (lower is better)
	// 3. RTT magnitude (lower is better)

	int32 ValidSamples = 0;
	for (const FSyncSample& Sample : SyncSamples)
	{
		if (Sample.bIsValid)
		{
			ValidSamples++;
		}
	}

	// Sample coverage: 0-1 based on how full the buffer is with valid samples
	float SampleCoverage = FMath::Clamp(static_cast<float>(ValidSamples) / SampleBufferSize, 0.0f, 1.0f);

	// RTT quality: 0-1 based on variance (lower variance = higher quality)
	// RTTVariance is in seconds squared, so compare to squared threshold
	// Assume good variance is < (0.01s)² = 0.0001, bad is > (0.1s)² = 0.01
	float VarianceQuality = 1.0f - FMath::Clamp(RTTVariance / 0.01, 0.0f, 1.0f);

	// RTT magnitude quality: 0-1 based on RTT (lower RTT = higher quality)
	// Assume good RTT is < 0.05s, bad is > 0.5s
	float RTTQuality = 1.0f - FMath::Clamp(CurrentRTT / 0.5, 0.0f, 1.0f);

	// Weighted average
	return (SampleCoverage * 0.4f) + (VarianceQuality * 0.3f) + (RTTQuality * 0.3f);
}

void USimpleTimeSynchronizerExtended::RequestTimeSync()
{
	if (IsSyncingClient())
	{
		ServerRequestTimeSync(GetLocalTime());
	}
}

void USimpleTimeSynchronizerExtended::ServerRequestTimeSync_Implementation(double ClientSendTime)
{
	// Server receives request and immediately responds with timestamps
	UWorld* World = GetWorld();
	if (!World || !World->GetGameState())
	{
		return; // GameState not ready yet
	}

	double ServerReceiveTime = World->GetGameState()->GetServerWorldTimeSeconds();
	double ServerSendTime = ServerReceiveTime; // Assume negligible processing time

	ClientReceiveTimeSync(ClientSendTime, ServerReceiveTime, ServerSendTime);
}

void USimpleTimeSynchronizerExtended::ClientReceiveTimeSync_Implementation(double ClientSendTime, double ServerReceiveTime, double ServerSendTime)
{
	// Client receives response and creates a sample
	double ClientReceiveTime = GetLocalTime();

	FSyncSample NewSample(ClientSendTime, ServerReceiveTime, ServerSendTime, ClientReceiveTime);

	// A response that claims to arrive before its request was sent is a duplicate or reordered packet.
	if (NewSample.RTT < 0.0)
	{
		return;
	}

	if (TotalSamplesTaken > 0 && ContradictsCurrentEstimate(NewSample))
	{
		if (++ConsecutiveContradictions >= 2)
		{
			// Confirmed: the clocks stepped relative to each other. The history describes the old relation, so
			// drop it and converge again from this sample with a fresh burst.
			SyncSamples.Reset();
			TotalSamplesTaken = 0;
			ConsecutiveContradictions = 0;
		}
		else
		{
			// Could be one bad packet. Don't let it into the estimate; ask again right away to find out.
			TimeSinceLastSync = FMath::Max(TimeSinceLastSync, FMath::Max(CurrentSyncInterval, BurstSampleInterval));
			return;
		}
	}
	else
	{
		ConsecutiveContradictions = 0;
	}

	ProcessSyncSample(NewSample);

	TotalSamplesTaken++;
}

void USimpleTimeSynchronizerExtended::ProcessSyncSample(const FSyncSample& Sample)
{
	// Add to buffer (rolling window)
	if (SyncSamples.Num() >= SampleBufferSize)
	{
		SyncSamples.RemoveAt(0);
	}
	SyncSamples.Add(Sample);

	// Reject outliers based on RTT
	RejectOutliers();

	// Update RTT statistics
	UpdateRTTStatistics();

	// Calculate new offset estimate using median
	double NewOffset = CalculateMedianOffset();

	// Apply exponential smoothing if we have a previous offset
	if (TotalSamplesTaken > 0)
	{
		// Adaptive alpha: higher quality samples get more weight
		float Quality = GetSyncQuality();
		float AdaptiveAlpha = FMath::Lerp(SmoothingAlpha * 0.5f, SmoothingAlpha * 2.0f, Quality);
		AdaptiveAlpha = FMath::Clamp(AdaptiveAlpha, 0.01f, 1.0f);

		CurrentClockOffset = FMath::Lerp(CurrentClockOffset, NewOffset, AdaptiveAlpha);
	}
	else
	{
		CurrentClockOffset = NewOffset;
	}

	// Update adaptive sync interval
	UpdateAdaptiveSyncInterval();
}

double USimpleTimeSynchronizerExtended::CalculateMedianOffset() const
{
	TArray<double> ValidOffsets;
	ValidOffsets.Reserve(SyncSamples.Num());

	for (const FSyncSample& Sample : SyncSamples)
	{
		if (Sample.bIsValid)
		{
			ValidOffsets.Add(Sample.ClockOffset);
		}
	}

	if (ValidOffsets.Num() == 0)
	{
		return CurrentClockOffset; // Fallback to current if no valid samples
	}

	// Sort and find median
	ValidOffsets.Sort();

	int32 MedianIndex = ValidOffsets.Num() / 2;
	if (ValidOffsets.Num() % 2 == 0 && ValidOffsets.Num() > 1)
	{
		// Even number: average the two middle values
		return (ValidOffsets[MedianIndex - 1] + ValidOffsets[MedianIndex]) / 2.0;
	}
	else
	{
		return ValidOffsets[MedianIndex];
	}
}

double USimpleTimeSynchronizerExtended::CalculateMedianRTT() const
{
	TArray<double> ValidRTTs;
	ValidRTTs.Reserve(SyncSamples.Num());

	for (const FSyncSample& Sample : SyncSamples)
	{
		if (Sample.bIsValid)
		{
			ValidRTTs.Add(Sample.RTT);
		}
	}

	if (ValidRTTs.Num() == 0)
	{
		return CurrentRTT; // Fallback to current if no valid samples
	}

	// Sort and find median
	ValidRTTs.Sort();

	int32 MedianIndex = ValidRTTs.Num() / 2;
	if (ValidRTTs.Num() % 2 == 0 && ValidRTTs.Num() > 1)
	{
		// Even number: average the two middle values
		return (ValidRTTs[MedianIndex - 1] + ValidRTTs[MedianIndex]) / 2.0;
	}
	else
	{
		return ValidRTTs[MedianIndex];
	}
}

void USimpleTimeSynchronizerExtended::UpdateRTTStatistics()
{
	if (SyncSamples.Num() == 0)
	{
		return;
	}

	// Calculate median RTT (more robust than mean)
	CurrentRTT = CalculateMedianRTT();

	// Calculate variance
	TArray<double> ValidRTTs;
	for (const FSyncSample& Sample : SyncSamples)
	{
		if (Sample.bIsValid)
		{
			ValidRTTs.Add(Sample.RTT);
		}
	}

	if (ValidRTTs.Num() < 2)
	{
		RTTVariance = 0.0;
		return;
	}

	// Calculate mean for variance calculation
	double Mean = 0.0;
	for (double RTT : ValidRTTs)
	{
		Mean += RTT;
	}
	Mean /= ValidRTTs.Num();

	// Calculate variance
	double Variance = 0.0;
	for (double RTT : ValidRTTs)
	{
		double Diff = RTT - Mean;
		Variance += Diff * Diff;
	}
	RTTVariance = Variance / ValidRTTs.Num();
}

void USimpleTimeSynchronizerExtended::RejectOutliers()
{
	if (SyncSamples.Num() < 3)
	{
		// Need at least 3 samples to meaningfully reject outliers
		return;
	}

	// Judge every sample afresh against the median RTT of the whole window. Judging only against the samples that
	// are still valid makes rejection permanent: after a lasting RTT increase every new sample is an "outlier" of
	// the old ones, and once those roll out of the buffer no valid sample is left to ever correct the offset.
	TArray<double> AllRTTs;
	AllRTTs.Reserve(SyncSamples.Num());
	for (const FSyncSample& Sample : SyncSamples)
	{
		AllRTTs.Add(Sample.RTT);
	}
	AllRTTs.Sort();
	const int32 MedianIndex = AllRTTs.Num() / 2;
	const double MedianRTT = AllRTTs.Num() % 2 == 0
		? (AllRTTs[MedianIndex - 1] + AllRTTs[MedianIndex]) / 2.0
		: AllRTTs[MedianIndex];

	// Reject samples with RTT significantly higher than median
	// Use a floor to prevent zero-trap where early samples with low RTT reject everything
	const double MinThreshold = 0.01; // 10ms minimum threshold
	const double OutlierThreshold = FMath::Max(MedianRTT * OutlierRTTMultiplier, MinThreshold);

	for (FSyncSample& Sample : SyncSamples)
	{
		Sample.bIsValid = Sample.RTT <= OutlierThreshold;
	}
}

void USimpleTimeSynchronizerExtended::UpdateAdaptiveSyncInterval()
{
	if (TotalSamplesTaken < InitialBurstSamples)
	{
		// Still in burst mode
		return;
	}

	// Adapt based on RTT variance (higher variance = sync more frequently)
	// Low variance (< 0.001): use max interval
	// High variance (> 0.01): use min interval

	float NormalizedVariance = FMath::Clamp(RTTVariance / 0.01, 0.0, 1.0);

	// Invert: high variance -> low interval (more frequent syncs)
	float IntervalFactor = 1.0f - NormalizedVariance;

	CurrentSyncInterval = FMath::Lerp(MinSyncInterval, MaxSyncInterval, IntervalFactor);
}

double USimpleTimeSynchronizerExtended::GetLocalTime() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

bool USimpleTimeSynchronizerExtended::ContradictsCurrentEstimate(const FSyncSample& Sample) const
{
	return FMath::Abs(Sample.ClockOffset - CurrentClockOffset) > Sample.RTT * 0.5 + ClockStepTolerance;
}


