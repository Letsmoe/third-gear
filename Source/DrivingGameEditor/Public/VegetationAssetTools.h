#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VegetationAssetTools.generated.h"

class USkeletalMesh;
class UStaticMesh;

/**
 * Editor helpers for vegetation assets. Callable from Python: unreal.VegetationAssetTools.<snake_case_name>(...)
 */
UCLASS()
class DRIVINGGAMEEDITOR_API UVegetationAssetTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Bakes a skinned Nanite-assembly tree (e.g. from the Procedural Vegetation Editor) into a static Nanite-assembly
	 * mesh in its reference pose: the trunk geometry is copied, every bone-attached twig node becomes a mesh-space node
	 * and skeletal twig parts are swapped for their static counterparts (SKM_X -> X in the same folder).
	 * Skinned foliage + virtual shadow maps crashes the GPU on Linux/Vulkan (UE 5.8), static assemblies are also cheaper.
	 */
	UFUNCTION(BlueprintCallable, Category = "Vegetation")
	static UStaticMesh* BakeSkinnedAssemblyToStatic(USkeletalMesh* Source, const FString& AssetPath, bool bSave = true);
};
