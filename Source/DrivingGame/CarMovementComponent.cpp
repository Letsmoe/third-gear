#include "CarMovementComponent.h"
#include "SnowTrackSubsystem.h"

#include "CarDrivetrain.h"
#include "CarSettings.h"
#include "CarSurfaceGrip.h"
#include "WeatherVisuals.h"
#include "WorldSurfaceQuery.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "CarWheels.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "Physics/PhysicsInterfaceCore.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"

DEFINE_LOG_CATEGORY_STATIC(LogCar, Log, All);

namespace
{
/** Friction of UE's default physical material; surfaces with other values scale tyre grip relative to dry asphalt. */
constexpr float DefaultSurfaceFriction = 0.7f;

/**
 * Physics-thread part of the car. Replaces Chaos' engine/transmission/tyre model with FCarDrivetrain;
 * keeps Chaos' suspension. Everything in here runs on the physics thread (async physics, fixed 2 ms steps).
 */
class FCarVehicleSimulation final : public UChaosWheeledVehicleSimulation
{
public:
	FCarVehicleSimulation(const FCarSimParams& InParams, TSharedRef<FCarSharedState, ESPMode::ThreadSafe> InShared)
		: Params(InParams)
		, Shared(InShared)
	{
		Drivetrain.Init(Params);
		Drivetrain.Reset(true);
	}

	virtual void ApplyInput(const FControlInputs& ControlInputs, float DeltaTime) override
	{
		// Chaos' own control inputs (throttle/brake/steering from the movement component) are not used.
		FScopeLock Lock(&Shared->Lock);
		Input = Shared->Input;
		for (int32 i = 0; i < CarNumWheels; ++i)
		{
			Surface[i] = Shared->Surface[i];
		}
		if (Shared->ResetCounter != SeenResetCounter)
		{
			SeenResetCounter = Shared->ResetCounter;
			Drivetrain.Reset(Shared->bResetEngineRunning);
			bHavePrevVelocity = false;
		}
	}

	virtual void ProcessMechanicalSimulation(float DeltaTime) override {}

	/**
	 * Chaos' force-based suspension, except that the spring force reaches the body along the ground normal: the
	 * road can only push perpendicular to itself (plus tyre friction); the horizontal part of a spring force along a
	 * pitched body's axis is taken by the suspension links in reality. Applying it to the body (as Chaos does) acted
	 * like ~1-2 % of the car's weight as a brake whenever the body pitched.
	 */
	virtual void ApplySuspensionForces(float DeltaTime, TArray<FWheelTraceParams>& WheelTraceParams) override
	{
		const int32 NumWheels = PVehicle->Suspension.Num();
		TArray<float, TInlineAllocator<CarNumWheels>> SpringForces;
		SpringForces.Init(0.f, NumWheels);
		for (int32 i = 0; i < NumWheels; ++i)
		{
			const FHitResult& Hit = WheelState.TraceResult[i];
			Chaos::FSimpleWheelSim& PWheel = PVehicle->Wheels[i];
			Chaos::FSimpleSuspensionSim& PSuspension = PVehicle->Suspension[i];
			if (!PWheel.InContact())
			{
				PSuspension.SetSuspensionLength(PSuspension.GetTraceLength(PWheel.GetEffectiveRadius()), PWheel.Setup().WheelRadius);
				PWheel.SetWheelLoadForce(0.f);
				continue;
			}
			PSuspension.SetSuspensionLength(Hit.Distance, PWheel.GetEffectiveRadius());
			PSuspension.SetLocalVelocity(WheelState.LocalWheelVelocity[i]);
			PSuspension.Simulate(DeltaTime);
			const float Force = FMath::Max(0.f, PSuspension.GetSuspensionForce());
			const FVector Point = WheelState.WheelWorldLocation[i] + PSuspension.Setup().SuspensionForceOffset;
			AddForceAtPosition(Hit.ImpactNormal * Force, Point);
			PWheel.SetWheelLoadForce(Force);
			PWheel.SetMassPerWheel(RigidHandle->M() / NumWheels);
			SpringForces[i] = Force;
		}

		// Anti-roll bars: move load from the less to the more compressed side of an axle (pure roll moment).
		for (const Chaos::FAxleSim& Axle : PVehicle->GetAxles())
		{
			if (Axle.Setup.WheelIndex.Num() != 2)
			{
				continue;
			}
			const int32 A = Axle.Setup.WheelIndex[0];
			const int32 B = Axle.Setup.WheelIndex[1];
			const FVector Force = VehicleState.VehicleUpAxis * (SpringForces[A] - SpringForces[B]) * Axle.Setup.RollbarScaling;
			AddForceAtPosition(Force, WheelState.WheelWorldLocation[A] + PVehicle->Suspension[A].Setup().SuspensionForceOffset);
			AddForceAtPosition(-Force, WheelState.WheelWorldLocation[B] + PVehicle->Suspension[B].Setup().SuspensionForceOffset);
		}
	}

