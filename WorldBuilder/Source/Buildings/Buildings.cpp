#include "ExactFloatingPoint.h"
#include "Buildings.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <set>

namespace WorldBuilder
{
namespace
{
struct FTypeDefault
{
	double Levels;
	const char* Roof;
};

/** type -> (levels, roof) defaults; roof "gabled" only applies to roughly rectangular footprints. */
const std::map<std::string, FTypeDefault, std::less<>>& TypeDefaults()
{
	static const std::map<std::string, FTypeDefault, std::less<>> Defaults = {
		{"house", {2, "gabled"}}, {"detached", {2, "gabled"}}, {"semidetached_house", {2, "gabled"}},
		{"terrace", {2, "gabled"}}, {"bungalow", {1, "gabled"}}, {"farm", {2, "gabled"}},
		{"farm_auxiliary", {1, "gabled"}}, {"barn", {1, "gabled"}}, {"allotment_house", {1, "gabled"}},
		{"cabin", {1, "gabled"}}, {"hut", {1, "gabled"}},
		{"apartments", {4, "flat"}}, {"residential", {3, "flat"}}, {"dormitory", {4, "flat"}},
		{"garage", {1, "flat"}}, {"garages", {1, "flat"}}, {"carport", {1, "flat"}}, {"shed", {1, "flat"}},
		{"roof", {1, "flat"}}, {"greenhouse", {1, "flat"}},
		{"commercial", {3, "flat"}}, {"retail", {1, "flat"}}, {"office", {4, "flat"}}, {"supermarket", {1, "flat"}},
		{"industrial", {2, "flat"}}, {"warehouse", {2, "flat"}}, {"manufacture", {2, "flat"}},
		{"school", {3, "flat"}}, {"university", {4, "flat"}}, {"hospital", {5, "flat"}}, {"public", {3, "flat"}},
		{"civic", {3, "flat"}}, {"church", {4, "gabled"}}, {"kindergarten", {1, "flat"}},
		{"train_station", {2, "flat"}},
	};
	return Defaults;
}

/** True for the small building types that have a fixed low eave. */
bool IsLowType(const std::string& BuildingType)
{
	static const std::set<std::string> LowTypes = {"garage", "garages", "carport", "shed", "roof", "greenhouse"};
	return LowTypes.count(BuildingType) > 0;
}

/** True for the shop and industrial types that are at least 5 m to the eave. */
bool IsIndustrialShed(const std::string& BuildingType)
{
	static const std::set<std::string> Types = {"retail", "supermarket", "industrial", "warehouse", "manufacture"};
	return Types.count(BuildingType) > 0;
}

/** The CRC-32 (the zlib one) of the bytes. */
uint32_t Crc32(std::string_view Bytes)
{
	static const std::array<uint32_t, 256> Table = [] {
		std::array<uint32_t, 256> Result{};
		for (uint32_t Index = 0; Index < 256; ++Index)
		{
			uint32_t Value = Index;
			for (int Bit = 0; Bit < 8; ++Bit)
			{
				Value = (Value & 1) ? (0xEDB88320u ^ (Value >> 1)) : (Value >> 1);
			}
			Result[Index] = Value;
		}
		return Result;
	}();
	uint32_t Crc = 0xFFFFFFFFu;
	for (char Character : Bytes)
	{
		Crc = Table[(Crc ^ static_cast<uint8_t>(Character)) & 0xFF] ^ (Crc >> 8);
	}
	return Crc ^ 0xFFFFFFFFu;
}

/** The text without leading and trailing white space. */
std::string Strip(const std::string& Text)
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

/** True for the OSM roof shapes that have a ridge. */
bool IsGabledShape(const std::string& Shape)
{
	static const std::set<std::string> Shapes = {"gabled", "hipped", "half-hipped", "gambrel", "mansard", "saltbox"};
	return Shapes.count(Shape) > 0;
}
}

std::optional<double> ParseTagNumber(const std::string* Value)
{
	if (Value == nullptr)
	{
		return std::nullopt;
	}
	std::string Text = Value->substr(0, Value->find(';'));
	std::replace(Text.begin(), Text.end(), ',', '.');
	Text.erase(std::remove(Text.begin(), Text.end(), 'm'), Text.end());
	Text = Strip(Text);
	if (Text.empty())
	{
		return std::nullopt;
	}
	char* End = nullptr;
	const double Number = std::strtod(Text.c_str(), &End);
	if (End != Text.c_str() + Text.size())
	{
		return std::nullopt;
	}
	return Number;
}

bool IsTruthy(const std::optional<double>& Value)
{
	return Value.has_value() && *Value != 0.0;
}

double Hash01(int64_t OsmId, int Salt)
{
	const std::string Key = std::to_string(OsmId) + ":" + std::to_string(Salt);
	return static_cast<double>(Crc32(Key) & 0xFFFF) / 65535.0;
}

FBuildingParams BuildingParams(const FTags& Tags, double FootprintArea)
{
	FBuildingParams Params;
	const std::string* BuildingTag = FindTag(Tags, "building");
	Params.BuildingType = BuildingTag != nullptr ? *BuildingTag : "yes";
	double Levels = 0.0;
	std::string Roof;
	const auto Known = TypeDefaults().find(Params.BuildingType);
	if (Known != TypeDefaults().end())
	{
		Levels = Known->second.Levels;
		Roof = Known->second.Roof;
	}
	else if (FootprintArea < 30.0) // building=yes or unknown: guess from size
	{
		Levels = 1;
		Roof = "flat";
		Params.BuildingType = "shed";
	}
	else if (FootprintArea < 220.0)
	{
		Levels = 2;
		Roof = "gabled";
	}
	else if (FootprintArea < 1500.0)
	{
		Levels = 3;
		Roof = "flat";
	}
	else
	{
		Levels = 2;
		Roof = "flat";
	}
	const std::optional<double> TaggedLevels = ParseTagNumber(FindTag(Tags, "building:levels"));
	if (IsTruthy(TaggedLevels))
	{
		Levels = std::max(1.0, *TaggedLevels);
	}
	const std::string* Shape = FindTag(Tags, "roof:shape");
	if (Shape != nullptr && IsGabledShape(*Shape))
	{
		Roof = "gabled";
	}
	else if (Shape != nullptr && (*Shape == "flat" || *Shape == "skillion"))
	{
		Roof = "flat";
	}
	double Eave = Levels * LevelHeight;
	if (IsLowType(Params.BuildingType))
	{
		Eave = Params.BuildingType != "greenhouse" ? 2.7 : 3.2;
	}
	else if (IsIndustrialShed(Params.BuildingType))
	{
		Eave = std::max(Eave, Levels <= 1 ? 5.0 : Levels * 4.0);
	}
	const std::optional<double> Height = ParseTagNumber(FindTag(Tags, "height"));
	if (IsTruthy(Height) && 2.0 <= *Height && *Height <= 150.0)
	{
		Eave = Roof == "flat" ? *Height : *Height * 0.7;
	}
	Params.EaveHeight = Eave;
	Params.Roof = Roof;
	return Params;
}

void FacadeStyle(int64_t OsmId, const std::string& BuildingType, std::string& Facade, std::string& RoofSection)
{
	const double Draw = Hash01(OsmId);
	if (BuildingType == "greenhouse")
	{
		Facade = "Facade_Glass";
		RoofSection = "Roof_Glass";
		return;
	}
	if (BuildingType == "industrial" || BuildingType == "warehouse" || BuildingType == "manufacture")
	{
		Facade = Draw < 0.6 ? "Facade_Metal" : "Facade_Concrete";
		RoofSection = "Roof_Flat";
		return;
	}
	static const std::set<std::string> Offices = {"retail", "supermarket", "commercial", "office", "school", "hospital",
		"university", "public"};
	if (Offices.count(BuildingType) > 0)
	{
		Facade = Draw < 0.5 ? "Facade_Plaster" : (Draw < 0.8 ? "Facade_Brick" : "Facade_Concrete");
		RoofSection = "Roof_Flat";
		return;
	}
	// Hamburg housing: lots of red/brown Klinker
	Facade = Draw < 0.6 ? "Facade_Brick" : "Facade_Plaster";
	RoofSection = "Roof_Tiles";
}
}
