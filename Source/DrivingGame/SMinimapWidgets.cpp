#include "SMinimapWidgets.h"

#include "Engine/TextureRenderTarget2D.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Map3DSubsystem.h"
#include "MapTileCache.h"
#include "MinimapIndex.h"
#include "MinimapSubsystem.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"

namespace
{
constexpr float MinimapShadowMarginPx = 14.f;
constexpr float MinimapCornerRadiusPx = 24.f;

/** Road widths in metres by road class, and the least width they get on screen in pixels. */
constexpr float RoadWidthMeters[] = {2.f, 3.f, 4.5f, 6.5f, 9.f};
constexpr float RoadMinimumPixels[] = {1.2f, 2.f, 2.8f, 3.6f, 4.8f};

/** Pixels per metre from which one-way arrows and footpath lines are drawn. */
constexpr float ArrowMinimumPixelsPerMetre = 1.2f;
constexpr float FootpathMinimumPixelsPerMetre = 1.4f;

/** Width of footpath lines on screen, pixels. */
constexpr float FootpathPixels = 1.1f;

/** Everything one map picture needs: what to draw and how the world maps to pixels. */
struct FMapScene
{
	const FMapPalette* Palette = &FMapPalette::Light();
	const FMinimapFrame* Frame = nullptr;
	const TArray<const FMapTile*>* Tiles = nullptr;
	/** World position (metres) at OriginPx, and the world direction (radians) that points up. */
	FVector2f CentreWorld = FVector2f::ZeroVector;
	float HeadingRadians = 0.f;
	FVector2f OriginPx = FVector2f::ZeroVector;
	float PixelsPerMetre = 1.f;
	/** Multiplier for line widths, markers and text, so the same design fits a larger image. */
	float UiScale = 1.f;
	bool bBuildings = false;
};

FLinearColor Linear(const FColor& Color)
{
	return FMapPalette::Linear(Color);
}

/** Draws map scenes into a widget's geometry. */
class FMapPainter
{
public:
	FMapPainter(FSlateWindowElementList& InElements, const FGeometry& InGeometry, int32 InLayer)
		: Elements(InElements), Geometry(InGeometry), Layer(InLayer)
	{
		PillBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
		PillBrush.TintColor = FLinearColor::White;
		PillBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
		PillBrush.OutlineSettings.Color = FLinearColor::Transparent;
		PillBrush.OutlineSettings.Width = 0.f;
		WhiteHandle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FCoreStyle::Get().GetBrush("GenericWhiteBox"));
	}

	int32 GetLayer() const { return Layer; }

	/** Land, water, footpaths, buildings, roads and the route, in that order. */
	void Scene(const FMapScene& Scene)
	{
		const FVector2f Size = FVector2f(Geometry.GetLocalSize());
		Fill(FVector2f::ZeroVector, Size, Linear(Scene.Palette->Land));
		if (Scene.Tiles)
		{
			Landcover(Scene);
			Water(Scene);
			if (Scene.PixelsPerMetre >= FootpathMinimumPixelsPerMetre)
			{
				Footpaths(Scene);
			}
			if (Scene.bBuildings)
			{
				Buildings(Scene);
			}
		}
		if (Scene.Frame)
		{
			Roads(Scene);
		}
	}

