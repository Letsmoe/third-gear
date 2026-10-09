#pragma once

#include "CoreMinimal.h"

struct FHitResult;

/**
 * Name of the surface material under a trace hit on generated ground, without the "M_" prefix ("Road_Asphalt",
 * "Road_Cobble", "Road_Pavers", "Pavement", "Terrain" ...). The trace has to be complex and ask for the face index.
 * NAME_None when the hit is not on a generated ground mesh.
 */
MAPRUNTIME_API FName FindWorldSurfaceName(const FHitResult& Hit);