	virtual void ProcessSteering(const FControlInputs& ControlInputs) override
	{
		CacheGeometry();
		MeanSteerDeg = FMath::Clamp(Input.SteeringWheelDeg / FMath::Max(1.f, Params.SteeringRatio), -Params.MaxRoadWheelAngleDeg, Params.MaxRoadWheelAngleDeg);
		// Compliance steer from the (low-pass filtered, to avoid a fast feedback loop) front side force.
		MeanSteerDeg -= Params.ComplianceSteerDegPerKN * FrontSideForceFiltered * 0.001f;
		const float Delta = FMath::DegreesToRadians(MeanSteerDeg);
		for (int32 WheelIdx = 0; WheelIdx < PVehicle->Wheels.Num(); ++WheelIdx)
		{
			Chaos::FSimpleWheelSim& PWheel = PVehicle->Wheels[WheelIdx];
			if (!PWheel.SteeringEnabled)
			{
				PWheel.SetSteeringAngle(0.f);
				continue;
			}
			// Ackermann geometry: the inner wheel turns more than the outer one.
			float Angle = Delta;
			if (FMath::Abs(Delta) > 1e-4f && WheelbaseM > 0.f)
			{
				const float Radius = WheelbaseM / FMath::Tan(FMath::Abs(Delta));
				const bool bRightWheel = PVehicle->Suspension[WheelIdx].GetLocalRestingPosition().Y > 0.f;
				const bool bInner = (Delta > 0.f) == bRightWheel;
				const float Offset = bInner ? -0.5f * TrackM : 0.5f * TrackM;
				Angle = FMath::Sign(Delta) * FMath::Atan(WheelbaseM / FMath::Max(0.1f, Radius + Offset));
			}
			PWheel.SetSteeringAngle(FMath::RadiansToDegrees(Angle));
		}
	}