	void Fill(const FVector2f& TopLeft, const FVector2f& Size, const FLinearColor& Color)
	{
		FSlateDrawElement::MakeBox(Elements, ++Layer, Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(TopLeft)),
			FCoreStyle::Get().GetBrush("GenericWhiteBox"), ESlateDrawEffect::None, Color);
	}

	/** A rounded label background centred on a position. */
	void Pill(const FVector2f& Centre, const FVector2f& Size, const FLinearColor& Color)
	{
		FSlateDrawElement::MakeBox(Elements, ++Layer, Geometry.ToPaintGeometry(Size, FSlateLayoutTransform(Centre - 0.5f * Size)),
			&PillBrush, ESlateDrawEffect::None, Color);
	}

	/** Text anchored by its centre. */
	void Label(const FString& Content, const FVector2f& Anchor, float FontSize, const FLinearColor& Color)
	{
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", FMath::RoundToInt(FontSize));
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		const FVector2f TextSize = FVector2f(Measure->Measure(Content, Font));
		FSlateDrawElement::MakeText(Elements, ++Layer, Geometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(Anchor - 0.5f * TextSize)),
			Content, Font, ESlateDrawEffect::None, Color);
	}

	/** Width of a text in pixels. */
	float TextWidth(const FString& Content, float FontSize) const
	{
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", FMath::RoundToInt(FontSize));
		return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Content, Font).X;
	}

	void Line(const FVector2f& From, const FVector2f& To, const FLinearColor& Color, float Thickness)
	{
		TArray<FVector2f> Points = {From, To};
		FSlateDrawElement::MakeLines(Elements, ++Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true, Thickness);
	}

	/** The navigation arrow of the car, pointing along AngleRadians clockwise from up, blue with a white border. */
	void CarMarker(const FMapPalette& Palette, const FVector2f& Position, float AngleRadians, float Size)
	{
		const auto Rotated = [&](const FVector2f& Local)
		{
			const float Sine = FMath::Sin(AngleRadians);
			const float Cosine = FMath::Cos(AngleRadians);
			return Position + Size * FVector2f(Local.X * Cosine - Local.Y * Sine, Local.X * Sine + Local.Y * Cosine);
		};
		const FVector2f Tip = Rotated(FVector2f(0.f, -1.3f));
		const FVector2f Notch = Rotated(FVector2f(0.f, 0.55f));
		const FVector2f RightBack = Rotated(FVector2f(0.85f, 1.f));
		const FVector2f LeftBack = Rotated(FVector2f(-0.85f, 1.f));
		TArray<FVector2f> Border = {Tip, RightBack, Notch, LeftBack, Tip};
		FSlateDrawElement::MakeLines(Elements, ++Layer, Geometry.ToPaintGeometry(), Border, ESlateDrawEffect::None, Linear(Palette.CarArrowBorder), true, 0.5f * Size);
		Triangle(Tip, RightBack, Notch, Linear(Palette.CarArrow));
		Triangle(Tip, Notch, LeftBack, Linear(Palette.CarArrow));
	}

	/** The waypoint: a red pin whose tip is at the position. */
	void WaypointPin(const FMapPalette& Palette, const FVector2f& Tip, float Size)
	{
		const FVector2f Head = Tip + FVector2f(0.f, -2.1f * Size);
		Triangle(Tip, Head + FVector2f(-0.85f * Size, 0.35f * Size), Head + FVector2f(0.85f * Size, 0.35f * Size), Linear(Palette.Waypoint));
		Circle(Head, Size, Linear(Palette.Waypoint));
		Circle(Head, 0.4f * Size, FLinearColor::White);
	}

	void Circle(const FVector2f& Centre, float Radius, const FLinearColor& Color)
	{
		constexpr int32 Segments = 24;
		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;
		const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
		const FColor VertexColor = Color.ToFColorSRGB();
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Centre, FVector2f(0.5f, 0.5f), VertexColor));
		for (int32 Step = 0; Step < Segments; ++Step)
		{
			const float Angle = 2.f * PI * Step / Segments;
			Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Centre + Radius * FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)),
				FVector2f(0.5f, 0.5f), VertexColor));
			Indices.Add(0);
			Indices.Add(Step + 1);
			Indices.Add(Step + 1 == Segments ? 1 : Step + 2);
		}
		FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, WhiteHandle, Vertices, Indices, nullptr, 0, 0);
	}

