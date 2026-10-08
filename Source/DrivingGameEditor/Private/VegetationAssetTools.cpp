#include "VegetationAssetTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/NaniteAssemblyData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "PhysicsEngine/BodySetup.h"
#include "Misc/PackageName.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogVegetationTools, Log, All);

namespace
{
/** Reference-pose transform of every bone in mesh (component) space. */
TArray<FTransform> ComponentSpaceRefPose(const FReferenceSkeleton& RefSkeleton)
{
	const TArray<FTransform>& Local = RefSkeleton.GetRefBonePose();
	TArray<FTransform> Result;
	Result.SetNum(Local.Num());
	for (int32 i = 0; i < Local.Num(); ++i)
	{
		const int32 Parent = RefSkeleton.GetParentIndex(i);
		Result[i] = Parent == INDEX_NONE ? Local[i] : Local[i] * Result[Parent];
	}
	return Result;
}

/** SKM_Leaf_Twig_01 -> Leaf_Twig_01 (static mesh in the same folder); static parts stay as they are. */
FSoftObjectPath StaticPartFor(const FSoftObjectPath& Part)
{
	UObject* Asset = Part.TryLoad();
	if (!Asset || Asset->IsA<UStaticMesh>())
	{
		return Part;
	}
	FString Name = Asset->GetName();
	Name.RemoveFromStart(TEXT("SKM_"));
	const FString Folder = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName());
	const FString Candidate = FString::Printf(TEXT("%s/%s.%s"), *Folder, *Name, *Name);
	if (LoadObject<UStaticMesh>(nullptr, *Candidate, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return FSoftObjectPath(Candidate);
	}
	UE_LOG(LogVegetationTools, Warning, TEXT("No static counterpart for assembly part %s"), *Part.ToString());
	return FSoftObjectPath();
}
}

UStaticMesh* UVegetationAssetTools::BakeSkinnedAssemblyToStatic(USkeletalMesh* Source, const FString& AssetPath, bool bSave)
{
	if (!Source)
	{
		return nullptr;
	}
	FMeshDescription MeshDescription;
	if (!Source->CloneMeshDescription(0, MeshDescription))
	{
		UE_LOG(LogVegetationTools, Error, TEXT("%s has no LOD0 mesh description"), *Source->GetPathName());
		return nullptr;
	}

	// --- assembly: bone-relative twig nodes -> mesh-space nodes in the reference pose ---
	const FMeshNaniteSettings& SourceNanite = Source->GetNaniteSettings();
	FNaniteAssemblyData Assembly = SourceNanite.NaniteAssemblyData;
	TArray<bool> PartValid;
	for (FNaniteAssemblyPart& Part : Assembly.Parts)
	{
		Part.MeshObjectPath = StaticPartFor(Part.MeshObjectPath);
		PartValid.Add(Part.MeshObjectPath.IsValid());
	}
	const TArray<FTransform> BonePose = ComponentSpaceRefPose(Source->GetRefSkeleton());
	TArray<FNaniteAssemblyNode> Nodes;
	Nodes.Reserve(Assembly.Nodes.Num());
	for (const FNaniteAssemblyNode& Node : Assembly.Nodes)
	{
		if (!PartValid.IsValidIndex(Node.PartIndex) || !PartValid[Node.PartIndex])
		{
			continue;
		}
		FNaniteAssemblyNode Baked = Node;
		if (Node.TransformSpace == ENaniteAssemblyNodeTransformSpace::BoneRelative)
		{
			// Twigs are rigidly attached; use the dominant bone.
			const FNaniteAssemblyBoneInfluence* Best = nullptr;
			for (const FNaniteAssemblyBoneInfluence& Influence : Node.BoneInfluences)
			{
				if (!Best || Influence.BoneWeight > Best->BoneWeight)
				{
					Best = &Influence;
				}
			}
			if (Best && BonePose.IsValidIndex(Best->BoneIndex))
			{
				const FTransform Mesh = FTransform(Node.Transform) * BonePose[Best->BoneIndex];
				Baked.Transform = FTransform3f(Mesh);
			}
		}
		Baked.TransformSpace = ENaniteAssemblyNodeTransformSpace::Local;
		Baked.BoneInfluences.Reset();
		Nodes.Add(Baked);
	}
	Assembly.Nodes = MoveTemp(Nodes);

	// --- asset ---
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	Package->FullyLoad();
	UStaticMesh* Mesh = FindObject<UStaticMesh>(Package, *AssetName);
	if (!Mesh)
	{
		Mesh = NewObject<UStaticMesh>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Mesh);
	}
	Mesh->PreEditChange(nullptr);

	// Same material list and order as the source, so the parts' material remaps stay valid.
	Mesh->GetStaticMaterials().Reset();
	for (const FSkeletalMaterial& Material : Source->GetMaterials())
	{
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material.MaterialInterface, Material.MaterialSlotName,
			Material.ImportedMaterialSlotName));
	}

	Mesh->SetNumSourceModels(1);
	FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(0);
	SourceModel.BuildSettings.bRecomputeNormals = false;
	SourceModel.BuildSettings.bRecomputeTangents = false;
	SourceModel.BuildSettings.bGenerateLightmapUVs = false;
	SourceModel.BuildSettings.bRemoveDegenerates = false;
	Mesh->CreateMeshDescription(0, MoveTemp(MeshDescription));
	Mesh->CommitMeshDescription(0);

	FMeshNaniteSettings Nanite = SourceNanite;
	Nanite.bEnabled = true;
	Nanite.NaniteAssemblyData = MoveTemp(Assembly);
	Mesh->SetNaniteSettings(Nanite);
	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseSimpleAsComplex; // no collision shapes: trunks get colliders

	Mesh->Build(/*bInSilent=*/true);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();

	if (bSave)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Mesh, *Filename, SaveArgs))
		{
			UE_LOG(LogVegetationTools, Error, TEXT("Failed to save %s"), *Filename);
		}
	}
	UE_LOG(LogVegetationTools, Log, TEXT("Baked %s -> %s: %d assembly parts, %d nodes, bounds %s"), *Source->GetName(),
		*AssetPath, Mesh->GetNaniteSettings().NaniteAssemblyData.Parts.Num(),
		Mesh->GetNaniteSettings().NaniteAssemblyData.Nodes.Num(), *Mesh->GetBounds().GetBox().ToString());
	return Mesh;
}