	virtual void ApplyWheelFrictionForces(float DeltaTime) override
	{
		const int32 NumWheels = FMath::Min(PVehicle->Wheels.Num(), CarNumWheels);
		FCarDrivetrain::FWheelContact Contacts[CarNumWheels];
		for (int32 i = 0; i < NumWheels; ++i)
		{
			Chaos::FSimpleWheelSim& PWheel = PVehicle->Wheels[i];
			const FHitResult& Hit = WheelState.TraceResult[i];
			if (!PWheel.InContact())
			{
				continue;
			}
			float Grip = 1.f;
			if (Hit.PhysMaterial.IsValid())
			{
				Grip = FMath::Clamp(Hit.PhysMaterial->Friction / DefaultSurfaceFriction, 0.1f, 1.5f);
			}
			const FRotator Steering(0.f, PWheel.GetSteeringAngle(), 0.f);
			const FVector WheelVelocity = Steering.UnrotateVector(WheelState.LocalWheelVelocity[i]) * 0.01f; // cm/s -> m/s
			Contacts[i].bContact = true;
			Contacts[i].Vx = WheelVelocity.X;
			Contacts[i].Vy = WheelVelocity.Y;
			Contacts[i].LoadN = FMath::Max(0.f, PWheel.GetWheelLoadForce() * 0.01f); // kg cm/s^2 -> N
			Contacts[i].Grip = Grip * Surface[i].GripScale * AquaplaningGripFactor(i, FMath::Abs(WheelVelocity.X));
			Contacts[i].PeakSlipScale = Surface[i].PeakSlipScale;
			Contacts[i].ShapeCScale = Surface[i].ShapeCScale;
			Contacts[i].ExtraRollingResistance = Surface[i].ExtraRollingResistance;
		}

		const float MassPerWheel = RigidHandle ? RigidHandle->M() / FMath::Max(1, NumWheels) : Params.MassKg / CarNumWheels;
		FCarDrivetrain::FWheelForces Forces[CarNumWheels];
		Drivetrain.Step(DeltaTime, Input, Contacts, MassPerWheel, Forces);

		RackTorque = 0.f;
		AppliedForce = FVector::ZeroVector;
		for (int32 i = 0; i < NumWheels; ++i)
		{
			Chaos::FSimpleWheelSim& PWheel = PVehicle->Wheels[i];
			const FHitResult& Hit = WheelState.TraceResult[i];

			// Wheel animation: same road speed on the (possibly differently sized) visual wheel.
			const float VisualRadius = FMath::Max(1.f, PWheel.GetEffectiveRadius());
			const float VisualOmega = Drivetrain.GetWheelOmega(i) * Params.RollingRadiusM * 100.f / VisualRadius;
			PWheel.SetAngularVelocity(VisualOmega);
			VisualAngle[i] = FMath::Fmod(VisualAngle[i] + VisualOmega * DeltaTime, UE_TWO_PI);
			PWheel.SetAngularPosition(VisualAngle[i]);

			if (!Contacts[i].bContact)
			{
				continue;
			}
			// Same ground frame as Chaos' own friction code: x along the ground in the car's heading, z = ground normal.
			const FRotator Steering(0.f, PWheel.GetSteeringAngle(), 0.f);
			const FVector ForceLocal = Steering.RotateVector(FVector(Forces[i].Fx, Forces[i].Fy, 0.f) * 100.f); // N -> kg cm/s^2
			const FVector GroundZ = Hit.Normal;
			const FVector GroundX = FVector::CrossProduct(VehicleState.VehicleRightAxis, GroundZ);
			const FVector GroundY = FVector::CrossProduct(GroundZ, GroundX);
			const FMatrix Ground(GroundX, GroundY, GroundZ, FVector::ZeroVector);
			const FVector ForceWorld = Ground.TransformVector(ForceLocal);
			AddForceAtPosition(ForceWorld, Hit.ImpactPoint);
			AppliedForce += ForceWorld;

			if (PWheel.SteeringEnabled)
			{
				RackTorque += Forces[i].Mz;
			}
		}
		RackTorque /= FMath::Max(1.f, Params.SteeringRatio);
		const float FrontSideForce = Forces[0].Fy + Forces[1].Fy;
		FrontSideForceFiltered += (FrontSideForce - FrontSideForceFiltered) * FMath::Min(1.f, DeltaTime / 0.04f);
	}

	virtual void UpdateSimulation(float DeltaTime, const FChaosVehicleAsyncInput& InputData, Chaos::FRigidBodyHandle_Internal* Handle) override
	{
		UChaosWheeledVehicleSimulation::UpdateSimulation(DeltaTime, InputData, Handle);
		if (!Handle || DeltaTime <= 0.f)
		{
			return;
		}

		// Aerodynamic drag (Chaos' own aero is disabled via DragCoefficient = 0).
		const FVector VelocityMps = VehicleState.VehicleWorldVelocity * 0.01f;
		const FVector Drag = -0.5f * Params.AirDensity * Params.DragAreaM2 * VelocityMps.Size() * VelocityMps;
		AddForce(Drag * 100.f);
		AppliedForce += Drag * 100.f;

		PublishTelemetry(DeltaTime);
	}

private:
	/**
	 * Grip left on standing water at this road speed: the front tyres float up first from about 70 km/h and are
	 * mostly gone by 90 km/h, the rear tyres follow in the front's wake with less loss.
	 */
	float AquaplaningGripFactor(int32 Wheel, float SpeedMps) const
	{
		if (Surface[Wheel].Aquaplaning <= 0.f)
		{
			return 1.f;
		}
		const float Floating = CarSurfaceGrip::Ramp(19.5f, 25.f, SpeedMps) * Surface[Wheel].Aquaplaning;
		return 1.f - (Wheel < 2 ? 0.7f : 0.35f) * Floating;
	}

	void CacheGeometry()
	{
		if (WheelbaseM > 0.f || PVehicle->Suspension.Num() < CarNumWheels)
		{
			return;
		}
		const FVector FL = PVehicle->Suspension[0].GetLocalRestingPosition();
		const FVector FR = PVehicle->Suspension[1].GetLocalRestingPosition();
		const FVector RL = PVehicle->Suspension[2].GetLocalRestingPosition();
		const FVector RR = PVehicle->Suspension[3].GetLocalRestingPosition();
		WheelbaseM = FMath::Abs(0.5f * (FL.X + FR.X) - 0.5f * (RL.X + RR.X)) * 0.01f;
		TrackM = FMath::Abs(FR.Y - FL.Y) * 0.01f;
	}

