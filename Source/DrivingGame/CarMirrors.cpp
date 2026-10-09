#include "CarMirrors.h"

#include "Camera/CameraComponent.h"
#include "CarPawn.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
TAutoConsoleVariable<int32> CVarMirrors(TEXT("tg.Mirrors"), 1,
	TEXT("Mirror captures: 1 = on, 0 = off (the glass shows black)."), ECVF_Default);

TAutoConsoleVariable<int32> CVarMirrorShadows(TEXT("tg.Mirrors.Shadows"), 0,
	TEXT("Dynamic shadows in the mirror captures: 1 = on, 0 = off (cheaper, but flatter)."), ECVF_Default);

TAutoConsoleVariable<float> CVarMirrorResolutionScale(TEXT("tg.Mirrors.ResolutionScale"), 1.f,
	TEXT("Scales the resolution of all mirror captures."), ECVF_Default);

TAutoConsoleVariable<int32> CVarMirrorSkyLight(TEXT("tg.Mirrors.SkyLight"), 1,
	TEXT("Sky light ambient in the mirror captures: 1 = on, 0 = off (cheaper, but shaded surfaces turn black)."), ECVF_Default);

TAutoConsoleVariable<int32> CVarMirrorProfile(TEXT("tg.Mirrors.CaptureAllEveryFrame"), 0,
	TEXT("Profiling aid: refresh every visible mirror on every frame, ignoring the update rates."), ECVF_Default);

/** Distance of the capture's near plane behind the glass plane, so the glass rim itself is not drawn, cm. */
constexpr float MirrorNearClipMarginCm = 0.5f;
}

UCarMirrorSettings::UCarMirrorSettings()
{
	GlassMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Vehicles/Materials/M_MirrorGlass.M_MirrorGlass")));
}

UCarMirrorsComponent::UCarMirrorsComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
}

ACarPawn* UCarMirrorsComponent::GetCar() const
{
	return Cast<ACarPawn>(GetOwner());
}

void UCarMirrorsComponent::BeginPlay()
{
	Super::BeginPlay();

	const UCarMirrorSettings* Settings = GetDefault<UCarMirrorSettings>();
	UStaticMesh* QuadMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	UMaterialInterface* GlassMaterial = Settings->GlassMaterial.LoadSynchronous();
	if (!QuadMesh || !GetCar())
	{
		return;
	}
	for (const FCarMirrorDefinition& Mirror : Settings->Mirrors)
	{
		CreateMirror(Mirror, QuadMesh, GlassMaterial);
	}
}

void UCarMirrorsComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (FMirror& Mirror : MirrorList)
	{
		if (Mirror.Glass)
		{
			Mirror.Glass->DestroyComponent();
		}
		if (Mirror.Capture)
		{
			Mirror.Capture->DestroyComponent();
		}
	}
	MirrorList.Reset();
	Super::EndPlay(EndPlayReason);
}

void UCarMirrorsComponent::CreateMirror(const FCarMirrorDefinition& Settings, UStaticMesh* QuadMesh, UMaterialInterface* GlassMaterial)
{
	ACarPawn* Car = GetCar();
	USkeletalMeshComponent* CarMesh = Car->GetMesh();

	FMirror& Mirror = MirrorList.AddDefaulted_GetRef();
	Mirror.Settings = Settings;

	// The quad's local X is the viewer's right and Z the glass normal (the plane mesh's visible side).
	const FVector Normal = Settings.Normal.GetSafeNormal();
	const FVector Up = (FVector::UpVector - Normal * Normal.Z).GetSafeNormal();
	const FVector Right = FVector::CrossProduct(Up, -Normal);
	Mirror.Glass = NewObject<UStaticMeshComponent>(Car, *FString::Printf(TEXT("MirrorGlass_%s"), *Settings.Name.ToString()));
	Mirror.Glass->SetStaticMesh(QuadMesh);
	Mirror.Glass->SetupAttachment(CarMesh);
	Mirror.Glass->SetRelativeLocationAndRotation(Settings.Center + Normal * Settings.LiftCm, FRotationMatrix::MakeFromXZ(Right, Normal).Rotator());
	Mirror.Glass->SetRelativeScale3D(FVector(Settings.WidthCm / 100.f, Settings.HeightCm / 100.f, 1.f));
	Mirror.Glass->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mirror.Glass->SetCastShadow(false);
	Mirror.Glass->bAffectDistanceFieldLighting = false;
	Mirror.Glass->RegisterComponent();

	Mirror.RenderTarget = NewObject<UTextureRenderTarget2D>(this);
	Mirror.RenderTarget->RenderTargetFormat = RTF_RGBA16f;
	Mirror.RenderTarget->bAutoGenerateMips = false;
	Mirror.RenderTarget->InitAutoFormat(Settings.ResolutionX, Settings.ResolutionY);
	Mirror.RenderTarget->UpdateResourceImmediate(true);

	if (GlassMaterial)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(GlassMaterial, this);
		Instance->SetTextureParameterValue(TEXT("MirrorImage"), Mirror.RenderTarget);
		Instance->SetScalarParameterValue(TEXT("AspectRatio"), Settings.WidthCm / Settings.HeightCm);
		Instance->SetScalarParameterValue(TEXT("CornerRadiusCm"), Settings.CornerRadiusCm);
		Instance->SetScalarParameterValue(TEXT("HeightCm"), Settings.HeightCm);
		Mirror.Glass->SetMaterial(0, Instance);
	}

	Mirror.Capture = NewObject<USceneCaptureComponent2D>(Car, *FString::Printf(TEXT("MirrorCapture_%s"), *Settings.Name.ToString()));
	Mirror.Capture->TextureTarget = Mirror.RenderTarget;
	Mirror.Capture->bCaptureEveryFrame = false;
	Mirror.Capture->bCaptureOnMovement = false;
	Mirror.Capture->bAlwaysPersistRenderingState = true;
	Mirror.Capture->CaptureSource = SCS_SceneColorHDR;
	Mirror.Capture->bUseCustomProjectionMatrix = true;
	Mirror.Capture->MaxViewDistanceOverride = GetDefault<UCarMirrorSettings>()->MaxViewDistanceCm;
	Mirror.Capture->bUseRayTracingIfEnabled = false;
	ApplyCheapShowFlags(*Mirror.Capture);
	Mirror.Capture->HideComponent(Mirror.Glass);
	Mirror.Capture->SetupAttachment(CarMesh);
	Mirror.Capture->RegisterComponent();
}

