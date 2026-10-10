#include "Map3DMeshBuilder.h"

namespace
{
constexpr float CentimetresPerMetre = 100.f;

/** Direction towards the light in the map's frame (x east, y south, z up). */
const FVector3f LightDirection = FVector3f(-0.45f, -0.55f, 0.7f).GetSafeNormal();

/** The 12 vertices and 20 faces of an icosahedron. */
FVector3f IcosahedronVertex(int32 Index)
{
	static const float Golden = (1.f + FMath::Sqrt(5.f)) * 0.5f;
	static const FVector3f Vertices[12] = {
		FVector3f(-1.f, Golden, 0.f), FVector3f(1.f, Golden, 0.f), FVector3f(-1.f, -Golden, 0.f), FVector3f(1.f, -Golden, 0.f),
		FVector3f(0.f, -1.f, Golden), FVector3f(0.f, 1.f, Golden), FVector3f(0.f, -1.f, -Golden), FVector3f(0.f, 1.f, -Golden),
		FVector3f(Golden, 0.f, -1.f), FVector3f(Golden, 0.f, 1.f), FVector3f(-Golden, 0.f, -1.f), FVector3f(-Golden, 0.f, 1.f)};
	return Vertices[Index].GetSafeNormal();
}

const int32 IcosahedronFaces[20][3] = {
	{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
	{3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
}

float Map3D::ShadeForNormal(const FVector3f& Normal)
{
	const float Lit = FVector3f::DotProduct(Normal.GetSafeNormal(), LightDirection);
	return FMath::Clamp(0.86f + 0.12f * Lit, 0.7f, 1.f);
}

int32 FMap3DMeshBuilder::AddVertex(const FVector3f& Position, EMap3DColor Color, float Shade)
{
	Positions.Add(Position);
	PaletteAndShade.Add(FVector2f((static_cast<float>(Color) + 0.5f) / Map3DPaletteSize, Shade));
	return Positions.Num() - 1;
}

void FMap3DMeshBuilder::AddTriangle(int32 A, int32 B, int32 C, const FVector3f& Facing)
{
	// The engine's convention: a triangle (A, B, C) faces along Cross(C - A, B - A).
	const FVector3f Face = FVector3f::CrossProduct(Positions[C] - Positions[A], Positions[B] - Positions[A]);
	if (Face.SizeSquared() < 1e-10f)
	{
		return;
	}
	const bool bFlip = FVector3f::DotProduct(Face, Facing) < 0.f;
	Indices.Add(A);
	Indices.Add(bFlip ? C : B);
	Indices.Add(bFlip ? B : C);
}

void FMap3DMeshBuilder::AddTriangle(const FVector3f& A, const FVector3f& B, const FVector3f& C, EMap3DColor Color, float Shade, const FVector3f& Facing)
{
	AddTriangle(AddVertex(A, Color, Shade), AddVertex(B, Color, Shade), AddVertex(C, Color, Shade), Facing);
}

void FMap3DMeshBuilder::AddQuad(const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D, EMap3DColor Color, float Shade, const FVector3f& Facing)
{
	const int32 VertexA = AddVertex(A, Color, Shade);
	const int32 VertexB = AddVertex(B, Color, Shade);
	const int32 VertexC = AddVertex(C, Color, Shade);
	const int32 VertexD = AddVertex(D, Color, Shade);
	AddTriangle(VertexA, VertexB, VertexC, Facing);
	AddTriangle(VertexA, VertexC, VertexD, Facing);
}

void FMap3DMeshBuilder::AddFlatTriangles(const TArray<FVector2f>& Vertices, const TArray<FIntVector3>& Triangles, float Height, EMap3DColor Color)
{
	const int32 First = Positions.Num();
	for (const FVector2f& Vertex : Vertices)
	{
		AddVertex(FVector3f(Vertex, Height), Color, 1.f);
	}
	for (const FIntVector3& Triangle : Triangles)
	{
		AddTriangle(First + Triangle.X, First + Triangle.Y, First + Triangle.Z, FVector3f::UpVector);
	}
}

void FMap3DMeshBuilder::AddFlatQuad(const FVector2f& A, const FVector2f& B, const FVector2f& C, const FVector2f& D, float Height, EMap3DColor Color)
{
	AddQuad(FVector3f(A, Height), FVector3f(B, Height), FVector3f(C, Height), FVector3f(D, Height), Color, 1.f, FVector3f::UpVector);
}

void FMap3DMeshBuilder::AddBox(const FVector3f& BaseCentre, const FVector3f& HalfSize, float YawRadians, EMap3DColor Color, float Shade)
{
	const float Sine = FMath::Sin(YawRadians);
	const float Cosine = FMath::Cos(YawRadians);
	const auto Corner = [&](float SignX, float SignY, float Height)
	{
		const FVector2f Local(SignX * HalfSize.X, SignY * HalfSize.Y);
		return FVector3f(BaseCentre.X + Local.X * Cosine - Local.Y * Sine, BaseCentre.Y + Local.X * Sine + Local.Y * Cosine, BaseCentre.Z + Height);
	};
	const float Top = 2.f * HalfSize.Z;
	const FVector3f Axis = FVector3f(Cosine, Sine, 0.f);
	const FVector3f Side = FVector3f(-Sine, Cosine, 0.f);
	AddQuad(Corner(-1, -1, Top), Corner(1, -1, Top), Corner(1, 1, Top), Corner(-1, 1, Top), Color, Shade, FVector3f::UpVector);
	AddQuad(Corner(1, -1, 0), Corner(1, 1, 0), Corner(1, 1, Top), Corner(1, -1, Top), Color, Shade * Map3D::ShadeForNormal(Axis), Axis);
	AddQuad(Corner(-1, 1, 0), Corner(-1, -1, 0), Corner(-1, -1, Top), Corner(-1, 1, Top), Color, Shade * Map3D::ShadeForNormal(-Axis), -Axis);
	AddQuad(Corner(1, 1, 0), Corner(-1, 1, 0), Corner(-1, 1, Top), Corner(1, 1, Top), Color, Shade * Map3D::ShadeForNormal(Side), Side);
	AddQuad(Corner(-1, -1, 0), Corner(1, -1, 0), Corner(1, -1, Top), Corner(-1, -1, Top), Color, Shade * Map3D::ShadeForNormal(-Side), -Side);
}

void FMap3DMeshBuilder::AddBlob(const FVector3f& Centre, const FVector3f& Radii, EMap3DColor Color, float Shade)
{
	for (const int32* Face : IcosahedronFaces)
	{
		const FVector3f A = IcosahedronVertex(Face[0]);
		const FVector3f B = IcosahedronVertex(Face[1]);
		const FVector3f C = IcosahedronVertex(Face[2]);
		const FVector3f Normal = (A + B + C).GetSafeNormal();
		const auto Place = [&](const FVector3f& Unit) { return Centre + Unit * Radii; };
		AddTriangle(Place(A), Place(B), Place(C), Color, Shade * Map3D::ShadeForNormal(Normal), Normal);
	}
}

void FMap3DMeshBuilder::AddRibbon(const TArray<FVector2f>& Points, float Width, float Height, EMap3DColor Color)
{
	if (Points.Num() < 2)
	{
		return;
	}
	const float HalfWidth = 0.5f * Width;
	// Left and right edge points at every vertex, along the mitred direction of the two segments meeting there.
	TArray<int32> Left;
	TArray<int32> Right;
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		const FVector2f Before = Points[Index] - Points[FMath::Max(Index - 1, 0)];
		const FVector2f After = Points[FMath::Min(Index + 1, Points.Num() - 1)] - Points[Index];
		FVector2f Direction = Before.GetSafeNormal() + After.GetSafeNormal();
		Direction = Direction.SizeSquared() > UE_SMALL_NUMBER ? Direction.GetSafeNormal() : After.GetSafeNormal();
		const FVector2f Normal(-Direction.Y, Direction.X);
		const FVector2f SegmentNormal(-After.GetSafeNormal().Y, After.GetSafeNormal().X);
		const FVector2f Mitre = Normal * HalfWidth / FMath::Max(FVector2f::DotProduct(Normal, SegmentNormal), 0.5f);
		Left.Add(AddVertex(FVector3f(Points[Index] + Mitre, Height), Color, 1.f));
		Right.Add(AddVertex(FVector3f(Points[Index] - Mitre, Height), Color, 1.f));
	}
	for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
	{
		AddTriangle(Left[Index], Left[Index + 1], Right[Index + 1], FVector3f::UpVector);
		AddTriangle(Left[Index], Right[Index + 1], Right[Index], FVector3f::UpVector);
	}
}

TSharedPtr<FMap3DMeshData> FMap3DMeshBuilder::Finish() const
{
	TSharedPtr<FMap3DMeshData> Data = MakeShared<FMap3DMeshData>();
	Data->Vertices.Reserve(Positions.Num());
	for (int32 Index = 0; Index < Positions.Num(); ++Index)
	{
		FMap3DVertex Vertex;
		Vertex.Position = Positions[Index] * CentimetresPerMetre;
		Vertex.PaletteU = PaletteAndShade[Index].X;
		Vertex.Shade = PaletteAndShade[Index].Y;
		Data->Vertices.Add(Vertex);
		Data->Bounds += FVector(Vertex.Position);
	}
	Data->Indices = Indices;
	return Data;
}