	void PublishTelemetry(float DeltaTime)
	{
		SimTime += DeltaTime;
		FCarTelemetry T;
		Drivetrain.FillTelemetry(T);
		T.SimTime = SimTime;
		const FTransform& Transform = VehicleState.VehicleWorldTransform;
		T.PositionM = Transform.GetLocation() * 0.01f;
		T.YawDeg = Transform.Rotator().Yaw;
		T.LocalVelocityMps = VehicleState.VehicleLocalVelocity * 0.01f;
		const FVector WorldVelocity = VehicleState.VehicleWorldVelocity * 0.01f;
		if (bHavePrevVelocity)
		{
			const FVector Accel = Transform.InverseTransformVectorNoScale((WorldVelocity - PrevWorldVelocity) / DeltaTime);
			FilteredAccel += (Accel - FilteredAccel) * FMath::Min(1.f, DeltaTime / 0.1f);
		}
		PrevWorldVelocity = WorldVelocity;
		bHavePrevVelocity = true;
		T.LocalAccelMps2 = FilteredAccel;
		T.YawRateDegPerSec = FMath::RadiansToDegrees(VehicleState.VehicleWorldAngularVelocity.Z);
		T.SpeedKmh = VehicleState.ForwardSpeed * 0.036f;
		T.RoadWheelAngleDeg = MeanSteerDeg;
		T.SteeringRackTorqueNm = RackTorque;
		T.AppliedForceN = Transform.InverseTransformVectorNoScale(AppliedForce) * 0.01f;

		FScopeLock Lock(&Shared->Lock);
		Shared->Telemetry = T;
	}

	FCarSimParams Params;
	TSharedRef<FCarSharedState, ESPMode::ThreadSafe> Shared;
	FCarDrivetrain Drivetrain;
	FCarDriverInput Input;
	FCarWheelSurface Surface[CarNumWheels];
	int32 SeenResetCounter = 0;
	double SimTime = 0.0;
	float MeanSteerDeg = 0.f;
	float RackTorque = 0.f;
	float FrontSideForceFiltered = 0.f;
	FVector AppliedForce = FVector::ZeroVector; // tyre + drag forces of the last step, kg cm/s^2 (diagnostics)
	float WheelbaseM = 0.f;
	float TrackM = 0.f;
	float VisualAngle[CarNumWheels] = {};
	FVector PrevWorldVelocity = FVector::ZeroVector;
	FVector FilteredAccel = FVector::ZeroVector;
	bool bHavePrevVelocity = false;
};
}

UCarMovementComponent::UCarMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, Shared(MakeShared<FCarSharedState, ESPMode::ThreadSafe>())
{
	const UCarSettings* Settings = GetDefault<UCarSettings>();

	WheelSetups.SetNum(CarNumWheels);
	for (int32 i = 0; i < CarNumWheels; ++i)
	{
		WheelSetups[i].WheelClass = i < 2 ? UCarWheelFront::StaticClass() : UCarWheelRear::StaticClass();
		WheelSetups[i].BoneName = Settings->WheelBones.IsValidIndex(i) ? Settings->WheelBones[i] : NAME_None;
		WheelSetups[i].AdditionalOffset = FVector::ZeroVector;
	}

	Mass = Settings->MassKg;
	bMechanicalSimEnabled = false;   // engine/gearbox are ours
	bSuspensionEnabled = true;
	bWheelFrictionEnabled = true;    // our tyre model runs in ApplyWheelFrictionForces
	bLegacyWheelFrictionPosition = false; // tyre forces at the contact patch
	DragCoefficient = 0.f;           // drag is ours
	DownforceCoefficient = 0.f;
	SleepThreshold = 0.f;            // never put the car to sleep (engine keeps running)
	bReverseAsBrake = false;
	bThrottleAsBrake = false;
}