void UCarMirrorsComponent::ApplyCheapShowFlags(USceneCaptureComponent2D& Capture) const
{
	FEngineShowFlags& Flags = Capture.ShowFlags;
	Flags.SetAntiAliasing(false);
	Flags.SetAmbientOcclusion(false);
	Flags.SetBloom(false);
	Flags.SetMotionBlur(false);
	Flags.SetDepthOfField(false);
	Flags.SetEyeAdaptation(false);
	Flags.SetScreenSpaceReflections(false);
	Flags.SetVolumetricFog(false);
	Flags.SetLumenGlobalIllumination(false);
	Flags.SetLumenReflections(false);
	Flags.SetLensFlares(false);
	Flags.SetGrain(false);
	Flags.SetVignette(false);
	Flags.SetDecals(false);
	Flags.SetParticles(false);
}

bool UCarMirrorsComponent::PrepareCapture(FMirror& Mirror, const FVector& EyeLocation) const
{
	const FTransform& CarTransform = GetCar()->GetMesh()->GetComponentTransform();
	const FCarMirrorDefinition& Settings = Mirror.Settings;

	const FVector GlassNormal = CarTransform.TransformVectorNoScale(Settings.Normal.GetSafeNormal());
	const FVector GlassRight = FVector::CrossProduct(FVector::UpVector, -GlassNormal).GetSafeNormal();
	const FVector GlassUp = FVector::CrossProduct(-GlassNormal, GlassRight);
	const FVector Centre = CarTransform.TransformPosition(Settings.Center) + GlassNormal * Settings.LiftCm;
	const FVector AimNormal = Settings.AimNormal.IsNearlyZero()
		? GlassNormal
		: CarTransform.TransformVectorNoScale(Settings.AimNormal.GetSafeNormal());

	const float EyeDistance = static_cast<float>(FVector::DotProduct(EyeLocation - Centre, AimNormal));
	if (EyeDistance < 5.f || FVector::DotProduct(EyeLocation - Centre, GlassNormal) < 5.f)
	{
		return false; // the driver is behind the glass (looking at the back of the mirror)
	}

	// The reflected eye looks through the glass towards the aim normal's side, with the aim plane as its image plane.
	const FVector ReflectedEye = EyeLocation - 2.0 * EyeDistance * AimNormal;
	const FVector CarUp = CarTransform.TransformVectorNoScale(FVector::UpVector);
	const FRotator Rotation = FRotationMatrix::MakeFromXZ(AimNormal, CarUp).Rotator();
	Mirror.Capture->SetWorldLocationAndRotation(ReflectedEye, Rotation);
	const FRotationMatrix Axes(Rotation);
	const FVector CameraRight = Axes.GetScaledAxis(EAxis::Y);
	const FVector CameraUp = Axes.GetScaledAxis(EAxis::Z);

	// Window: the glass outline as the driver sees it, carried onto the aim plane, in the capture's view space.
	FVector2D WindowMin(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	FVector2D WindowMax(-TNumericLimits<double>::Max(), -TNumericLimits<double>::Max());
	const double CornerSigns[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
	for (const double* Sign : CornerSigns)
	{
		const FVector Corner = Centre + GlassRight * (0.5 * Settings.WidthCm * Sign[0]) + GlassUp * (0.5 * Settings.HeightCm * Sign[1]);
		const FVector RayToCorner = Corner - EyeLocation;
		const double AlongAim = -FVector::DotProduct(RayToCorner, AimNormal); // the aim normal points at the driver
		if (AlongAim < 1.0)
		{
			return false;
		}
		const FVector OnAimPlane = EyeLocation + RayToCorner * (EyeDistance / AlongAim);
		const FVector FromReflectedEye = OnAimPlane - ReflectedEye;
		const FVector2D InView(FVector::DotProduct(FromReflectedEye, CameraRight), FVector::DotProduct(FromReflectedEye, CameraUp));
		WindowMin = FVector2D(FMath::Min(WindowMin.X, InView.X), FMath::Min(WindowMin.Y, InView.Y));
		WindowMax = FVector2D(FMath::Max(WindowMax.X, InView.X), FMath::Max(WindowMax.Y, InView.Y));
	}
	const FVector2D WindowCentre = 0.5 * (WindowMin + WindowMax);
	const FVector2D HalfSize = 0.5 * (WindowMax - WindowMin) * Settings.FieldScale;
	const float Left = static_cast<float>((WindowCentre.X - HalfSize.X) / EyeDistance);
	const float Right = static_cast<float>((WindowCentre.X + HalfSize.X) / EyeDistance);
	const float Bottom = static_cast<float>((WindowCentre.Y - HalfSize.Y) / EyeDistance);
	const float Top = static_cast<float>((WindowCentre.Y + HalfSize.Y) / EyeDistance);

	// Infinite reversed-Z off-axis perspective, same convention as FReversedZPerspectiveMatrix.
	const float NearPlane = EyeDistance + MirrorNearClipMarginCm;
	Mirror.Capture->CustomProjectionMatrix = FMatrix(
		FPlane(2.f / (Right - Left), 0.f, 0.f, 0.f),
		FPlane(0.f, 2.f / (Top - Bottom), 0.f, 0.f),
		FPlane(-(Right + Left) / (Right - Left), -(Top + Bottom) / (Top - Bottom), 0.f, 1.f),
		FPlane(0.f, 0.f, NearPlane, 0.f));
	return true;
}

void UCarMirrorsComponent::ApplyPerCaptureSettings(FMirror& Mirror) const
{
	Mirror.Capture->ShowFlags.SetDynamicShadows(CVarMirrorShadows.GetValueOnGameThread() != 0);
	Mirror.Capture->ShowFlags.SetSkyLighting(CVarMirrorSkyLight.GetValueOnGameThread() != 0);

	const float ResolutionScale = FMath::Clamp(CVarMirrorResolutionScale.GetValueOnGameThread(), 0.25f, 4.f);
	const int32 TargetX = FMath::Max(16, FMath::RoundToInt(Mirror.Settings.ResolutionX * ResolutionScale));
	const int32 TargetY = FMath::Max(16, FMath::RoundToInt(Mirror.Settings.ResolutionY * ResolutionScale));
	if (Mirror.RenderTarget->SizeX != TargetX || Mirror.RenderTarget->SizeY != TargetY)
	{
		Mirror.RenderTarget->ResizeTarget(TargetX, TargetY);
	}
}

bool UCarMirrorsComponent::IsInView(const FMirror& Mirror, const UCameraComponent& Camera) const
{
	const FVector MirrorLocation = GetCar()->GetMesh()->GetComponentTransform().TransformPosition(Mirror.Settings.Center);
	const FVector ToMirror = (MirrorLocation - Camera.GetComponentLocation()).GetSafeNormal();
	const double CosineLimit = FMath::Cos(FMath::DegreesToRadians(GetDefault<UCarMirrorSettings>()->SkipOutsideViewDegrees));
	return FVector::DotProduct(Camera.GetForwardVector(), ToMirror) > CosineLimit;
}

void UCarMirrorsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (MirrorList.IsEmpty() || CVarMirrors.GetValueOnGameThread() == 0)
	{
		return;
	}
	const UCameraComponent* Camera = GetOwner()->FindComponentByClass<UCameraComponent>();
	if (!Camera)
	{
		return;
	}

	// One capture per frame at most: take the next mirror that is due and in the driver's view.
	const double Now = GetWorld()->GetTimeSeconds();
	const int32 Count = MirrorList.Num();
	const bool bCaptureAll = CVarMirrorProfile.GetValueOnGameThread() != 0;
	for (int32 Step = 0; Step < Count; ++Step)
	{
		FMirror& Mirror = MirrorList[(NextMirror + Step) % Count];
		const double Interval = 1.0 / FMath::Max(1.f, Mirror.Settings.UpdateRateHz);
		if (!bCaptureAll && (Now - Mirror.LastCaptureTime < Interval || !IsInView(Mirror, *Camera)))
		{
			continue;
		}
		NextMirror = (NextMirror + Step + 1) % Count;
		Mirror.LastCaptureTime = Now;
		if (PrepareCapture(Mirror, Camera->GetComponentLocation()))
		{
			ApplyPerCaptureSettings(Mirror);
			Mirror.Capture->CaptureScene();
		}
		if (!bCaptureAll)
		{
			return;
		}
	}
}