private:
	/** How a tile's local metres turn into pixels: a base point and the images of the local x and y axes. */
	struct FTileTransform
	{
		FVector2f Base = FVector2f::ZeroVector;
		FVector2f AxisX = FVector2f::ZeroVector;
		FVector2f AxisY = FVector2f::ZeroVector;

		FVector2f Apply(const FVector2f& Local) const { return Base + Local.X * AxisX + Local.Y * AxisY; }
	};

	static FTileTransform MakeTileTransform(const FMapScene& Scene, const FMapTile& Tile)
	{
		const FVector2f Forward(FMath::Cos(Scene.HeadingRadians), FMath::Sin(Scene.HeadingRadians));
		const FVector2f Right(-Forward.Y, Forward.X);
		const FVector2f Offset = Tile.OriginM - Scene.CentreWorld;
		FTileTransform Result;
		Result.Base = Scene.OriginPx + Scene.PixelsPerMetre * FVector2f(FVector2f::DotProduct(Offset, Right), -FVector2f::DotProduct(Offset, Forward));
		Result.AxisX = Scene.PixelsPerMetre * FVector2f(Right.X, -Forward.X);
		Result.AxisY = Scene.PixelsPerMetre * FVector2f(Right.Y, -Forward.Y);
		return Result;
	}

	FVector2f WorldToPixels(const FMapScene& Scene, const FVector2f& Point) const
	{
		const FVector2f View = MinimapGeometry::ToViewFrame(Point - Scene.CentreWorld, Scene.HeadingRadians);
		return Scene.OriginPx + Scene.PixelsPerMetre * FVector2f(View.X, -View.Y);
	}

	/** Whether any part of a tile can be on screen. */
	bool IsTileVisible(const FMapScene& Scene, const FMapTile& Tile) const
	{
		const FVector2f Centre = WorldToPixels(Scene, Tile.OriginM + FVector2f(0.5f * Tile.SizeM));
		const float HalfDiagonal = 0.71f * Tile.SizeM * Scene.PixelsPerMetre;
		const FVector2f Size = FVector2f(Geometry.GetLocalSize());
		return Centre.X > -HalfDiagonal && Centre.Y > -HalfDiagonal && Centre.X < Size.X + HalfDiagonal && Centre.Y < Size.Y + HalfDiagonal;
	}

	/** Every tile's land cover image, a little oversized so that neighbours overlap without seams. */
	void Landcover(const FMapScene& Scene)
	{
		for (const FMapTile* Tile : *Scene.Tiles)
		{
			if (!Tile->LandcoverBrush.IsValid() || !IsTileVisible(Scene, *Tile))
			{
				continue;
			}
			const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*Tile->LandcoverBrush);
			const FTileTransform Transform = MakeTileTransform(Scene, *Tile);
			const float Pad = 0.6f / FMath::Max(Scene.PixelsPerMetre, 0.01f);
			const FSlateRenderTransform Render = Geometry.GetAccumulatedRenderTransform();
			const FVector2f Corners[4] = {FVector2f(-Pad, -Pad), FVector2f(Tile->SizeM + Pad, -Pad), FVector2f(Tile->SizeM + Pad, Tile->SizeM + Pad), FVector2f(-Pad, Tile->SizeM + Pad)};
			const FVector2f Uvs[4] = {FVector2f(0.f, 0.f), FVector2f(1.f, 0.f), FVector2f(1.f, 1.f), FVector2f(0.f, 1.f)};
			TArray<FSlateVertex> Vertices;
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				const FVector2f Local = Corners[Corner];
				Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, Transform.Apply(Local), Uvs[Corner] * (Tile->SizeM + 2.f * Pad) / Tile->SizeM - FVector2f(Pad / Tile->SizeM), FColor::White));
			}
			TArray<SlateIndex> Indices = {0, 1, 2, 0, 2, 3};
			FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, Handle, Vertices, Indices, nullptr, 0, 0);
		}
	}

	void Water(const FMapScene& Scene)
	{
		for (const FMapTile* Tile : *Scene.Tiles)
		{
			if (Tile->Water.Indices.Num() > 0 && IsTileVisible(Scene, *Tile))
			{
				FilledLayer(Scene, *Tile, Tile->Water, Linear(Scene.Palette->Water));
			}
		}
	}

	void Buildings(const FMapScene& Scene)
	{
		for (int32 Style = 0; Style < 3; ++Style)
		{
			const FLinearColor Color = Linear(Scene.Palette->BuildingFill(static_cast<EMapBuildingStyle>(Style)));
			for (const FMapTile* Tile : *Scene.Tiles)
			{
				if (Tile->Buildings[Style].Indices.Num() > 0 && IsTileVisible(Scene, *Tile))
				{
					FilledLayer(Scene, *Tile, Tile->Buildings[Style], Color);
				}
			}
		}
	}

	void FilledLayer(const FMapScene& Scene, const FMapTile& Tile, const FMapTileLayer& Mesh, const FLinearColor& Color)
	{
		const FTileTransform Transform = MakeTileTransform(Scene, Tile);
		const FSlateRenderTransform Render = Geometry.GetAccumulatedRenderTransform();
		const FColor VertexColor = Color.ToFColorSRGB();
		Scratch.Reset();
		for (const FVector2f& Vertex : Mesh.Vertices)
		{
			Scratch.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, Transform.Apply(Vertex), FVector2f(0.5f, 0.5f), VertexColor));
		}
		FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, WhiteHandle, Scratch, Mesh.Indices, nullptr, 0, 0);
	}

	/** Pavements and footpaths as thin lines of about a pixel, like the green lines along the roads of Google Maps. */
	void Footpaths(const FMapScene& Scene)
	{
		const FColor VertexColor = Linear(Scene.Palette->Footpath).ToFColorSRGB();
		const FSlateRenderTransform Render = Geometry.GetAccumulatedRenderTransform();
		const float HalfWidthMeters = 0.5f * FootpathPixels * Scene.UiScale / Scene.PixelsPerMetre;
		for (const FMapTile* Tile : *Scene.Tiles)
		{
			const FMapTileStrips& Strips = Tile->Footpaths;
			if (Strips.Indices.Num() == 0 || !IsTileVisible(Scene, *Tile))
			{
				continue;
			}
			const FTileTransform Transform = MakeTileTransform(Scene, *Tile);
			Scratch.Reset();
			for (int32 Index = 0; Index < Strips.Points.Num(); ++Index)
			{
				const FVector2f Offset = HalfWidthMeters * Strips.Normals[Index];
				Scratch.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, Transform.Apply(Strips.Points[Index] + Offset), FVector2f(0.5f, 0.5f), VertexColor));
				Scratch.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Render, Transform.Apply(Strips.Points[Index] - Offset), FVector2f(0.5f, 0.5f), VertexColor));
			}
			FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, WhiteHandle, Scratch, Strips.Indices, nullptr, 0, 0);
		}
	}

	/** Roads: all edges first, then all fills, so that crossings merge; then arrows and the route over them. */
	void Roads(const FMapScene& Scene)
	{
		for (int32 Tier = 0; Tier <= 4; ++Tier)
		{
			Strokes(Scene, Tier, /*bEdge=*/true);
		}
		for (int32 Tier = 0; Tier <= 4; ++Tier)
		{
			Strokes(Scene, Tier, /*bEdge=*/false);
		}
		if (Scene.PixelsPerMetre >= ArrowMinimumPixelsPerMetre)
		{
			OneWayArrows(Scene);
		}
		Strokes(Scene, MinimapRouteTier, /*bEdge=*/true);
		Strokes(Scene, MinimapRouteTier, /*bEdge=*/false);
	}

	void Strokes(const FMapScene& Scene, int32 Tier, bool bEdge)
	{
		const bool bRoute = Tier == MinimapRouteTier;
		const float FillWidth = bRoute ? 7.f * Scene.UiScale
			: FMath::Max(RoadMinimumPixels[Tier] * Scene.UiScale, RoadWidthMeters[Tier] * Scene.PixelsPerMetre);
		const float Width = bEdge ? FillWidth + (bRoute ? 3.f : 1.5f) * Scene.UiScale : FillWidth;
		FLinearColor Color = Linear(bEdge ? Scene.Palette->RoadEdge : Scene.Palette->Road[FMath::Min(Tier, 4)]);
		if (bRoute)
		{
			Color = Linear(bEdge ? Scene.Palette->RouteEdge : Scene.Palette->Route);
		}
		for (const FMinimapStroke& Stroke : Scene.Frame->Strokes)
		{
			if (Stroke.Tier != Tier || Stroke.PointCount < 2)
			{
				continue;
			}
			ScratchLine.Reset();
			for (int32 Offset = 0; Offset < Stroke.PointCount; ++Offset)
			{
				const FVector2f& Point = Scene.Frame->Points[Stroke.FirstPoint + Offset];
				ScratchLine.Add(Scene.OriginPx + FVector2f(Point.X, -Point.Y) * Scene.PixelsPerMetre);
			}
			FSlateDrawElement::MakeLines(Elements, ++Layer, Geometry.ToPaintGeometry(), ScratchLine, ESlateDrawEffect::None, Color, true, Width);
		}
	}

	/** Small arrows along one-way roads. */
	void OneWayArrows(const FMapScene& Scene)
	{
		const FLinearColor Color = Linear(Scene.Palette->OneWayArrow);
		const FVector2f Size = FVector2f(Geometry.GetLocalSize());
		const float Half = 3.5f * Scene.UiScale;
		for (const FMinimapArrow& Arrow : Scene.Frame->Arrows)
		{
			const FVector2f Position = Scene.OriginPx + FVector2f(Arrow.Position.X, -Arrow.Position.Y) * Scene.PixelsPerMetre;
			if (Position.X < 0.f || Position.Y < 0.f || Position.X > Size.X || Position.Y > Size.Y)
			{
				continue;
			}
			const FVector2f Forward(Arrow.Direction.X, -Arrow.Direction.Y);
			const FVector2f Side(-Forward.Y, Forward.X);
			Triangle(Position + 1.4f * Half * Forward, Position - Half * Forward + 0.9f * Half * Side, Position - 0.3f * Half * Forward, Color);
			Triangle(Position + 1.4f * Half * Forward, Position - 0.3f * Half * Forward, Position - Half * Forward - 0.9f * Half * Side, Color);
		}
	}

	void Triangle(const FVector2f& A, const FVector2f& B, const FVector2f& C, const FLinearColor& Color)
	{
		const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
		const FColor VertexColor = Color.ToFColorSRGB();
		TArray<FSlateVertex> Vertices;
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, A, FVector2f(0.5f, 0.5f), VertexColor));
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, B, FVector2f(0.5f, 0.5f), VertexColor));
		Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, C, FVector2f(0.5f, 0.5f), VertexColor));
		TArray<SlateIndex> Indices = {0, 1, 2};
		FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, WhiteHandle, Vertices, Indices, nullptr, 0, 0);
	}

	FSlateWindowElementList& Elements;
	const FGeometry& Geometry;
	int32 Layer;
	FSlateBrush PillBrush;
	FSlateResourceHandle WhiteHandle;
	TArray<FSlateVertex> Scratch;
	TArray<FVector2f> ScratchLine;
};

