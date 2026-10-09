#include "CloudSky.h"

#include "Components/PrimitiveComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "DynamicRHI.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Camera/PlayerCameraManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogCloudSky, Log, All);

static TAutoConsoleVariable<int32> CVarCloudSky(
	TEXT("tg.CloudSky"), 1, TEXT("1 shows the dome with the clouds of the sky light's real-time capture, 0 hides it and shows the plain atmosphere."), ECVF_Default);
static TAutoConsoleVariable<int32> CVarCloudSkyResolution(
	TEXT("tg.CloudSkyResolution"), 1024, TEXT("Edge length of the sky capture cube faces in pixels."), ECVF_Default);

namespace CloudSkyDetail
{
	/** The rig the console commands act on. */
	TWeakObjectPtr<UCloudSkyRig> ActiveRig;

	constexpr const TCHAR* CloudMaterialPath = TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst");
	constexpr const TCHAR* DomeMaterialPath = TEXT("/Game/World/Sky/M_SkyDome");
	constexpr const TCHAR* SphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");

	/** The dome stays behind every tile of the world, which reaches 60 km (the flat ground ring). */
	constexpr float DomeRadiusCentimetres = 12000000.f;
	/** The engine sphere has a radius of 50 cm at scale 1. */
	constexpr float EngineSphereRadiusCentimetres = 50.f;

	/** Cloud layer in kilometres above the ground: fair-weather cumulus and stratus sit low and thin. */
	constexpr float LayerBottomKilometres = 1.5f;
	constexpr float LayerHeightKilometres = 4.f;

	/** Cloud cover and the material's Cloud_GlobalCoverage that produces it; measured, the response is steep near a closed deck. */
	struct FCoveragePoint
	{
		float Cover;
		float MaterialCoverage;
	};
	constexpr FCoveragePoint CoverageCurve[] = {
		{0.00f, -0.40f},  // clear
		{0.15f, -0.24f},  // a few small cumulus
		{0.30f, -0.16f},  // fair-weather cumulus
		{0.45f, -0.12f},
		{0.60f, -0.09f},  // broken: about half the sky
		{0.75f, -0.05f},
		{0.85f, 0.00f},   // nearly closed
		{1.00f, 0.12f},   // a closed deck with soft structure
	};

	/** Cloud_GlobalDensity of scattered cumulus and of a thick deck; the deck is denser and brighter. */
	constexpr float CumulusDensity = 0.008f;
	constexpr float DeckDensity = 0.016f;

	/** Noise_Strength: the deck gets stronger fine detail so that it reads as soft structure rather than a flat grey. */
	const FLinearColor CumulusNoiseStrength(0.8f, 0.08f, 0.03f, 2.5f);
	const FLinearColor DeckNoiseStrength(0.8f, 0.25f, 0.12f, 2.5f);

	/** Cloud_AlbedoColor of fair-weather cloud and of rain cloud, which is thicker and so darker underneath. */
	const FLinearColor FairAlbedo(0.98f, 0.98f, 0.98f, 0.5f);
	const FLinearColor RainAlbedo(0.62f, 0.64f, 0.67f, 0.5f);

	/** Material coverage for a weather cloud cover, by interpolating CoverageCurve. */
	float MaterialCoverageFor(float Cover)
	{
		const int32 Count = UE_ARRAY_COUNT(CoverageCurve);
		for (int32 Index = 1; Index < Count; ++Index)
		{
			if (Cover <= CoverageCurve[Index].Cover)
			{
				const float Span = CoverageCurve[Index].Cover - CoverageCurve[Index - 1].Cover;
				const float Alpha = (Cover - CoverageCurve[Index - 1].Cover) / Span;
				return FMath::Lerp(CoverageCurve[Index - 1].MaterialCoverage, CoverageCurve[Index].MaterialCoverage, Alpha);
			}
		}
		return CoverageCurve[Count - 1].MaterialCoverage;
	}

