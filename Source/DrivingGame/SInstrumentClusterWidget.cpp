#include "SInstrumentClusterWidget.h"

#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"

namespace
{
constexpr float ClusterPi = 3.14159265f;

/** First dial angle in degrees clockwise from the screen's +X axis: 135 is the lower left. */
constexpr float DialStartDegrees = 135.f;
constexpr float DialSweepDegrees = 270.f;

constexpr float SpeedometerMaxKmh = 260.f;
constexpr float TachometerMaxRpm = 8000.f;
constexpr float RedZoneStartRpm = 6500.f;

const FLinearColor DialFaceColor(0.004f, 0.004f, 0.005f);
const FLinearColor BezelColor(0.34f, 0.35f, 0.37f);
const FLinearColor NumeralColor(0.92f, 0.93f, 0.95f);
const FLinearColor DimPrintColor(0.07f, 0.07f, 0.075f);
const FLinearColor NeedleColor(0.95f, 0.06f, 0.03f);
const FLinearColor RedLampColor(0.95f, 0.08f, 0.06f);
const FLinearColor AmberLampColor(1.f, 0.55f, 0.02f);
const FLinearColor GreenLampColor(0.08f, 0.9f, 0.18f);
const FLinearColor BlueLampColor(0.12f, 0.38f, 1.f);

FVector2f OnCircle(const FVector2f& Center, float Radius, float Degrees)
{
	const float Radians = Degrees * ClusterPi / 180.f;
	return Center + Radius * FVector2f(FMath::Cos(Radians), FMath::Sin(Radians));
}

/** Draws one frame of the cluster; created per paint with the state to show. */
class FClusterPainter
{
public:
	FClusterPainter(FSlateWindowElementList& InElements, const FGeometry& InGeometry, int32 InLayer, const FClusterState& InState)
		: Elements(InElements), Geometry(InGeometry), Layer(InLayer), State(InState)
	{
		Size = FVector2f(Geometry.GetLocalSize());
		Unit = Size.Y;
		DiscBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
		DiscBrush.TintColor = FLinearColor::White;
		DiscBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
		DiscBrush.OutlineSettings.Color = FLinearColor::Transparent;
		DiscBrush.OutlineSettings.Width = 0.f;
		PanelBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
		PanelBrush.TintColor = FLinearColor::White;
		PanelBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		PanelBrush.OutlineSettings.CornerRadii = FVector4(0.04f * Unit, 0.04f * Unit, 0.04f * Unit, 0.04f * Unit);
		PanelBrush.OutlineSettings.Color = FLinearColor::Transparent;
		PanelBrush.OutlineSettings.Width = 0.f;
	}

	/** Paints everything in back to front order. */
	void Paint()
	{
		const float Radius = 0.47f * Unit;
		const FVector2f TachometerCentre(0.5f * Unit, 0.5f * Unit);
		const FVector2f SpeedometerCentre(Size.X - 0.5f * Unit, 0.5f * Unit);

		PaintDialFace(TachometerCentre, Radius);
		PaintDialFace(SpeedometerCentre, Radius);
		PaintTachometerScale(TachometerCentre, Radius);
		PaintSpeedometerScale(SpeedometerCentre, Radius);
		PaintCoolantGauge(TachometerCentre, Radius);
		PaintFuelGauge(SpeedometerCentre, Radius);
		PaintCentreDisplay(TachometerCentre.X + Radius, SpeedometerCentre.X - Radius);
		PaintTellTales(0.5f * Size.X);
		PaintWarningLamps(0.5f * Size.X);
		PaintNeedle(TachometerCentre, Radius, NeedleFraction(State.Rpm / TachometerMaxRpm));
		PaintNeedle(SpeedometerCentre, Radius, NeedleFraction(FMath::Abs(State.SpeedKmh) / SpeedometerMaxKmh));
	}

private:
	/** Needle position 0..1 along the scale: the start-up sweep overrides the real value; no power means rest. */
	float NeedleFraction(float Value) const
	{
		if (!State.bPowered)
		{
			return 0.f;
		}
		if (State.SweepFraction >= 0.f)
		{
			return State.SweepFraction;
		}
		return FMath::Clamp(Value, 0.f, 1.02f);
	}