FString DescribeDistance(float Meters)
{
	if (Meters >= 1000.f)
	{
		return FString::Printf(TEXT("%.1f km"), Meters / 1000.f);
	}
	return FString::Printf(TEXT("%d m"), FMath::RoundToInt(Meters / 10.f) * 10);
}

/** Text on a white rounded label, centred on a position. */
void LabelPill(FMapPainter& Painter, const FMapPalette& Palette, const FString& Content, const FVector2f& Centre, float FontSize,
	const FLinearColor& TextColor, float UiScale)
{
	const FVector2f PillSize(Painter.TextWidth(Content, FontSize) + 16.f * UiScale, FontSize * 1.9f);
	FLinearColor Background = Linear(Palette.LabelBackground);
	Background.A = 0.92f;
	Painter.Pill(Centre, PillSize, Background);
	Painter.Label(Content, Centre, FontSize, TextColor);
}
}

// ---------------------------------------------------------------------------------------------------------------------
// Minimap
// ---------------------------------------------------------------------------------------------------------------------

void SMinimapContent::Construct(const FArguments& InArgs, UMinimapSubsystem* InOwner)
{
	Owner = InOwner;
}

int32 SMinimapContent::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const UMinimapSubsystem* Subsystem = Owner.Get();
	if (!Subsystem)
	{
		return LayerId;
	}
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	const float UiScale = Size.X / MinimapDisplayPx;
	const float RadiusMeters = Subsystem->GetMinimapRadiusMeters();

	const FMapPalette& Palette = Subsystem->GetPalette();
	FMapScene Scene;
	Scene.Palette = &Palette;
	Scene.Frame = &Subsystem->GetMinimapFrame();
	Scene.Tiles = &Subsystem->GetMinimapTiles();
	Scene.CentreWorld = Subsystem->GetCarPositionMeters();
	Scene.HeadingRadians = Subsystem->GetMinimapHeadingRadians();
	Scene.OriginPx = 0.5f * Size;
	Scene.PixelsPerMetre = 0.5f * Size.X / RadiusMeters;
	Scene.UiScale = UiScale;
	Scene.bBuildings = true;

	// The 3D picture, when it is on, replaces the flat map and brings its own car and waypoint.
	UMap3DSubsystem* Map3D = Subsystem->GetWorld()->GetSubsystem<UMap3DSubsystem>();
	const bool b3D = Map3D && Map3D->PaintMinimap(OutDrawElements, AllottedGeometry, LayerId);
	FMapPainter Painter(OutDrawElements, AllottedGeometry, LayerId);
	if (!b3D)
	{
		Painter.Scene(Scene);
		FVector2f WaypointView;
		if (Subsystem->GetMinimapWaypoint(WaypointView))
		{
			Painter.WaypointPin(Palette, Scene.OriginPx + FVector2f(WaypointView.X, -WaypointView.Y) * Scene.PixelsPerMetre, 8.f * UiScale);
		}
		Painter.CarMarker(Palette, Scene.OriginPx, 0.f, 11.f * UiScale);
	}

	// North sits on the rim, in the direction the world's north has in the car's frame.
	const FVector2f NorthDirection = MinimapGeometry::ToViewFrame(FVector2f(0.f, -1.f), Subsystem->GetMinimapHeadingRadians());
	const FVector2f NorthPosition = Scene.OriginPx + (0.5f * Size.X - 26.f * UiScale) * FVector2f(NorthDirection.X, -NorthDirection.Y);
	const FLinearColor LabelColor = Linear(Palette.Label);
	FLinearColor CompassBackground = Linear(Palette.LabelBackground);
	CompassBackground.A = 0.95f;
	Painter.Circle(NorthPosition, 13.f * UiScale, CompassBackground);
	Painter.Label(TEXT("N"), NorthPosition, 13.f * UiScale, Linear(Palette.Waypoint));
	LabelPill(Painter, Palette, DescribeDistance(RadiusMeters), FVector2f(48.f * UiScale, Size.Y - 24.f * UiScale), 11.f * UiScale, LabelColor, UiScale);
	float RemainingMeters = 0.f;
	if (Subsystem->GetRemainingRouteMeters(RemainingMeters))
	{
		LabelPill(Painter, Palette, DescribeDistance(RemainingMeters), FVector2f(Scene.OriginPx.X, Size.Y - 28.f * UiScale), 17.f * UiScale,
			Linear(Palette.Route), UiScale);
	}
	return Painter.GetLayer() + 1;
}