	/** 0 to 1 over the given interval. */
	float SmoothRamp(float From, float To, float Value)
	{
		const float Alpha = FMath::Clamp((Value - From) / (To - From), 0.f, 1.f);
		return Alpha * Alpha * (3.f - 2.f * Alpha);
	}

	/**
	 * Lowest face resolution the capture has to be set to; the sky light's capture also sets the resolution of its
	 * convolution, so the cost grows with the square of it.
	 */
	constexpr TCHAR CaptureResolutionVariable[] = TEXT("r.SkyLight.RealTimeReflectionCapture.ResolutionOverride");
}

static FAutoConsoleCommand CloudSkyParamCommand(
	TEXT("CloudSky.Param"), TEXT("CloudSky.Param <name> <1 to 4 numbers>: overrides a cloud material parameter (1 number = scalar)."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Arguments)
	{
		UCloudSkyRig* Rig = CloudSkyDetail::ActiveRig.Get();
		if (!Rig || Arguments.Num() < 2)
		{
			return;
		}
		FLinearColor Value(0.f, 0.f, 0.f, 0.f);
		for (int32 Index = 1; Index < FMath::Min(Arguments.Num(), 5); ++Index)
		{
			Value.Component(Index - 1) = FCString::Atof(*Arguments[Index]);
		}
		Rig->SetDebugParameter(FName(*Arguments[0]), Value, Arguments.Num() == 2);
	}));

static FAutoConsoleCommand CloudSkyDumpCommand(
	TEXT("CloudSky.Dump"), TEXT("Logs the cloud material's parameters."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		if (UCloudSkyRig* Rig = CloudSkyDetail::ActiveRig.Get())
		{
			Rig->LogParameters();
		}
	}));

bool UCloudSkyRig::Initialise(UWorld* World)
{
	if (!World || FParse::Param(FCommandLine::Get(), TEXT("NoCloudSky")))
	{
		return false;
	}
	UMaterialInterface* CloudBase = LoadObject<UMaterialInterface>(nullptr, CloudSkyDetail::CloudMaterialPath, nullptr, LOAD_NoWarn);
	UMaterialInterface* DomeBase = LoadObject<UMaterialInterface>(nullptr, CloudSkyDetail::DomeMaterialPath, nullptr, LOAD_NoWarn);
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, CloudSkyDetail::SphereMeshPath, nullptr, LOAD_NoWarn);
	if (!CloudBase || !DomeBase || !Sphere)
	{
		UE_LOG(LogCloudSky, Warning, TEXT("Cloud sky off: %s missing (run Scripts/create_cloud_sky.py)"), DomeBase ? CloudSkyDetail::CloudMaterialPath : CloudSkyDetail::DomeMaterialPath);
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.bDeferConstruction = true;
	AVolumetricCloud* Clouds = World->SpawnActor<AVolumetricCloud>(SpawnParameters);
	UVolumetricCloudComponent* CloudComponent = Clouds->FindComponentByClass<UVolumetricCloudComponent>();
	CloudMaterial = UMaterialInstanceDynamic::Create(CloudBase, this);
	CloudComponent->SetMaterial(CloudMaterial);
	CloudComponent->SetLayerBottomAltitude(CloudSkyDetail::LayerBottomKilometres);
	CloudComponent->SetLayerHeight(CloudSkyDetail::LayerHeightKilometres);
	// Traced only into the sky light capture; the dome shows the result, so the main views never trace clouds.
	CloudComponent->SetRenderInMainPass(false);
	Clouds->FinishSpawning(FTransform::Identity);
	CloudActor = Clouds;

	if (FParse::Param(FCommandLine::Get(), TEXT("CloudSkyNoDome")))
	{
		return true;
	}
	AStaticMeshActor* DomeActor = World->SpawnActor<AStaticMeshActor>(SpawnParameters);
	UStaticMeshComponent* DomeMesh = DomeActor->GetStaticMeshComponent();
	DomeMesh->SetMobility(EComponentMobility::Movable);
	DomeMesh->SetStaticMesh(Sphere);
	DomeMaterial = UMaterialInstanceDynamic::Create(DomeBase, this);
	DomeMesh->SetMaterial(0, DomeMaterial);
	DomeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	DomeMesh->SetCastShadow(false);
	DomeMesh->bAffectDynamicIndirectLighting = false;
	DomeMesh->bAffectDistanceFieldLighting = false;
	DomeMesh->bVisibleInRayTracing = false;
	DomeMesh->bVisibleInReflectionCaptures = false;
	// With a sky mesh in the scene the engine draws only sky meshes into the capture, so the dome is in it too; its
	// material draws the atmosphere there and the capture's own cubemap in the views (see create_cloud_sky.py).
	DomeMesh->bVisibleInRealTimeSkyCaptures = true;
	DomeMesh->SetWorldScale3D(FVector(CloudSkyDetail::DomeRadiusCentimetres / CloudSkyDetail::EngineSphereRadiusCentimetres));
	Dome = DomeActor;

	// Height fog also covers sky material meshes, and at the dome's distance it would swallow the sky completely.
	// Nothing in the world is further than the dome's inner 80 %, so cutting the fog off there only spares the dome.
	for (TActorIterator<AExponentialHeightFog> FogIt(World); FogIt; ++FogIt)
	{
		FogIt->GetComponent()->SetFogCutoffDistance(CloudSkyDetail::DomeRadiusCentimetres * 0.8f);
	}
	CloudSkyDetail::ActiveRig = this;
	ConfigureCapture();
	UE_LOG(LogCloudSky, Log, TEXT("Cloud sky on: capture %d px per face"), CVarCloudSkyResolution.GetValueOnGameThread());
	return true;
}

