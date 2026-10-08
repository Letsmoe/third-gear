#include "DgMeshImporter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogDgMesh, Log, All);

namespace
{
struct FDgSection
{
	FString Name;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UVs;
	TArray<FColor> Colors;
	TArray<uint32> Indices;
};

class FReader
{
public:
	explicit FReader(const TArray<uint8>& InData) : Data(InData) {}

	bool Read(void* Out, int64 Size)
	{
		if (Offset + Size > Data.Num())
		{
			return false;
		}
		FMemory::Memcpy(Out, Data.GetData() + Offset, Size);
		Offset += Size;
		return true;
	}

	template <typename T>
	bool ReadArray(TArray<T>& Out, int64 Count)
	{
		Out.SetNumUninitialized(Count);
		return Read(Out.GetData(), Count * sizeof(T));
	}

private:
	const TArray<uint8>& Data;
	int64 Offset = 0;
};

bool ParseDgMesh(const FString& FilePath, TArray<FDgSection>& OutSections)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
	{
		UE_LOG(LogDgMesh, Error, TEXT("Cannot read %s"), *FilePath);
		return false;
	}
	FReader Reader(Bytes);
	char Magic[4];
	uint32 SectionCount = 0;
	if (!Reader.Read(Magic, 4) || FMemory::Memcmp(Magic, "DGM1", 4) != 0 || !Reader.Read(&SectionCount, 4))
	{
		UE_LOG(LogDgMesh, Error, TEXT("%s is not a DGM1 file"), *FilePath);
		return false;
	}
	for (uint32 s = 0; s < SectionCount; ++s)
	{
		FDgSection& Section = OutSections.AddDefaulted_GetRef();
		uint32 NameLen = 0, VertexCount = 0, IndexCount = 0;
		TArray<uint8> NameBytes;
		TArray<uint8> ColorBytes;
		if (!Reader.Read(&NameLen, 4) || !Reader.ReadArray(NameBytes, NameLen) || !Reader.Read(&VertexCount, 4) ||
			!Reader.Read(&IndexCount, 4) || !Reader.ReadArray(Section.Positions, VertexCount) ||
			!Reader.ReadArray(Section.Normals, VertexCount) || !Reader.ReadArray(Section.UVs, VertexCount) ||
			!Reader.ReadArray(ColorBytes, VertexCount * 4) || !Reader.ReadArray(Section.Indices, IndexCount))
		{
			UE_LOG(LogDgMesh, Error, TEXT("%s: truncated section %u"), *FilePath, s);
			return false;
		}
		Section.Name = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(NameBytes.GetData())), NameLen);
		Section.Name = Section.Name.Left(NameLen);
		Section.Colors.SetNumUninitialized(VertexCount);
		for (uint32 v = 0; v < VertexCount; ++v)
		{
			Section.Colors[v] = FColor(ColorBytes[v * 4], ColorBytes[v * 4 + 1], ColorBytes[v * 4 + 2], ColorBytes[v * 4 + 3]);
		}
	}
	return true;
}
}

UStaticMesh* UDgMeshImporter::ImportDgMesh(const FString& FilePath, const FString& AssetPath, const FString& MaterialFolder,
	bool bEnableNanite, bool bSave)
{
	TArray<FDgSection> Sections;
	if (!ParseDgMesh(FilePath, Sections) || Sections.IsEmpty())
	{
		return nullptr;
	}

	// --- mesh description ---
	FMeshDescription MeshDescription;
	FStaticMeshAttributes Attributes(MeshDescription);
	Attributes.Register();
	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> InstanceUVs = Attributes.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
	TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
	InstanceUVs.SetNumChannels(1);

	int32 TotalVertices = 0, TotalTriangles = 0;
	for (const FDgSection& Section : Sections)
	{
		TotalVertices += Section.Positions.Num();
		TotalTriangles += Section.Indices.Num() / 3;
	}
	MeshDescription.ReserveNewVertices(TotalVertices);
	MeshDescription.ReserveNewVertexInstances(TotalVertices);
	MeshDescription.ReserveNewTriangles(TotalTriangles);

	for (const FDgSection& Section : Sections)
	{
		const FPolygonGroupID Group = MeshDescription.CreatePolygonGroup();
		SlotNames[Group] = FName(*Section.Name);

		TArray<FVertexInstanceID> Instances;
		Instances.SetNumUninitialized(Section.Positions.Num());
		for (int32 v = 0; v < Section.Positions.Num(); ++v)
		{
			const FVertexID Vertex = MeshDescription.CreateVertex();
			VertexPositions[Vertex] = Section.Positions[v];
			const FVertexInstanceID Instance = MeshDescription.CreateVertexInstance(Vertex);
			InstanceNormals[Instance] = Section.Normals[v];
			InstanceUVs.Set(Instance, 0, Section.UVs[v]);
			InstanceColors[Instance] = FVector4f(FLinearColor(Section.Colors[v]));
			Instances[v] = Instance;
		}
		for (int32 i = 0; i + 2 < Section.Indices.Num(); i += 3)
		{
			const FVertexInstanceID Tri[3] = {Instances[Section.Indices[i]], Instances[Section.Indices[i + 1]],
				Instances[Section.Indices[i + 2]]};
			MeshDescription.CreateTriangle(Group, Tri);
		}
	}

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
	Mesh->GetStaticMaterials().Reset();
	for (const FDgSection& Section : Sections)
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr,
			*FString::Printf(TEXT("%s/M_%s.M_%s"), *MaterialFolder, *Section.Name, *Section.Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Material)
		{
			Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		Mesh->GetStaticMaterials().Add(FStaticMaterial(Material, FName(*Section.Name), FName(*Section.Name)));
	}

	Mesh->SetNumSourceModels(1);
	FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(0);
	SourceModel.BuildSettings.bRecomputeNormals = false;
	SourceModel.BuildSettings.bRecomputeTangents = true;
	SourceModel.BuildSettings.bUseMikkTSpace = true;
	SourceModel.BuildSettings.bGenerateLightmapUVs = false;
	SourceModel.BuildSettings.bRemoveDegenerates = true;
	SourceModel.BuildSettings.bUseFullPrecisionUVs = true; // world-space UVs reach hundreds of metres
	Mesh->CreateMeshDescription(0, MoveTemp(MeshDescription));
	Mesh->CommitMeshDescription(0);

	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = bEnableNanite;
	Nanite.FallbackRelativeError = 0.f; // keep the full mesh for collision and non-Nanite paths
	Mesh->SetNaniteSettings(Nanite);

	Mesh->CreateBodySetup();
	Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
	Mesh->bAllowCPUAccess = false;

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
			UE_LOG(LogDgMesh, Error, TEXT("Failed to save %s"), *Filename);
		}
	}
	UE_LOG(LogDgMesh, Log, TEXT("Imported %s: %d sections, %d vertices, %d triangles"), *AssetPath, Sections.Num(),
		TotalVertices, TotalTriangles);
	return Mesh;
}