void SMinimapWidget::Construct(const FArguments& InArgs, UTextureRenderTarget2D* InImage)
{
	ImageBrush = MakeShared<FSlateBrush>();
	ImageBrush->DrawAs = ESlateBrushDrawType::RoundedBox;
	ImageBrush->ImageSize = FVector2f(MinimapContentPx, MinimapContentPx);
	ImageBrush->SetResourceObject(InImage);
	ImageBrush->OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
	ImageBrush->OutlineSettings.CornerRadii = FVector4(MinimapCornerRadiusPx, MinimapCornerRadiusPx, MinimapCornerRadiusPx, MinimapCornerRadiusPx);
	ImageBrush->OutlineSettings.Color = FLinearColor::Transparent;
	ImageBrush->OutlineSettings.Width = 0.f;

	ShadowBrush = MakeShared<FSlateBrush>();
	ShadowBrush->DrawAs = ESlateBrushDrawType::RoundedBox;
	ShadowBrush->OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
	ShadowBrush->OutlineSettings.CornerRadii = FVector4(MinimapCornerRadiusPx, MinimapCornerRadiusPx, MinimapCornerRadiusPx, MinimapCornerRadiusPx);
	ShadowBrush->OutlineSettings.Color = FLinearColor::Transparent;
	ShadowBrush->OutlineSettings.Width = 0.f;
}