void UCarMovementComponent::OnCreatePhysicsState()
{
	// Chaos' constraint-based suspension spring uses non-physical units; the force-based one is a plain spring/damper
	// (N per cm) that we can tune with real-world values.
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Vehicle.DisableConstraintSuspension")))
	{
		CVar->Set(1, ECVF_SetByCode);
	}

	// Centre of mass from weight distribution and height, relative to the wheel bones.
	const UCarSettings* Settings = GetDefault<UCarSettings>();
	if (const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(GetOwner() ? GetOwner()->GetRootComponent() : nullptr);
		Mesh && Settings->WheelBones.Num() >= CarNumWheels)
	{
		auto BoneLocation = [Mesh](FName Bone) { return Mesh->GetSocketTransform(Bone, RTS_Component).GetLocation(); };
		const float FrontX = 0.5f * (BoneLocation(Settings->WheelBones[0]).X + BoneLocation(Settings->WheelBones[1]).X);
		const float RearX = 0.5f * (BoneLocation(Settings->WheelBones[2]).X + BoneLocation(Settings->WheelBones[3]).X);
		const float Wheelbase = FrontX - RearX;
		if (Wheelbase > 10.f)
		{
			bEnableCenterOfMassOverride = true;
			CenterOfMassOverride = FVector(FrontX - Wheelbase * (1.f - Settings->FrontWeightFraction), 0.f,
			                               Settings->CenterOfMassHeightCm - Settings->OriginHeightAboveGroundCm);
		}
	}

	Super::OnCreatePhysicsState();

	// Moments of inertia: set the real values directly (the physics asset's shapes say little about a car's mass
	// distribution). The mass-space frame is rotated to the body axes first, so X/Y/Z really are roll/pitch/yaw.
	FBodyInstance* Body = UpdatedPrimitive ? UpdatedPrimitive->GetBodyInstance() : nullptr;
	if (Body && FPhysicsInterface::IsValid(Body->GetPhysicsActor()))
	{
		const FVector Current = Body->GetBodyInertiaTensor();
		const FVector Desired = Settings->InertiaKgM2 * 10000.f; // kg m^2 -> kg cm^2
		FPhysicsCommand::ExecuteWrite(Body->GetPhysicsActor(), [&Desired](const FPhysicsActorHandle& Actor)
		{
			FTransform Com = FPhysicsInterface::GetComTransformLocal_AssumesLocked(Actor);
			Com.SetRotation(FQuat::Identity);
			FPhysicsInterface::SetComLocalPose_AssumesLocked(Actor, Com);
			FPhysicsInterface::SetMassSpaceInertiaTensor_AssumesLocked(Actor, Desired);
		});
		InertiaTensorScale = FVector::OneVector; // if Chaos re-applies its mass properties, keep ours

		// No artificial velocity damping: air drag and rolling resistance are modelled explicitly.
		Body->LinearDamping = 0.f;
		Body->AngularDamping = 0.f;
		Body->UpdateDampingProperties();

		// A sleeping body ignores the tyre forces (e.g. creeping away with the clutch) and stops the engine simulation.
		Body->GetPhysicsActor()->GetGameThreadAPI().SetSleepType(Chaos::ESleepType::NeverSleep);
		const FBox Bounds = Body->GetBodyBounds().TransformBy(UpdatedPrimitive->GetComponentTransform().Inverse());
		UE_LOG(LogCar, Log, TEXT("Car physics: mass %.0f kg, COM %s cm, inertia %s kg m^2 (was %s), collision bounds %s, damping lin %.3f ang %.3f"), Body->GetBodyMass(),
		       *CenterOfMassOverride.ToString(), *(Desired / 10000.f).ToString(), *(Current / 10000.f).ToString(), *Bounds.ToString(),
		       Body->LinearDamping, Body->AngularDamping);
	}
}

TUniquePtr<Chaos::FSimpleWheeledVehicle> UCarMovementComponent::CreatePhysicsVehicle()
{
	VehicleSimulationPT = MakeUnique<FCarVehicleSimulation>(GetDefault<UCarSettings>()->MakeSimParams(), Shared);
	return UChaosVehicleMovementComponent::CreatePhysicsVehicle();
}

void UCarMovementComponent::SetDriverInput(const FCarDriverInput& Input)
{
	FScopeLock Lock(&Shared->Lock);
	Shared->Input = Input;
}

FCarTelemetry UCarMovementComponent::GetTelemetry() const
{
	FScopeLock Lock(&Shared->Lock);
	return Shared->Telemetry;
}

void UCarMovementComponent::ResetDrivetrain(bool bEngineRunning)
{
	FScopeLock Lock(&Shared->Lock);
	Shared->bResetEngineRunning = bEngineRunning;
	++Shared->ResetCounter;
}