	FLinearColor PrintColor() const
	{
		return State.bPowered ? NumeralColor * State.Backlight : DimPrintColor;
	}

	FSlateLayoutTransform Placement(const FVector2f& Position) const { return FSlateLayoutTransform(Position); }

	void Disc(const FVector2f& Centre, float Radius, const FLinearColor& Color)
	{
		const FVector2f Extent(2.f * Radius, 2.f * Radius);
		FSlateDrawElement::MakeBox(Elements, ++Layer, Geometry.ToPaintGeometry(Extent, Placement(Centre - 0.5f * Extent)),
			&DiscBrush, ESlateDrawEffect::None, Color);
	}

	void Panel(const FVector2f& TopLeft, const FVector2f& PanelSize, const FLinearColor& Color)
	{
		FSlateDrawElement::MakeBox(Elements, ++Layer, Geometry.ToPaintGeometry(PanelSize, Placement(TopLeft)),
			&PanelBrush, ESlateDrawEffect::None, Color);
	}

	void Line(const FVector2f& From, const FVector2f& To, const FLinearColor& Color, float Thickness)
	{
		TArray<FVector2f> Points = {From, To};
		FSlateDrawElement::MakeLines(Elements, ++Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true, Thickness);
	}

	void Polyline(const TArray<FVector2f>& Points, const FLinearColor& Color, float Thickness)
	{
		FSlateDrawElement::MakeLines(Elements, ++Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color, true, Thickness);
	}

	void Arc(const FVector2f& Centre, float Radius, float StartDegrees, float EndDegrees, const FLinearColor& Color, float Thickness)
	{
		TArray<FVector2f> Points;
		const int32 Steps = FMath::Max(2, FMath::CeilToInt(FMath::Abs(EndDegrees - StartDegrees) / 2.f));
		for (int32 Step = 0; Step <= Steps; ++Step)
		{
			Points.Add(OnCircle(Centre, Radius, FMath::Lerp(StartDegrees, EndDegrees, static_cast<float>(Step) / Steps)));
		}
		Polyline(Points, Color, Thickness);
	}