FVector2D SMinimapWidget::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(MinimapDisplayPx + 2.f * MinimapShadowMarginPx, MinimapDisplayPx + 2.f * MinimapShadowMarginPx);
}

int32 SMinimapWidget::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	// A soft shadow: a few rounded boxes growing outwards and fading, a little lower than the panel.
	constexpr int32 ShadowSteps = 6;
	for (int32 Step = ShadowSteps; Step >= 1; --Step)
	{
		const float Grow = static_cast<float>(Step) * 1.8f;
		const FVector2f Size(MinimapDisplayPx + 2.f * Grow, MinimapDisplayPx + 2.f * Grow);
		const FVector2f TopLeft(MinimapShadowMarginPx - Grow, MinimapShadowMarginPx - Grow + 3.f);
		FSlateDrawElement::MakeBox(OutDrawElements, ++LayerId, AllottedGeometry.ToPaintGeometry(Size, FSlateLayoutTransform(TopLeft)),
			ShadowBrush.Get(), ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.045f));
	}
	FSlateDrawElement::MakeBox(OutDrawElements, ++LayerId,
		AllottedGeometry.ToPaintGeometry(FVector2f(MinimapDisplayPx, MinimapDisplayPx), FSlateLayoutTransform(FVector2f(MinimapShadowMarginPx))),
		ImageBrush.Get(), ESlateDrawEffect::None, FLinearColor::White);
	return LayerId + 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Full map
