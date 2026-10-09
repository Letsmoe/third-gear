#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

/**
 * The numbers the street model assumes where OSM says nothing, in metres unless named otherwise (Python:
 * streets/assumptions.py).
 *
 * Each table follows German design practice and is tuned against Hamburg's street survey: the median measured kerb to
 * kerb width of each kind of road outside bergedorf_core, which is kept for checking (compare_survey.py). Urban means
 * buildings line the road (roads.road_context); rural roads have none within reach or a limit of 70 km/h and more.
 */
namespace WorldBuilder::Assumptions
{
/** A value for urban surroundings and one for rural ones. */
struct FUrbanRural
{
	double Urban = 0.0;
	double Rural = 0.0;
};

/** A carriageway width for two-way and for one-way traffic. */
struct FTwoWayOneWay
{
	double TwoWay = 0.0;
	double OneWay = 0.0;
};

/** The length of a dash and of the gap after it. */
struct FDashGap
{
	double Dash = 0.0;
	double Gap = 0.0;
};

using FClassSet = std::unordered_set<std::string>;

// Width of one marked travel lane, by road class: (urban, rural). Links (slip roads) use their base class.
inline const std::unordered_map<std::string, FUrbanRural> MarkedLaneWidth = {
	{"motorway", {3.75, 3.75}},
	{"trunk", {3.5, 3.5}},
	{"primary", {3.25, 3.5}},
	{"secondary", {3.35, 3.35}},
	{"tertiary", {2.85, 3.25}},
	{"unclassified", {2.8, 3.0}},
	{"residential", {2.8, 3.0}},
};
inline constexpr FUrbanRural MarkedLaneWidthOther = {2.75, 3.0};

// A single-lane slip road (highway=*_link) is paved wide enough to pass a broken-down car and for lorries to make
// the curve: its one travel lane gets this width (survey: 5 to 7.5 m kerb to kerb).
inline constexpr double SingleLaneLinkWidth = 5.5;

// Width of a single-lane one-way carriageway without painted lanes, by road class: room for one lane and a parked
// car or a cyclist beside it.
inline const std::unordered_map<std::string, double> OneWayWidth = {
	{"secondary", 4.0},
	{"tertiary", 4.0},
	{"unclassified", 4.25},
	{"residential", 4.4},
	{"living_street", 3.5},
	{"service", 3.0},
};
inline constexpr double OneWayWidthOther = 3.5;

// Width of a two-way carriageway without painted lanes (RASt's "Begegnungsfall": two cars or a car and a lorry
// pass each other on one surface), by road class: (urban, rural).
inline const std::unordered_map<std::string, FUrbanRural> UnmarkedTwoWayWidth = {
	{"tertiary", {4.9, 5.0}},
	{"unclassified", {4.0, 3.0}},
	{"residential", {5.1, 5.0}},
	{"living_street", {3.4, 3.4}},
	{"service", {2.7, 3.0}},
};
inline constexpr FUrbanRural UnmarkedTwoWayWidthOther = {5.0, 5.0};
// A residential street signed for 50 km/h or more is a collector street (RASt "Sammelstraße") and wider.
inline constexpr double CollectorStreetWidth = 6.3;
inline constexpr double CollectorStreetSpeed = 50.0;

// An unmarked two-way carriageway narrower than this is one shared lane: oncoming cars slow down and pass each other
// half on the verge, as on country lanes and narrow residential streets.
inline constexpr double SharedLaneMaxWidth = 4.5;

// Service roads only buses may use (bus stations, bus gates) have lanes as wide as bus lanes.
inline constexpr double BusRoadLaneWidth = 3.25;

// Service roads by their service= tag: (two-way, one-way) carriageway width.
inline const std::unordered_map<std::string, FTwoWayOneWay> ServiceWidth = {
	{"parking_aisle", {5.4, 3.0}},
	{"driveway", {2.7, 2.7}},
	{"alley", {2.7, 2.7}},
};

/**
 * Importance of the road classes, lowest first: the higher class wins the junction surface and goes straight through.
 * Classes that are not listed rank 0.
 */
int RoadClassRank(const std::string& RoadClass);

// Classes whose lanes are painted when OSM does not say (no lanes= and no lane_markings=). Below tertiary, roads
// are unmarked; tertiary roads mapped without lanes= in Hamburg are mostly unmarked too.
inline const FClassSet MarkedByDefault = {"motorway", "trunk", "primary", "secondary"};
inline constexpr int DefaultLanesOneWay = 1;
inline constexpr int DefaultLanesTwoWay = 2;

// Most lanes a road of each class plausibly has in one direction; more is a tagging error such as lanes=12 on a
// two-lane approach. A two-way road may have twice as many.
inline const std::unordered_map<std::string, int> MaxLanesPerDirection = {
	{"motorway", 4}, {"trunk", 4}, {"primary", 4}, {"secondary", 3}, {"tertiary", 3}};
inline constexpr int MaxLanesPerDirectionOther = 2;

// Strips beside the travel lanes, inside the kerbs.
inline constexpr double CycleLaneWidth = 1.85;          // exclusive cycle lane (Radfahrstreifen) including its 0.25 m solid line
inline constexpr double AdvisoryCycleLaneWidth = 1.5;   // advisory cycle lane (Schutzstreifen) including its dashed line
inline constexpr double BusLaneWidth = 3.25;
// Gutter (Rinne) at the kerb on urban roads, paved shoulder strip (Randstreifen) on rural ones: (urban, rural).
inline constexpr FUrbanRural MarginWidth = {0.3, 0.3};

// A width= tag is taken as the kerb to kerb width where mappers measured it that way. On the classified roads of the
// survey area it is 1.2 to 2.5 m short of the kerbs (lanes only), so there it can only widen the assumption.
inline const FClassSet WidthTagTrustedClasses = {"residential", "unclassified", "living_street", "service", "road",
												 "track"};
inline constexpr double WidthTagRangeLow = 2.0;
inline constexpr double WidthTagRangeHigh = 30.0;
// Even where the tag is trusted, one this much narrower than the assumption most likely measures a single lane
// (common on one-way streets) and is ignored.
inline constexpr double WidthTagLaneOnlyGap = 1.0;
// No travel lane gets narrower than this when the strips are fitted to a width= tag.
inline constexpr double MinTravelLaneWidth = 2.0;

// A road piece shorter than this that is narrower than the road on both sides of it is a tagging mistake (a real
// narrowing needs two tapers of up to 30 m and some road between them): it takes the narrower neighbour's
// cross-section.
inline constexpr double ShortNarrowingMaxLength = 80.0;
// Narrower means by at least this much.
inline constexpr double NarrowingMinDifference = 0.3;

// ---- Lines (sources: <data root>/downloads/road_rules/NOTES.md) ----

// Classes whose lane, centre and edge lines are painted at all.
inline const FClassSet PaintedClasses = {"motorway", "motorway_link", "trunk", "trunk_link", "primary",
										 "primary_link", "secondary", "secondary_link", "tertiary", "tertiary_link"};
// Edge lines on these classes, and on rural roads from RuralEdgeLineMinWidth.
inline const FClassSet EdgeLineClasses = {"motorway", "trunk", "primary", "motorway_link", "trunk_link"};
inline constexpr double RuralEdgeLineMinWidth = 5.5;
// No centre line on carriageways narrower than this, nor in Tempo 30 zones (VwV-StVO zu Zeichen 340, StVO §45 1c).
inline constexpr double CentreLineMinWidth = 5.5;
// Lanes narrower than this are not marked (RASt 06 corrections p. 126).
inline constexpr double MinMarkedLaneWidth = 2.75;

// Where a road widens, narrows or gains or loses a lane, its lines move sideways over a taper ("Verziehung") of
// speed x shift / 3 metres (RASt 06 6.1.4.3: lz = V * i / 3); inside towns 10 to 20 m usually suffice and the taper
// is at most 30 m. Lines of a lane that only one road has start or end where the taper does.
inline constexpr double TaperSpeedFactor = 1.0 / 3.0;
inline constexpr double TaperMinLength = 10.0;
inline constexpr double UrbanTaperMaxLength = 30.0;
// A taper takes at most this share of its road segment, so the two ends of a short segment don't overlap.
inline constexpr double TaperMaxSegmentShare = 0.45;

// Speed for the tapers where maxspeed is not tagged: the German defaults inside and outside towns.
inline constexpr double DefaultSpeedUrban = 50.0;
inline constexpr double DefaultSpeedRural = 100.0;

// Between the directions, roads with two or more lanes in one direction get a double solid line (Fahrstreifen-
// begrenzung, Zeichen 295, VwV-StVO): two narrow lines this far apart, centre to centre.
inline constexpr double DoubleLineSpacing = 0.24;
// A lane that turns where its neighbour doesn't is marked off with a broad line (0.25 m): broken along the road, solid
// over the queueing length before the junction (RASt 06 Tabelle 45: 20 m as a rule). The broken line's dashes are not
// in the free sources.
inline constexpr double TurnLaneQueueLength = 20.0;
inline constexpr FDashGap TurnLaneDash = {3.0, 3.0};

// Cycle lane lines (VwV-StVO zu Zeichen 340, Bayern Musterblatt 1): an exclusive lane has a solid broad line (0.25 m),
// an advisory one a narrow line of 1 m dashes and 1 m gaps; across junctions both become a cycle crossing ("Furt") of
// two broad lines with 0.5 m dashes and 0.2 m gaps.
inline constexpr FDashGap AdvisoryCycleLineDash = {1.0, 1.0};
inline constexpr FDashGap CycleFurtDash = {0.5, 0.2};

// Kerb radius at junction corners: lines stop where the corner's curve begins.
inline constexpr double CornerRadius = 4.0;
// Left-turn guide lines inside a junction: dash and gap 1:1 (RMS Teil 1 3.2.2.3); the dash length is not in the free
// sources.
inline constexpr FDashGap GuideLineDash = {1.5, 1.5};
// Lane lines stop at a junction; a cross of two bars this long marks where the lines of crossing roads would meet
// (the size is not in the free sources).
inline constexpr double JunctionCrossLength = 1.0;
// The edge line of the through road continues across the mouth of a side road as a broken broad line.
inline constexpr FDashGap EdgeGuideDash = {1.5, 1.5};
// Where a road joins or leaves another at a shallow angle (merging and diverging lanes), the other road's edge line on
// that side stays broken until the joining carriageway is MergeGap away from it.
inline constexpr double MergeMaxAngle = 45.0;
inline constexpr double MergeGap = 2.0;
// A two-way road and two one-way carriageways meeting at a node, each within this angle of straight on, are a dual
// carriageway split (splits.py).
inline constexpr double SplitMaxDeflection = 45.0;
// A short dual carriageway section that ends at a junction (not another split) opens from the junction's node over
// this length; the junction's mouth hides it.
inline constexpr double DualSectionJunctionRamp = 6.0;
// Hatched areas (Sperrfläche, Zeichen 298) in the gores of splits: stripes at this angle to the road, this wide and
// this far apart (the dimensions are not in the free sources).
inline constexpr double HatchAngleDegrees = 45.0;
inline constexpr double HatchStripeWidth = 0.3;
inline constexpr double HatchSpacing = 1.5;
// Two arms are one road going straight through a junction when their directions differ by at most this much.
inline constexpr double ThroughMaxDeflectionDegrees = 35.0;
// Side arms of these classes (driveways, parking aisles) don't interrupt the lines of the road they join.
inline const FClassSet MinorArmClasses = {"service", "track"};
// Left turns get guide lines around the corner at signalised junctions (a signal within this distance) when the
// approach has its own left-turn lanes (turn:lanes).
inline constexpr double SignalJunctionRadius = 40.0;
// Spacing of the points of generated lines.
inline constexpr double LinePointSpacing = 1.0;
}
