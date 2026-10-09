#pragma once

#include "CoreMinimal.h"

struct FTrafficVehicleModel;
struct FWorldFurnitureInstances;
struct FWorldPoi;

/**
 * Cars parked along the streets: placed by the world compiler (POIS kind ParkedCar), drawn as instanced City Sample
 * meshes of the AI traffic models, blocked by an invisible box per car.
 */
namespace ParkedCars
{
/** The models parked cars come in, in the order of Tools/osmimport/osmimport/parking.py MODELS. Shared and not yet loaded. */
const TArray<TSharedPtr<FTrafficVehicleModel>>& GetModels();

/** Turns a ParkedCar record into a placement (pose and a paint picked from the traffic palette by position). */
void AddParkedCar(const FWorldPoi& Poi, const FVector2D& TileOriginM, FWorldFurnitureInstances& Out);

/** Transform of a mesh modelled in car space (the body, glass and wheels all are) for a car with the given pose. */
FTransform MeshTransform(const FTrafficVehicleModel& Model, const FTransform& Pose);

/** Transform of a unit cube (100 cm) that covers the car's body, for the collision box. */
FTransform ColliderTransform(const FTrafficVehicleModel& Model, const FTransform& Pose);

/** Whether parked cars are switched off (-NoParkedCars or tg.ParkedCars 0). */
bool IsDisabled();
}