// ---------------------------------------------------------------------------------------------------------------------

void SFullMapWidget::Construct(const FArguments& InArgs, UMinimapSubsystem* InOwner)
{
	Owner = InOwner;
}

FVector2f SFullMapWidget::OffsetFromCentre(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) const
{
	const FVector2f Local = FVector2f(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));
	return Local - 0.5f * FVector2f(MyGeometry.GetLocalSize());
}

FReply SFullMapWidget::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		// A right click clears the waypoint; a right drag turns and tilts the 3D map.
		bRightDown = true;
		bDragged = false;
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}
	bOrbiting = MouseEvent.IsControlDown();
	bLeftDown = true;
	bDragged = false;
	PressPosition = OffsetFromCentre(MyGeometry, MouseEvent);
	return FReply::Handled().CaptureMouse(SharedThis(this));
}

FReply SFullMapWidget::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	UMinimapSubsystem* Subsystem = Owner.Get();
	if (!(bLeftDown || bRightDown) || !Subsystem)
	{
		return FReply::Unhandled();
	}
	const FVector2f Delta = FVector2f(MouseEvent.GetCursorDelta()) / MyGeometry.Scale;
	if (bRightDown)
	{
		bDragged = bDragged || Delta.SizeSquared() > 0.f;
		OrbitMap(Delta);
		return FReply::Handled();
	}
	const FVector2f Position = OffsetFromCentre(MyGeometry, MouseEvent);
	if (bDragged || FVector2f::Distance(Position, PressPosition) > 4.f)
	{
		bDragged = true;
		if (bOrbiting)
		{
			OrbitMap(Delta);
		}
		else
		{
			Subsystem->PanFullMap(Delta);
		}
	}
	return FReply::Handled();
}

void SFullMapWidget::OrbitMap(const FVector2f& CursorDeltaPixels)
{
	UMap3DSubsystem* Map3D = Owner.IsValid() ? Owner->GetWorld()->GetSubsystem<UMap3DSubsystem>() : nullptr;
	if (Map3D)
	{
		Map3D->OrbitFullMap(CursorDeltaPixels);
	}
}

FReply SFullMapWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton && bRightDown)
	{
		bRightDown = false;
		UMinimapSubsystem* Subsystem = Owner.Get();
		if (Subsystem && !bDragged)
		{
			Subsystem->ClearWaypoint();
		}
		return FReply::Handled().ReleaseMouseCapture();
	}
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !bLeftDown)
	{
		return FReply::Unhandled();
	}
	bLeftDown = false;
	UMinimapSubsystem* Subsystem = Owner.Get();
	if (Subsystem && !bDragged)
	{
		Subsystem->SetWaypointFromFullMap(OffsetFromCentre(MyGeometry, MouseEvent));
	}
	return FReply::Handled().ReleaseMouseCapture();
}