void UCarMovementComponent::SetSurfaceConditionsOverride(const FCarSurfaceConditions& Conditions)
{
	bSurfaceOverride = true;
	SurfaceOverride = Conditions;
	SurfaceUpdateCountdown = 0.f;
}

void UCarMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateWheelSurfaces(DeltaTime);
	StampSnowTracks();
}

void UCarMovementComponent::StampSnowTracks()
{
	USnowTrackSubsystem* SnowTracks = GetWorld() ? GetWorld()->GetSubsystem<USnowTrackSubsystem>() : nullptr;
	const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(UpdatedComponent);
	const UCarSettings* Settings = GetDefault<UCarSettings>();
	if (!SnowTracks || !Mesh || Settings->WheelBones.Num() < CarNumWheels)
	{
		return;
	}
	constexpr float TyreWidthCm = 20.f;
	const FCarTelemetry Telemetry = GetTelemetry();
	for (int32 Wheel = 0; Wheel < CarNumWheels; ++Wheel)
	{
		if (Telemetry.bContact[Wheel])
		{
			SnowTracks->StampWheel(reinterpret_cast<const uint8*>(this) + Wheel, Mesh->GetSocketLocation(Settings->WheelBones[Wheel]), TyreWidthCm);
		}
	}
}

FCarSurfaceConditions UCarMovementComponent::CurrentSurfaceConditions() const
{
	if (bSurfaceOverride)
	{
		return SurfaceOverride;
	}
	FCarSurfaceConditions Conditions;
	const UWeatherVisualsSubsystem* Visuals = GetWorld() ? GetWorld()->GetSubsystem<UWeatherVisualsSubsystem>() : nullptr;
	if (Visuals)
	{
		Conditions.Wetness = Visuals->GetWetness();
		Conditions.SnowCover = Visuals->GetSnowCover();
		Conditions.TemperatureCelsius = Visuals->GetState().TemperatureCelsius;
	}
	return Conditions;
}

void UCarMovementComponent::UpdateWheelSurfaces(float DeltaTime)
{
	// Grip changes slowly (weather, material borders), so ten updates a second are plenty.
	constexpr float UpdateIntervalSeconds = 0.1f;
	SurfaceUpdateCountdown -= DeltaTime;
	if (SurfaceUpdateCountdown > 0.f)
	{
		return;
	}
	SurfaceUpdateCountdown = UpdateIntervalSeconds;

	const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(UpdatedComponent);
	const UCarSettings* Settings = GetDefault<UCarSettings>();
	if (!Mesh || Settings->WheelBones.Num() < CarNumWheels)
	{
		return;
	}
	// A drive test without an explicit surface runs on dry asphalt whatever the map's weather is.
	if (!bSurfaceOverride && FParse::Param(FCommandLine::Get(), TEXT("DriveTest")))
	{
		SetSurfaceConditionsOverride(FCarSurfaceConditions());
	}

	const FCarSurfaceConditions Conditions = CurrentSurfaceConditions();
	FCarWheelSurface Surfaces[CarNumWheels];
	FCollisionQueryParams Params(SCENE_QUERY_STAT(CarWheelSurface), /*bTraceComplex=*/true, GetOwner());
	Params.bReturnFaceIndex = true;
	for (int32 Wheel = 0; Wheel < CarNumWheels; ++Wheel)
	{
		const FVector Start = Mesh->GetSocketLocation(Settings->WheelBones[Wheel]);
		FHitResult Hit;
		const bool bHitGround = GetWorld()->LineTraceSingleByChannel(Hit, Start, Start - FVector(0.f, 0.f, 150.f), ECC_Visibility, Params);
		const FName GroundName = bHitGround ? FindWorldSurfaceName(Hit) : NAME_None;
		// Position-based variation: snow depth and grip differ from wheel to wheel and from patch to patch.
		const float Variation = FMath::Sin(Start.X * 0.013f + Start.Y * 0.021f) * FMath::Sin(Start.Y * 0.017f - Start.X * 0.007f);
		Surfaces[Wheel] = CarSurfaceGrip::Evaluate(CarSurfaceGrip::Classify(GroundName), Conditions, Variation);
	}

	FScopeLock Lock(&Shared->Lock);
	for (int32 Wheel = 0; Wheel < CarNumWheels; ++Wheel)
	{
		Shared->Surface[Wheel] = Surfaces[Wheel];
	}
}
