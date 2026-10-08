#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DgMeshImporter.generated.h"

class UStaticMesh;

/**
 * Builds static mesh assets from .dgmesh tiles written by Tools/osmimport (format documented in osmimport/mesh.py).
 * Callable from editor Python: unreal.DgMeshImporter.import_dg_mesh(...)
 */
UCLASS()
class DRIVINGGAMEEDITOR_API UDgMeshImporter : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Creates (or replaces) a static mesh asset at AssetPath (e.g. /Game/World/Bergedorf/SM_tile_0_0) from a .dgmesh file.
	 * Each section becomes a material slot named after the section; its material is loaded from
	 * MaterialFolder/M_<Section> if it exists, otherwise the engine default material is used.
	 * Collision uses the render triangles (complex as simple) so the car drives on exactly what is drawn.
	 */
	UFUNCTION(BlueprintCallable, Category = "OSM Import")
	static UStaticMesh* ImportDgMesh(const FString& FilePath, const FString& AssetPath, const FString& MaterialFolder,
		bool bEnableNanite, bool bSave);
};