FReply SFullMapWidget::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (UMinimapSubsystem* Subsystem = Owner.Get())
	{
		Subsystem->ZoomFullMap(MouseEvent.GetWheelDelta(), OffsetFromCentre(MyGeometry, MouseEvent));
	}
	return FReply::Handled();
}

int32 SFullMapWidget::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	UMinimapSubsystem* Subsystem = Owner.Get();
	if (!Subsystem)
	{
		return LayerId;
	}
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	// The 3D picture, when it is on, replaces the flat map and brings its own car and waypoint.
	UMap3DSubsystem* Map3D = Subsystem->GetWorld()->GetSubsystem<UMap3DSubsystem>();
	const bool b3D = Map3D && Map3D->PaintFullMap(OutDrawElements, AllottedGeometry, LayerId);
	if (!b3D)
	{
		Subsystem->PrepareFullMapFrame(Size);
	}

	const FMapPalette& Palette = Subsystem->GetPalette();
	FMapScene Scene;
	Scene.Palette = &Palette;
	Scene.Frame = &Subsystem->GetFullMapFrame();
	Scene.Tiles = &Subsystem->GetFullMapTiles();
	Scene.CentreWorld = Subsystem->GetFullMapCentreMeters();
	Scene.HeadingRadians = -HALF_PI;
	Scene.OriginPx = 0.5f * Size;
	Scene.PixelsPerMetre = 1.f / Subsystem->GetFullMapMetersPerPixel();
	Scene.bBuildings = Subsystem->AreFullMapBuildingsShown();

	FMapPainter Painter(OutDrawElements, AllottedGeometry, LayerId);
	if (b3D)
	{
		// North on a small compass, since the picture can be turned.
		const float NorthAngle = Map3D->GetFullMapNorthAngleRadians();
		const FVector2f CompassCentre(Size.X - 56.f, 56.f);
		Painter.Circle(CompassCentre, 26.f, Linear(Palette.LabelBackground));
		Painter.Label(TEXT("N"), CompassCentre + 15.f * FVector2f(FMath::Sin(NorthAngle), -FMath::Cos(NorthAngle)), 14.f, Linear(Palette.Waypoint));
	}
	else
	{
		Painter.Scene(Scene);
		FVector2f WaypointView;
		if (Subsystem->GetFullMapWaypoint(WaypointView))
		{
			Painter.WaypointPin(Palette, Scene.OriginPx + FVector2f(WaypointView.X, -WaypointView.Y) * Scene.PixelsPerMetre, 9.f);
		}
		FVector2f CarView;
		float CarAngleRadians = 0.f;
		if (Subsystem->GetFullMapCar(CarView, CarAngleRadians))
		{
			Painter.CarMarker(Palette, Scene.OriginPx + FVector2f(CarView.X, -CarView.Y) * Scene.PixelsPerMetre, CarAngleRadians, 11.f);
		}
	}

	const FLinearColor LabelColor = Linear(Palette.Label);
	LabelPill(Painter, Palette, TEXT("Click: waypoint     Right click: clear     Drag: move     Wheel: zoom     M: close"), FVector2f(Scene.OriginPx.X, 28.f), 14.f, LabelColor, 1.f);
	float RemainingMeters = 0.f;
	if (Subsystem->GetRemainingRouteMeters(RemainingMeters))
	{
		LabelPill(Painter, Palette, FString::Printf(TEXT("Route: %s"), *DescribeDistance(RemainingMeters)), FVector2f(Scene.OriginPx.X, 64.f), 20.f,
			Linear(Palette.Route), 1.f);
	}
	if (!b3D)
	{
		const float ScaleMeters = Subsystem->GetFullMapScaleBarMeters();
		const float BarPixels = ScaleMeters * Scene.PixelsPerMetre;
		const FVector2f BarStart(40.f, Size.Y - 40.f);
		Painter.Line(BarStart, BarStart + FVector2f(BarPixels, 0.f), LabelColor, 3.f);
		LabelPill(Painter, Palette, DescribeDistance(ScaleMeters), BarStart + FVector2f(0.5f * BarPixels, -18.f), 12.f, LabelColor, 1.f);
	}
	return Painter.GetLayer() + 1;
}