	/** A filled convex polygon, drawn as a triangle fan with an anti-aliased outline. */
	void Polygon(const TArray<FVector2f>& Points, const FLinearColor& Color)
	{
		if (Points.Num() < 3)
		{
			return;
		}
		const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("GenericWhiteBox");
		const FSlateResourceHandle Handle = FSlateApplication::Get().GetRenderer()->GetResourceHandle(*WhiteBrush);
		const FSlateRenderTransform Transform = Geometry.GetAccumulatedRenderTransform();
		const FColor VertexColor = Color.ToFColorSRGB();
		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;
		for (const FVector2f& Point : Points)
		{
			Vertices.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Point, FVector2f(0.5f, 0.5f), VertexColor));
		}
		for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
		{
			Indices.Add(0);
			Indices.Add(Index);
			Indices.Add(Index + 1);
		}
		FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, Handle, Vertices, Indices, nullptr, 0, 0);
		TArray<FVector2f> Outline = Points;
		Outline.Add(Points[0]);
		Polyline(Outline, Color, 1.6f);
	}

	enum class EAnchor
	{
		Left,
		Centre,
		Right,
	};

	void Text(const FString& Content, const FVector2f& Anchor, float FontSize, const FLinearColor& Color, EAnchor Alignment, const char* Weight = "Bold")
	{
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(Weight, FMath::RoundToInt(FontSize));
		const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		const FVector2f TextSize = FVector2f(Measure->Measure(Content, Font));
		FVector2f TopLeft = Anchor - 0.5f * TextSize;
		if (Alignment == EAnchor::Left)
		{
			TopLeft.X = Anchor.X;
		}
		else if (Alignment == EAnchor::Right)
		{
			TopLeft.X = Anchor.X - TextSize.X;
		}
		FSlateDrawElement::MakeText(Elements, ++Layer, Geometry.ToPaintGeometry(TextSize, Placement(TopLeft)), Content, Font, ESlateDrawEffect::None, Color);
	}

	void PaintDialFace(const FVector2f& Centre, float Radius)
	{
		const float BezelDimming = FMath::Lerp(0.3f, 1.f, State.Backlight); // the chrome ring catches the backlight
		Disc(Centre, Radius, BezelColor * BezelDimming);
		Disc(Centre, Radius * 0.975f, FLinearColor(0.12f, 0.125f, 0.13f) * BezelDimming);
		Disc(Centre, Radius * 0.95f, DialFaceColor);
	}

	/** Ticks and numerals along a dial's scale. */
	void PaintScale(const FVector2f& Centre, float Radius, float MaxValue, float MajorStep, int32 MinorPerMajor,
		float LabelDivisor, float LabelSize, const FLinearColor& Color)
	{
		const int32 MajorCount = FMath::RoundToInt(MaxValue / MajorStep);
		const int32 TickCount = MajorCount * MinorPerMajor;
		for (int32 Tick = 0; Tick <= TickCount; ++Tick)
		{
			const bool bMajor = Tick % MinorPerMajor == 0;
			const float Angle = DialStartDegrees + DialSweepDegrees * Tick / TickCount;
			const float Inner = Radius * (bMajor ? 0.80f : 0.86f);
			Line(OnCircle(Centre, Inner, Angle), OnCircle(Centre, Radius * 0.92f, Angle), Color, bMajor ? 0.012f * Unit : 0.006f * Unit);
			if (bMajor)
			{
				const float Value = Tick / MinorPerMajor * MajorStep / LabelDivisor;
				Text(FString::Printf(TEXT("%d"), FMath::RoundToInt(Value)), OnCircle(Centre, Radius * 0.65f, Angle), LabelSize * Radius, Color, EAnchor::Centre);
			}
		}
	}

	void PaintSpeedometerScale(const FVector2f& Centre, float Radius)
	{
		PaintScale(Centre, Radius, SpeedometerMaxKmh, 20.f, 2, 1.f, 0.078f, PrintColor());
		Text(TEXT("km/h"), Centre + FVector2f(0.f, 0.22f * Radius), 0.058f * Radius * 2.f, PrintColor() * 0.8f, EAnchor::Centre, "Regular");
	}

	void PaintTachometerScale(const FVector2f& Centre, float Radius)
	{
		PaintScale(Centre, Radius, TachometerMaxRpm, 1000.f, 2, 1000.f, 0.115f, PrintColor());
		const float RedStart = DialStartDegrees + DialSweepDegrees * RedZoneStartRpm / TachometerMaxRpm;
		const FLinearColor Red = State.bPowered ? RedLampColor * State.Backlight : DimPrintColor;
		Arc(Centre, Radius * 0.945f, RedStart, DialStartDegrees + DialSweepDegrees, Red, 0.014f * Unit);
		Text(TEXT("1/min x1000"), Centre + FVector2f(0.f, 0.22f * Radius), 0.054f * Radius * 2.f, PrintColor() * 0.8f, EAnchor::Centre, "Regular");
	}

	/** A small gauge in the open lower part of a dial: an arc with a needle and a caption. */
	void PaintMiniGauge(const FVector2f& Centre, float Radius, float Fraction, const TCHAR* Low, const TCHAR* High, const FLinearColor& ArcColor)
	{
		const FLinearColor Scale = State.bPowered ? ArcColor * State.Backlight : DimPrintColor;
		Arc(Centre, Radius, 200.f, 340.f, Scale, 0.008f * Unit);
		Text(Low, OnCircle(Centre, Radius * 1.45f, 195.f), 0.036f * Unit, PrintColor(), EAnchor::Centre, "Regular");
		Text(High, OnCircle(Centre, Radius * 1.45f, 345.f), 0.036f * Unit, PrintColor(), EAnchor::Centre, "Regular");
		if (!State.bPowered)
		{
			return;
		}
		const float Angle = 200.f + 140.f * FMath::Clamp(Fraction, 0.f, 1.f);
		Line(Centre, OnCircle(Centre, Radius * 1.1f, Angle), NeedleColor, 0.009f * Unit);
		Disc(Centre, 0.012f * Unit, NeedleColor);
	}

	void PaintCoolantGauge(const FVector2f& DialCentre, float DialRadius)
	{
		const FVector2f Centre = DialCentre + FVector2f(0.f, 0.60f * DialRadius);
		PaintMiniGauge(Centre, 0.17f * DialRadius, State.CoolantFraction, TEXT("C"), TEXT("H"), AmberLampColor);
	}

	void PaintFuelGauge(const FVector2f& DialCentre, float DialRadius)
	{
		const FVector2f Centre = DialCentre + FVector2f(0.f, 0.60f * DialRadius);
		PaintMiniGauge(Centre, 0.17f * DialRadius, State.FuelFraction, TEXT("0"), TEXT("1"), NumeralColor);
	}

	/** Needle with a tapered body, a short tail and a hub cap. */
	void PaintNeedle(const FVector2f& Centre, float Radius, float Fraction)
	{
		if (!State.bPowered)
		{
			return;
		}
		const float Angle = DialStartDegrees + DialSweepDegrees * Fraction;
		const float Radians = Angle * ClusterPi / 180.f;
		const FVector2f Direction(FMath::Cos(Radians), FMath::Sin(Radians));
		const FVector2f Across(-Direction.Y, Direction.X);
		const FLinearColor Color = NeedleColor * FMath::Lerp(0.8f, 1.f, State.Backlight);
		TArray<FVector2f> Body = {
			Centre - 0.2f * Radius * Direction + 0.016f * Unit * Across,
			Centre + 0.93f * Radius * Direction + 0.003f * Unit * Across,
			Centre + 0.93f * Radius * Direction - 0.003f * Unit * Across,
			Centre - 0.2f * Radius * Direction - 0.016f * Unit * Across,
		};
		Polygon(Body, Color);
		Disc(Centre, 0.048f * Unit, FLinearColor(0.02f, 0.02f, 0.022f));
		Disc(Centre, 0.036f * Unit, FLinearColor(0.16f, 0.165f, 0.17f));
	}

	/** The panel between the dials: time and outside temperature, gear and speed, odometer. */
	void PaintCentreDisplay(float LeftEdge, float RightEdge)
	{
		const float Margin = 0.02f * Unit;
		const FVector2f TopLeft(LeftEdge + Margin, 0.2f * Unit);
		const FVector2f PanelSize(RightEdge - LeftEdge - 2.f * Margin, 0.6f * Unit);
		Panel(TopLeft, PanelSize, FLinearColor(0.012f, 0.014f, 0.017f));
		if (!State.bPowered)
		{
			return;
		}
		const FLinearColor Text1 = FLinearColor(0.9f, 0.92f, 0.95f) * State.Backlight;
		const FLinearColor Accent = FLinearColor(0.35f, 0.62f, 1.f) * State.Backlight;
		const float CentreX = TopLeft.X + 0.5f * PanelSize.X;
		const float Top = TopLeft.Y;

		const int32 Hours = FMath::FloorToInt(State.TimeOfDayHours);
		const int32 Minutes = FMath::FloorToInt(FMath::Fmod(State.TimeOfDayHours, 1.f) * 60.f);
		Text(FString::Printf(TEXT("%02d:%02d"), Hours, Minutes), FVector2f(TopLeft.X + 0.03f * Unit, Top + 0.06f * Unit), 0.058f * Unit, Text1, EAnchor::Left);
		Text(FString::Printf(TEXT("%.0f°C"), State.OutsideTemperatureCelsius), FVector2f(TopLeft.X + PanelSize.X - 0.03f * Unit, Top + 0.06f * Unit),
			0.058f * Unit, Text1, EAnchor::Right);
		const float RuleY = Top + 0.115f * Unit;
		Line(FVector2f(TopLeft.X + 0.02f * Unit, RuleY), FVector2f(TopLeft.X + PanelSize.X - 0.02f * Unit, RuleY), Accent * 0.5f, 0.004f * Unit);

		FString GearText = TEXT("N");
		if (State.Gear == -1)
		{
			GearText = TEXT("R");
		}
		else if (State.Gear > 0)
		{
			GearText = FString::FromInt(State.Gear);
		}
		Text(GearText, FVector2f(CentreX - 0.12f * Unit, Top + 0.30f * Unit), 0.2f * Unit, Accent, EAnchor::Centre);
		Text(FString::Printf(TEXT("%.0f"), FMath::Abs(State.SpeedKmh)), FVector2f(CentreX + 0.2f * Unit, Top + 0.275f * Unit), 0.14f * Unit, Text1, EAnchor::Centre);
		Text(TEXT("km/h"), FVector2f(CentreX + 0.2f * Unit, Top + 0.385f * Unit), 0.04f * Unit, Text1 * 0.7f, EAnchor::Centre, "Regular");

		const FString Odometer = FString::Printf(TEXT("%s km"), *FText::AsNumber(FMath::FloorToInt(State.OdometerKm)).ToString());
		Text(Odometer, FVector2f(CentreX, Top + 0.5f * Unit), 0.045f * Unit, Text1 * 0.85f, EAnchor::Centre, "Regular");
		if (State.Message.IsEmpty())
		{
			Text(FString::Printf(TEXT("%.1f km"), State.TripKm), FVector2f(CentreX, Top + 0.555f * Unit), 0.04f * Unit, Text1 * 0.6f, EAnchor::Centre, "Regular");
			return;
		}
		Panel(FVector2f(TopLeft.X + 0.02f * Unit, Top + 0.53f * Unit), FVector2f(PanelSize.X - 0.04f * Unit, 0.056f * Unit), FLinearColor(0.35f, 0.18f, 0.f) * State.Backlight);
		Text(State.Message, FVector2f(CentreX, Top + 0.558f * Unit), 0.034f * Unit, FLinearColor(1.f, 0.8f, 0.4f) * State.Backlight, EAnchor::Centre);
	}

	void LampArrow(const FVector2f& Centre, bool bPointsLeft, const FLinearColor& Color)
	{
		const float Direction = bPointsLeft ? -1.f : 1.f;
		const float S = 0.046f * Unit;
		TArray<FVector2f> Head = {
			Centre + FVector2f(Direction * S, 0.f),
			Centre + FVector2f(Direction * 0.1f * S, -S),
			Centre + FVector2f(Direction * 0.1f * S, S),
		};
		Polygon(Head, Color);
		TArray<FVector2f> Tail = {
			Centre + FVector2f(Direction * 0.1f * S, -0.38f * S),
			Centre + FVector2f(-Direction * S, -0.38f * S),
			Centre + FVector2f(-Direction * S, 0.38f * S),
			Centre + FVector2f(Direction * 0.1f * S, 0.38f * S),
		};
		Polygon(Tail, Color);
	}

	/** Headlamp symbol: a D shaped lens with beams, slanted down for the low beam and level for the high beam. */
	void LampBeam(const FVector2f& Centre, bool bHigh, const FLinearColor& Color)
	{
		const float S = 0.042f * Unit;
		TArray<FVector2f> Lens;
		for (int32 Step = 0; Step <= 10; ++Step)
		{
			const float Angle = -90.f + 180.f * Step / 10.f;
			Lens.Add(Centre + FVector2f(-0.35f * S, 0.f) + FVector2f(0.65f * S * FMath::Cos(Angle * ClusterPi / 180.f), S * FMath::Sin(Angle * ClusterPi / 180.f)));
		}
		Lens.Add(Centre + FVector2f(-0.35f * S, S));
		Lens.Add(Centre + FVector2f(-0.35f * S, -S));
		Polyline(Lens, Color, 0.007f * Unit);
		for (int32 Ray = 0; Ray < 4; ++Ray)
		{
			const float Y = (-0.7f + 0.47f * Ray) * S;
			const float Slant = bHigh ? 0.f : 0.28f * S;
			Line(Centre + FVector2f(-0.55f * S, Y), Centre + FVector2f(-1.45f * S, Y + Slant), Color, 0.006f * Unit);
		}
	}

	void PaintTellTales(float CentreX)
	{
		if (!State.bPowered)
		{
			return;
		}
		const float Y = 0.1f * Unit;
		const float Gap = 0.15f * Unit;
		const FLinearColor Green = GreenLampColor * State.Backlight;
		if (State.bLeftIndicator)
		{
			LampArrow(FVector2f(CentreX - 1.5f * Gap, Y), true, Green);
		}
		if (State.bRightIndicator)
		{
			LampArrow(FVector2f(CentreX + 1.5f * Gap, Y), false, Green);
		}
		if (State.bLowBeam && !State.bHighBeam)
		{
			LampBeam(FVector2f(CentreX - 0.5f * Gap + 0.02f * Unit, Y), false, Green);
		}
		if (State.bHighBeam)
		{
			LampBeam(FVector2f(CentreX + 0.5f * Gap + 0.02f * Unit, Y), true, BlueLampColor * State.Backlight);
		}
	}

	void LampEngine(const FVector2f& Centre, const FLinearColor& Color)
	{
		const float S = 0.045f * Unit;
		TArray<FVector2f> Block = {
			Centre + FVector2f(-S, -0.35f * S), Centre + FVector2f(-0.4f * S, -0.35f * S), Centre + FVector2f(-0.25f * S, -0.8f * S),
			Centre + FVector2f(0.5f * S, -0.8f * S), Centre + FVector2f(0.5f * S, -0.45f * S), Centre + FVector2f(0.95f * S, -0.45f * S),
			Centre + FVector2f(0.95f * S, 0.75f * S), Centre + FVector2f(-0.4f * S, 0.75f * S), Centre + FVector2f(-0.7f * S, 0.4f * S),
			Centre + FVector2f(-S, 0.4f * S), Centre + FVector2f(-S, -0.35f * S),
		};
		Polyline(Block, Color, 0.007f * Unit);
	}

	void LampBattery(const FVector2f& Centre, const FLinearColor& Color)
	{
		const float S = 0.045f * Unit;
		TArray<FVector2f> Box = {
			Centre + FVector2f(-S, -0.55f * S), Centre + FVector2f(S, -0.55f * S), Centre + FVector2f(S, 0.65f * S),
			Centre + FVector2f(-S, 0.65f * S), Centre + FVector2f(-S, -0.55f * S),
		};
		Polyline(Box, Color, 0.007f * Unit);
		Line(Centre + FVector2f(-0.55f * S, -0.55f * S), Centre + FVector2f(-0.55f * S, -0.85f * S), Color, 0.009f * Unit);
		Line(Centre + FVector2f(0.55f * S, -0.55f * S), Centre + FVector2f(0.55f * S, -0.85f * S), Color, 0.009f * Unit);
		Line(Centre + FVector2f(-0.65f * S, 0.05f * S), Centre + FVector2f(-0.25f * S, 0.05f * S), Color, 0.006f * Unit);
		Line(Centre + FVector2f(0.25f * S, 0.05f * S), Centre + FVector2f(0.65f * S, 0.05f * S), Color, 0.006f * Unit);
		Line(Centre + FVector2f(0.45f * S, -0.15f * S), Centre + FVector2f(0.45f * S, 0.25f * S), Color, 0.006f * Unit);
	}

	void LampOil(const FVector2f& Centre, const FLinearColor& Color)
	{
		const float S = 0.045f * Unit;
		TArray<FVector2f> Can = {
			Centre + FVector2f(-S, -0.1f * S), Centre + FVector2f(-0.3f * S, -0.45f * S), Centre + FVector2f(0.55f * S, -0.45f * S),
			Centre + FVector2f(0.8f * S, -0.15f * S), Centre + FVector2f(S, -0.15f * S), Centre + FVector2f(0.55f * S, 0.15f * S),
			Centre + FVector2f(0.55f * S, 0.55f * S), Centre + FVector2f(-0.6f * S, 0.55f * S), Centre + FVector2f(-0.6f * S, 0.15f * S),
			Centre + FVector2f(-S, -0.1f * S),
		};
		Polyline(Can, Color, 0.007f * Unit);
		Arc(Centre + FVector2f(-1.25f * S, 0.62f * S), 0.16f * S, 0.f, 360.f, Color, 0.007f * Unit);
	}

	void LampCircleText(const FVector2f& Centre, const FString& Content, const FLinearColor& Color)
	{
		const float S = 0.05f * Unit;
		Arc(Centre, S, 0.f, 360.f, Color, 0.007f * Unit);
		Arc(Centre, 1.3f * S, 20.f, 160.f, Color, 0.005f * Unit);
		Arc(Centre, 1.3f * S, 200.f, 340.f, Color, 0.005f * Unit);
		Text(Content, Centre, (Content.Len() > 1 ? 0.03f : 0.046f) * Unit, Color, EAnchor::Centre);
	}

	void LampAirbag(const FVector2f& Centre, const FLinearColor& Color)
	{
		const float S = 0.042f * Unit;
		Disc(Centre + FVector2f(-0.45f * S, -0.5f * S), 0.28f * S, Color);
		TArray<FVector2f> Seat = {
			Centre + FVector2f(-0.2f * S, -0.2f * S), Centre + FVector2f(0.2f * S, 0.6f * S), Centre + FVector2f(-0.4f * S, 0.7f * S),
		};
		Polyline(Seat, Color, 0.008f * Unit);
		Arc(Centre + FVector2f(0.35f * S, 0.0f), 0.7f * S, -60.f, 60.f, Color, 0.006f * Unit);
	}

	void PaintWarningLamps(float CentreX)
	{
		if (!State.bPowered)
		{
			return;
		}
		const float Y = 0.9f * Unit;
		const float Gap = 0.15f * Unit;
		const float Back = State.Backlight;
		if (State.bCheckEngine)
		{
			LampEngine(FVector2f(CentreX - 2.5f * Gap, Y), AmberLampColor * Back);
		}
		if (State.bOilPressure)
		{
			LampOil(FVector2f(CentreX - 1.5f * Gap, Y), RedLampColor * Back);
		}
		if (State.bBattery)
		{
			LampBattery(FVector2f(CentreX - 0.5f * Gap, Y), RedLampColor * Back);
		}
		if (State.bHandbrake)
		{
			LampCircleText(FVector2f(CentreX + 0.5f * Gap, Y), TEXT("P"), RedLampColor * Back);
		}
		if (State.bAbs)
		{
			LampCircleText(FVector2f(CentreX + 1.5f * Gap, Y), TEXT("ABS"), AmberLampColor * Back);
		}
		if (State.bAirbag)
		{
			LampAirbag(FVector2f(CentreX + 2.5f * Gap, Y), RedLampColor * Back);
		}
	}

	FSlateWindowElementList& Elements;
	const FGeometry& Geometry;
	int32 Layer;
	const FClusterState& State;
	FVector2f Size = FVector2f::ZeroVector;
	float Unit = 1.f;
	FSlateBrush DiscBrush;
	FSlateBrush PanelBrush;
};
}

void SInstrumentClusterWidget::Construct(const FArguments& InArgs)
{
}

int32 SInstrumentClusterWidget::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	FClusterPainter Painter(OutDrawElements, AllottedGeometry, LayerId, State);
	Painter.Paint();
	return LayerId + 4000;
}