void UCloudSkyRig::ConfigureCapture()
{
	if (IConsoleVariable* Resolution = IConsoleManager::Get().FindConsoleVariable(CloudSkyDetail::CaptureResolutionVariable))
	{
		Resolution->Set(CVarCloudSkyResolution.GetValueOnGameThread(), ECVF_SetByCode);
	}
	// One cloud face per frame instead of two: the capture's cost per frame is halved at the price of a refresh
	// that takes twice as many frames, which the slowly changing sky does not show.
	if (IConsoleVariable* FacesPerFrame = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice.SkyCloudCubeFacePerFrame")))
	{
		FacesPerFrame->Set(1, ECVF_SetByCode);
	}
	// Without the volumetric render target the main views skip the cloud pass altogether when the cloud is not
	// rendered in the main pass; with it they would still trace and only skip the composite.
	IConsoleVariable* RenderTarget = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricRenderTarget"));
	if (RenderTarget && !FParse::Param(FCommandLine::Get(), TEXT("CloudSkyKeepVrt")))
	{
		RenderTarget->Set(0, ECVF_SetByCode);
	}
}

void UCloudSkyRig::Update(const FCloudSkyInputs& Inputs)
{
	AStaticMeshActor* DomeActor = Dome.Get();
	UWorld* World = DomeActor ? DomeActor->GetWorld() : nullptr;
	if (!World)
	{
		return;
	}
	const bool bShowDome = CVarCloudSky.GetValueOnGameThread() != 0;
	DomeActor->SetActorHiddenInGame(!bShowDome);
	if (APlayerController* Controller = World->GetFirstPlayerController())
	{
		if (Controller->PlayerCameraManager)
		{
			DomeActor->SetActorLocation(Controller->PlayerCameraManager->GetCameraLocation());
		}
	}
	DomeMaterial->SetVectorParameterValue(TEXT("SunDirection"), FLinearColor(Inputs.SunDirection.X, Inputs.SunDirection.Y, Inputs.SunDirection.Z, 0.f));
	DomeMaterial->SetScalarParameterValue(TEXT("SunVisibility"), Inputs.SunVisibility);
	if (!CloudMaterial)
	{
		return;
	}
	const float Cover = FMath::Clamp(Inputs.CloudCover, 0.f, 1.f);
	const float Deck = CloudSkyDetail::SmoothRamp(0.8f, 1.f, Cover);
	CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalCoverage"), CloudSkyDetail::MaterialCoverageFor(Cover));
	CloudMaterial->SetScalarParameterValue(TEXT("Cloud_GlobalDensity"), FMath::Lerp(CloudSkyDetail::CumulusDensity, CloudSkyDetail::DeckDensity, Deck));
	CloudMaterial->SetVectorParameterValue(TEXT("Noise_Strength"), FMath::Lerp(CloudSkyDetail::CumulusNoiseStrength, CloudSkyDetail::DeckNoiseStrength, Deck));
	CloudMaterial->SetVectorParameterValue(TEXT("Cloud_AlbedoColor"), FMath::Lerp(CloudSkyDetail::FairAlbedo, CloudSkyDetail::RainAlbedo, Inputs.RainIntensity));
	for (const TPair<FName, FDebugParameter>& Parameter : DebugParameters)
	{
		if (Parameter.Value.bScalar)
		{
			CloudMaterial->SetScalarParameterValue(Parameter.Key, Parameter.Value.Value.R);
		}
		else
		{
			CloudMaterial->SetVectorParameterValue(Parameter.Key, Parameter.Value.Value);
		}
	}
}

void UCloudSkyRig::SetDebugParameter(FName Name, const FLinearColor& Value, bool bScalar)
{
	DebugParameters.Add(Name, FDebugParameter{Value, bScalar});
}

void UCloudSkyRig::LogParameters() const
{
	if (!CloudMaterial)
	{
		return;
	}
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Guids;
	CloudMaterial->GetAllScalarParameterInfo(Infos, Guids);
	for (const FMaterialParameterInfo& Info : Infos)
	{
		float Value = 0.f;
		CloudMaterial->GetScalarParameterValue(Info, Value);
		UE_LOG(LogCloudSky, Log, TEXT("scalar %s = %g"), *Info.Name.ToString(), Value);
	}
	CloudMaterial->GetAllVectorParameterInfo(Infos, Guids);
	for (const FMaterialParameterInfo& Info : Infos)
	{
		FLinearColor Value;
		CloudMaterial->GetVectorParameterValue(Info, Value);
		UE_LOG(LogCloudSky, Log, TEXT("vector %s = %g %g %g %g"), *Info.Name.ToString(), Value.R, Value.G, Value.B, Value.A);
	}
}

void CloudSkyFrameLog::Record(UWorld* World)
{
	static float WindowSeconds = -1.f;
	static TArray<float> FrameMilliseconds;
	static double WindowStart = 0.0;
	if (WindowSeconds < 0.f)
	{
		WindowSeconds = 0.f;
		FParse::Value(FCommandLine::Get(), TEXT("GpuFrameLog="), WindowSeconds);
	}
	if (WindowSeconds <= 0.f || !World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (WindowStart == 0.0)
	{
		WindowStart = Now;
	}
	FrameMilliseconds.Add(float(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles())));
	if (Now - WindowStart < WindowSeconds)
	{
		return;
	}
	FrameMilliseconds.Sort();
	double Sum = 0.0;
	for (const float Value : FrameMilliseconds)
	{
		Sum += Value;
	}
	const int32 Count = FrameMilliseconds.Num();
	UE_LOG(LogCloudSky, Display, TEXT("GPUFRAMES t=%.0f s: %d frames, average %.2f ms, median %.2f ms, 99th %.2f ms, max %.2f ms"),
		Now, Count, Sum / Count, FrameMilliseconds[Count / 2], FrameMilliseconds[FMath::Min(Count - 1, int32(Count * 0.99f))], FrameMilliseconds.Last());
	FrameMilliseconds.Reset();
	WindowStart = Now;
}
