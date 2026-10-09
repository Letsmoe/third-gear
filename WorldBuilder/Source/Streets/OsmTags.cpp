#include "OsmTags.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "Assumptions.h"

namespace WorldBuilder::OsmTags
{
namespace
{
const std::unordered_set<std::string> OnewayValues = {"yes", "1", "true", "-1"};
const std::unordered_set<std::string> RoundaboutValues = {"roundabout", "circular"};
const std::unordered_set<std::string> AlwaysOnewayClasses = {"motorway", "motorway_link"};
const std::unordered_set<std::string> CycleLaneValues = {"lane", "opposite_lane"};
const std::unordered_set<std::string> BusLaneValues = {"lane", "opposite_lane"};
// Speed limits written as a zone instead of a number (maxspeed=DE:urban and the like), in km/h.
const std::vector<std::pair<std::string, double>> ZoneSpeedLimits = {
	{"urban", 50.0}, {"rural", 100.0}, {"zone30", 30.0}, {"zone:30", 30.0},
	{"living_street", 7.0}, {"motorway", 130.0}, {"bicycle_road", 30.0}};
constexpr double MilesToKilometres = 1.609344;

/** The text without leading and trailing white space. */
std::string Stripped(const std::string& Text)
{
	size_t Begin = 0;
	size_t End = Text.size();
	while (Begin < End && std::isspace(static_cast<unsigned char>(Text[Begin])))
	{
		++Begin;
	}
	while (End > Begin && std::isspace(static_cast<unsigned char>(Text[End - 1])))
	{
		--End;
	}
	return Text.substr(Begin, End - Begin);
}

bool EndsWith(const std::string& Text, const std::string& Suffix)
{
	return Text.size() >= Suffix.size() && Text.compare(Text.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
}

std::string Lowered(std::string Text)
{
	for (char& Character : Text)
	{
		Character = static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
	}
	return Text;
}

/** How many lanes a lane list such as turn:lanes="left|through|right" lists, or nothing when it is not tagged. */
std::optional<int> LaneListCount(const FTags& Tags, const char* Key)
{
	const std::string Value = ValueOrEmpty(Tags, Key);
	if (Value.empty())
	{
		return std::nullopt;
	}
	return static_cast<int>(SplitText(Value, '|').size());
}

/** The per-lane values of a lane list such as bicycle:lanes=no|no|designated, or none when it is not tagged. */
std::vector<std::string> LaneValues(const FTags& Tags, const std::string& Key, const std::string& Suffix)
{
	const std::string Value = ValueOrEmpty(Tags, (Key + ":lanes" + Suffix).c_str());
	if (Value.empty())
	{
		return {};
	}
	return SplitText(Value, '|');
}

/** The value at an index of a lane list, or "" past its end. */
std::string ValueAt(const std::vector<std::string>& Values, size_t Index)
{
	if (Index < Values.size())
	{
		return Values[Index];
	}
	return "";
}

/** How many lanes of one lane list (suffix "", ":forward" or ":backward") are for bicycles or buses only. */
int NonMotorLanes(const FTags& Tags, const std::string& Suffix)
{
	const std::vector<std::string> Bicycle = LaneValues(Tags, "bicycle", Suffix);
	std::vector<std::string> Vehicle = LaneValues(Tags, "vehicle", Suffix);
	if (Vehicle.empty())
	{
		Vehicle = LaneValues(Tags, "motor_vehicle", Suffix);
	}
	std::vector<std::string> Bus = LaneValues(Tags, "bus", Suffix);
	if (Bus.empty())
	{
		Bus = LaneValues(Tags, "psv", Suffix);
	}
	const size_t Count = std::max({Bicycle.size(), Vehicle.size(), Bus.size()});
	int Total = 0;
	for (size_t Index = 0; Index < Count; ++Index)
	{
		const bool bCycleOnly = ValueAt(Bicycle, Index) == "designated" && ValueAt(Vehicle, Index) == "no";
		const bool bBusOnly = ValueAt(Bus, Index) == "designated";
		if (bCycleOnly || bBusOnly)
		{
			++Total;
		}
	}
	return Total;
}
}

std::string ValueOrEmpty(const FTags& Tags, const char* Key)
{
	const std::string* Value = FindTag(Tags, Key);
	if (Value == nullptr)
	{
		return "";
	}
	return *Value;
}

bool HasTag(const FTags& Tags, const char* Key)
{
	return FindTag(Tags, Key) != nullptr;
}

bool TagEquals(const FTags& Tags, const char* Key, const char* Value)
{
	const std::string* Found = FindTag(Tags, Key);
	return Found != nullptr && *Found == Value;
}

std::vector<std::string> SplitText(const std::string& Text, char Separator)
{
	std::vector<std::string> Parts;
	size_t Begin = 0;
	while (true)
	{
		const size_t End = Text.find(Separator, Begin);
		if (End == std::string::npos)
		{
			Parts.push_back(Text.substr(Begin));
			return Parts;
		}
		Parts.push_back(Text.substr(Begin, End - Begin));
		Begin = End + 1;
	}
}

std::optional<double> Number(const std::string* Value)
{
	if (Value == nullptr)
	{
		return std::nullopt;
	}
	std::string Text = SplitText(*Value, ';')[0];
	std::replace(Text.begin(), Text.end(), ',', '.');
	Text.erase(std::remove(Text.begin(), Text.end(), 'm'), Text.end());
	Text = Stripped(Text);
	if (Text.empty() || Text.find('x') != std::string::npos || Text.find('X') != std::string::npos)
	{
		return std::nullopt;
	}
	char* ParsedEnd = nullptr;
	const double Parsed = std::strtod(Text.c_str(), &ParsedEnd);
	if (*ParsedEnd != '\0')
	{
		return std::nullopt;
	}
	return Parsed;
}

double Number(const std::string* Value, double Default)
{
	return Number(Value).value_or(Default);
}

std::string Highway(const FTags& Tags)
{
	return ValueOrEmpty(Tags, "highway");
}

std::string BaseClass(const FTags& Tags)
{
	std::string RoadClass = Highway(Tags);
	if (EndsWith(RoadClass, "_link"))
	{
		RoadClass.resize(RoadClass.size() - 5);
	}
	return RoadClass;
}

bool IsLink(const FTags& Tags)
{
	return EndsWith(Highway(Tags), "_link");
}

bool IsOneway(const FTags& Tags)
{
	if (OnewayValues.count(ValueOrEmpty(Tags, "oneway")) > 0 && HasTag(Tags, "oneway"))
	{
		return true;
	}
	if (RoundaboutValues.count(ValueOrEmpty(Tags, "junction")) > 0 && HasTag(Tags, "junction"))
	{
		return true;
	}
	return AlwaysOnewayClasses.count(Highway(Tags)) > 0;
}

bool IsReversedOneway(const FTags& Tags)
{
	return TagEquals(Tags, "oneway", "-1");
}

std::optional<int> TurnLaneCount(const FTags& Tags)
{
	if (IsOneway(Tags))
	{
		return LaneListCount(Tags, "turn:lanes");
	}
	const std::optional<int> Forward = LaneListCount(Tags, "turn:lanes:forward");
	const std::optional<int> Backward = LaneListCount(Tags, "turn:lanes:backward");
	if (!Forward.has_value() || !Backward.has_value())
	{
		return std::nullopt;
	}
	return *Forward + *Backward;
}

std::optional<int> TaggedLaneCount(const FTags& Tags)
{
	const std::optional<int> FromTurnLanes = TurnLaneCount(Tags);
	if (FromTurnLanes.has_value() && *FromTurnLanes != 0)
	{
		return FromTurnLanes;
	}
	const std::optional<double> Lanes = Number(FindTag(Tags, "lanes"));
	if (!Lanes.has_value() || *Lanes == 0.0 || *Lanes < 1.0)
	{
		return std::nullopt;
	}
	return static_cast<int>(*Lanes);
}

int NonMotorLanesCounted(const FTags& Tags)
{
	if (IsOneway(Tags))
	{
		return NonMotorLanes(Tags, "");
	}
	return NonMotorLanes(Tags, ":forward") + NonMotorLanes(Tags, ":backward");
}

std::optional<std::pair<int, int>> TaggedLanesByDirection(const FTags& Tags)
{
	const std::optional<double> Forward = Number(FindTag(Tags, "lanes:forward"));
	const std::optional<double> Backward = Number(FindTag(Tags, "lanes:backward"));
	if (!Forward.has_value() || !Backward.has_value())
	{
		return std::nullopt;
	}
	return std::make_pair(static_cast<int>(*Forward), static_cast<int>(*Backward));
}

bool IsBusRoad(const FTags& Tags)
{
	const bool bBusesAllowed = TagEquals(Tags, "bus", "yes") || TagEquals(Tags, "bus", "designated")
		|| TagEquals(Tags, "psv", "yes") || TagEquals(Tags, "psv", "designated");
	const bool bOthersBanned = TagEquals(Tags, "vehicle", "no") || TagEquals(Tags, "motor_vehicle", "no")
		|| TagEquals(Tags, "access", "no");
	return bBusesAllowed && bOthersBanned;
}

bool IsZone30(const FTags& Tags)
{
	for (const char* Key : {"maxspeed:type", "source:maxspeed", "zone:maxspeed", "zone:traffic"})
	{
		const std::string Value = Lowered(ValueOrEmpty(Tags, Key));
		if (Value.find("zone30") != std::string::npos || EndsWith(Value, ":30")
			|| Value.find("zone:30") != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

std::optional<bool> LaneMarkings(const FTags& Tags)
{
	if (TagEquals(Tags, "lane_markings", "yes"))
	{
		return true;
	}
	if (TagEquals(Tags, "lane_markings", "no"))
	{
		return false;
	}
	return std::nullopt;
}

std::optional<double> SpeedLimit(const FTags& Tags)
{
	const std::string Value = Stripped(ValueOrEmpty(Tags, "maxspeed"));
	if (Value.empty())
	{
		return std::nullopt;
	}
	const size_t LastColon = Value.rfind(':');
	const std::string Zone = LastColon == std::string::npos ? Value : Value.substr(LastColon + 1);  // DE:urban, DE:zone30
	for (const auto& [ZoneName, Limit] : ZoneSpeedLimits)
	{
		if (Zone == ZoneName)
		{
			return Limit;
		}
	}
	if (EndsWith(Value, "mph"))
	{
		const std::string Miles = Value.substr(0, Value.size() - 3);
		const std::optional<double> Parsed = Number(&Miles);
		if (!Parsed.has_value())
		{
			return std::nullopt;
		}
		return *Parsed * MilesToKilometres;
	}
	return Number(&Value);
}

FSideValues SideValues(const FTags& Tags, const std::string& Key)
{
	FSideValues Sides;
	const std::string* KeyValue = FindTag(Tags, Key.c_str());
	const std::string* BothValue = FindTag(Tags, (Key + ":both").c_str());
	const std::string* Both = nullptr;
	if (BothValue != nullptr && !BothValue->empty())
	{
		Both = BothValue;
	}
	else if (KeyValue != nullptr && !KeyValue->empty())
	{
		Both = KeyValue;
	}
	if (Both != nullptr)
	{
		Sides.Left = *Both;
		Sides.Right = *Both;
		const bool bKeyAlone = KeyValue != nullptr && !KeyValue->empty() && BothValue == nullptr;
		if (bKeyAlone && IsOneway(Tags))
		{
			Sides.Left.reset();
		}
	}
	const std::string* Left = FindTag(Tags, (Key + ":left").c_str());
	if (Left != nullptr && !Left->empty())
	{
		Sides.Left = *Left;
	}
	const std::string* Right = FindTag(Tags, (Key + ":right").c_str());
	if (Right != nullptr && !Right->empty())
	{
		Sides.Right = *Right;
	}
	return Sides;
}

FSideValues CyclewaySides(const FTags& Tags)
{
	return SideValues(Tags, "cycleway");
}

bool IsCycleLaneValue(const std::string& Value)
{
	return CycleLaneValues.count(Value) > 0;
}

bool CycleLaneIsAdvisory(const FTags& Tags, bool bRightSide)
{
	const std::string SideKey = std::string("cycleway:") + (bRightSide ? "right" : "left") + ":lane";
	for (const std::string& Key : {SideKey, std::string("cycleway:both:lane"), std::string("cycleway:lane")})
	{
		const std::string* Value = FindTag(Tags, Key.c_str());
		if (Value != nullptr)
		{
			return *Value == "advisory";
		}
	}
	return false;
}

bool HasBusLane(const FTags& Tags, bool bRightSide)
{
	const FSideValues Sides = SideValues(Tags, "busway");
	const std::optional<std::string>& Value = Sides.OfSide(bRightSide);
	return Value.has_value() && BusLaneValues.count(*Value) > 0;
}

std::vector<std::string> TurnLanes(const FTags& Tags, ETravel Travel)
{
	std::string Value;
	if (IsOneway(Tags))
	{
		const bool bForwardIsTravel = !IsReversedOneway(Tags);
		if ((Travel == ETravel::Forward) != bForwardIsTravel)
		{
			return {};
		}
		Value = ValueOrEmpty(Tags, "turn:lanes");
	}
	else if (Travel == ETravel::Forward)
	{
		Value = ValueOrEmpty(Tags, "turn:lanes:forward");
	}
	else
	{
		Value = ValueOrEmpty(Tags, "turn:lanes:backward");
	}
	if (Value.empty())
	{
		return {};
	}
	return SplitText(Value, '|');
}
}
