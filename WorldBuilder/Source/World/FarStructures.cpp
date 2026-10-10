#include "FarStructures.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include "PlacementFrame.h"

namespace WorldBuilder
{
namespace
{
constexpr double Pi = 3.14159265358979323846;
constexpr double DegreesPerRadian = 180.0 / Pi;
/** Compass bearing the turbines face: Hamburg's prevailing wind comes from the west-south-west. */
constexpr double PrevailingWindBearing = 250.0;
/** Turbines spread around the prevailing direction by up to this much, degrees. */
constexpr double TurbineYawSpread = 12.0;
/** Turbines turn at this share of their rated speed. */
constexpr double TurbineSpeedShare = 0.8;
/** A line bending by more than this at a tower makes it an anchor tower, degrees. */
constexpr double AnchorAngleDegrees = 10.0;
/** Conductor sag on a span of ReferenceSpan metres; it grows with the square of the span. */
constexpr double ReferenceSag = 9.0;
constexpr double ReferenceSpan = 300.0;
/** The earth wire is strung tighter than the conductors. */
constexpr double EarthWireSagShare = 0.8;
/** Spans longer than this are a data error (a tower missing in OSM), metres. */
constexpr double LongestSpan = 700.0;

std::string Lowercase(std::string Text)
{
	std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char Character) { return std::tolower(Character); });
	return Text;
}

std::string TagOrEmpty(const FTags& Tags, const char* Key)
{
	const std::string* Value = FindTag(Tags, Key);
	return Value == nullptr ? std::string() : *Value;
}

/** The world point of a UTM easting and northing. */
FWorldPoint UtmToWorld(double East, double North)
{
	return {East - OriginEast, -(North - OriginNorth)};
}

// ---------------------------------------------------------------------------------------------------------- turbines

/** The kit turbine for an OSM turbine: by maker and model where tagged, else by hub height, else by position. */
std::string TurbineModel(const FOsmPoint& Turbine)
{
	const std::string Text = Lowercase(TagOrEmpty(Turbine.Tags, "manufacturer") + " " + TagOrEmpty(Turbine.Tags, "model")
		+ " " + TagOrEmpty(Turbine.Tags, "generator:model") + " " + TagOrEmpty(Turbine.Tags, "name"));
	if (Text.find("n117") != std::string::npos || Text.find("nordex") != std::string::npos)
	{
		return "N117";
	}
	if (Text.find("e-92") != std::string::npos || Text.find("e92") != std::string::npos || Text.find("enercon") != std::string::npos)
	{
		return "E92";
	}
	if (Text.find("micon") != std::string::npos || Text.find("nm48") != std::string::npos)
	{
		return "NM48";
	}
	if (Text.find("mm100") != std::string::npos || Text.find("senvion") != std::string::npos || Text.find("repower") != std::string::npos)
	{
		return "MM100";
	}
	std::string Height = TagOrEmpty(Turbine.Tags, "height:hub");
	if (Height.empty())
	{
		Height = TagOrEmpty(Turbine.Tags, "height");
	}
	if (!Height.empty())
	{
		const double Metres = std::atof(Height.c_str());
		if (Metres > 0.0 && Metres <= 75.0)
		{
			return "NM48";
		}
		if (Metres >= 102.0)
		{
			return "E92";
		}
		if (Metres >= 95.0)
		{
			return "MM100";
		}
		if (Metres > 0.0)
		{
			return "N117";
		}
	}
	const char* const Common[] = {"MM100", "N117", "E92"};
	return Common[static_cast<int>(PositionNoise(Turbine.Position.X, Turbine.Position.Y, 1) * 3.0) % 3];
}

/** Tower, nacelle and spinning rotor of one turbine. */
void AddTurbine(const FOsmPoint& Turbine, const FKitSpecs& Specs, const FGroundSampler& Ground,
				std::vector<FStructurePlacement>& Out)
{
	const std::string Model = TurbineModel(Turbine);
	const std::string Key = "turbines." + Model;
	const double TowerHeight = Specs.Number(Key + ".tower_height", 0.0);
	const std::vector<double> Socket = Specs.Numbers(Key + ".rotor_socket");
	if (TowerHeight <= 0.0 || Socket.size() != 3)
	{
		return;
	}
	const double X = Turbine.Position.X;
	const double Y = Turbine.Position.Y;
	const float Z = static_cast<float>(Ground(X, Y));
	// The kit's rotor side is local +Y in Unreal (-Y in Blender); yaw turns it to face the wind's bearing.
	const double Yaw = PrevailingWindBearing + 180.0 + (PositionNoise(X, Y, 2) * 2.0 - 1.0) * TurbineYawSpread;
	const double Radians = Yaw / DegreesPerRadian;

	FStructurePlacement Tower;
	Tower.Mesh = "turbines_" + Model + "_Tower";
	Tower.X = X;
	Tower.Y = Y;
	Tower.Z = Z;
	Tower.Yaw = static_cast<float>(Yaw);
	Out.push_back(Tower);

	FStructurePlacement Nacelle = Tower;
	Nacelle.Mesh = "turbines_" + Model + "_Nacelle";
	Nacelle.Z = Z + static_cast<float>(TowerHeight);
	Out.push_back(Nacelle);

	// The hub: Socket is (0, -overhang, axis height) in the kit frame, so the overhang is along local +Y in Unreal.
	const double Overhang = -Socket[1];
	FStructurePlacement Rotor = Tower;
	Rotor.Mesh = "turbines_" + Model + "_Rotor";
	Rotor.Kind = EStructureKind::Rotor;
	Rotor.X = X - std::sin(Radians) * Overhang;
	Rotor.Y = Y + std::cos(Radians) * Overhang;
	Rotor.Z = Z + static_cast<float>(TowerHeight + Socket[2]);
	Rotor.Roll = static_cast<float>(Specs.Number(Key + ".rotor_tilt_degrees", 5.0));
	Rotor.Param0 = static_cast<float>(Specs.Number(Key + ".rated_rpm", 15.0) * TurbineSpeedShare);
	Rotor.Param1 = static_cast<float>(PositionNoise(X, Y, 3) * 120.0);
	Out.push_back(Rotor);
}

// ----------------------------------------------------------------------------------------------------------- pylons

/** One conductor position of a pylon design, in the kit frame (x across, y along the line ahead, z up). */
struct FSocket
{
	std::string Name;
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
};

/** A pylon design's sockets: the conductors by their name without _Back and _Ahead, and the earth wire. */
struct FDesign
{
	std::string Name;
	/** Conductor base name, then the socket facing back and the one facing ahead (the same on suspension towers). */
	std::vector<std::pair<FSocket, FSocket>> Conductors;
	std::optional<FSocket> EarthWire;
};

std::string BaseSocketName(const std::string& Name)
{
	for (const char* Suffix : {"_Back", "_Ahead"})
	{
		const std::string Text(Suffix);
		if (Name.size() > Text.size() && Name.compare(Name.size() - Text.size(), Text.size(), Text) == 0)
		{
			return Name.substr(0, Name.size() - Text.size());
		}
	}
	return Name;
}

/** The sockets of one kit pylon from the spec. */
FDesign ReadDesign(const FKitSpecs& Specs, const std::string& Name)
{
	FDesign Design;
	Design.Name = Name;
	const std::string Key = "pylons." + Name + ".sockets";
	std::map<std::string, std::pair<FSocket, FSocket>> ByBase;
	std::vector<std::string> Order;
	for (int Index = 0; Index < Specs.ListSize(Key); ++Index)
	{
		const std::string Item = Key + "." + std::to_string(Index);
		const std::vector<double> Position = Specs.Numbers(Item + ".position");
		if (Position.size() != 3)
		{
			continue;
		}
		const FSocket Socket{Specs.Text(Item + ".name"), Position[0], Position[1], Position[2]};
		if (Socket.Name == "EarthWire")
		{
			Design.EarthWire = Socket;
			continue;
		}
		const std::string Base = BaseSocketName(Socket.Name);
		if (ByBase.count(Base) == 0)
		{
			ByBase[Base] = {Socket, Socket};
			Order.push_back(Base);
		}
		const bool bAhead = Socket.Name.size() > Base.size() && Socket.Name.substr(Base.size()) == "_Ahead";
		const bool bBack = Socket.Name.size() > Base.size() && Socket.Name.substr(Base.size()) == "_Back";
		if (bAhead)
		{
			ByBase[Base].second = Socket;
		}
		if (bBack)
		{
			ByBase[Base].first = Socket;
		}
	}
	for (const std::string& Base : Order)
	{
		Design.Conductors.push_back(ByBase[Base]);
	}
	return Design;
}

/** A tower on the lines: its position, design family and the directions to its neighbours. */
struct FTower
{
	const FOsmPoint* Point = nullptr;
	/** Donau, OneLevel or Barrel. */
	std::string Family;
	std::vector<size_t> Neighbours;
	bool bAnchor = false;
	/** World direction of the line ahead (unit) and the yaw that turns the kit's ahead axis onto it. */
	double AheadX = 1.0;
	double AheadY = 0.0;
	double Yaw = 0.0;
	float Z = 0.0f;
};

/** The design family a tower's tags name, or empty. */
std::string FamilyFromTags(const FTags& Tags)
{
	const std::string Design = Lowercase(TagOrEmpty(Tags, "design"));
	const std::string Material = Lowercase(TagOrEmpty(Tags, "material"));
	const std::string Structure = Lowercase(TagOrEmpty(Tags, "structure"));
	if (Design.find("barrel") != std::string::npos || Material == "concrete" || Structure == "tubular")
	{
		return "Barrel";
	}
	if (Design.find("donau") != std::string::npos || Design.find("two-level") != std::string::npos)
	{
		return "Donau";
	}
	if (Design.find("one-level") != std::string::npos)
	{
		return "OneLevel";
	}
	return {};
}

/** Towers by node id, and each line as a list of tower indices (nodes that are no tower split the line). */
struct FTowerNetwork
{
	std::vector<FTower> Towers;
	std::vector<std::vector<size_t>> Runs;
};

FTowerNetwork ConnectTowers(const FFarStructureSources& Sources)
{
	FTowerNetwork Network;
	std::unordered_map<int64_t, size_t> TowerOfNode;
	for (const FOsmPoint& Point : Sources.Towers)
	{
		TowerOfNode[Point.Id] = Network.Towers.size();
		FTower& Tower = Network.Towers.emplace_back();
		Tower.Point = &Point;
		Tower.Family = FamilyFromTags(Point.Tags);
	}
	for (const FOsmWay& Line : Sources.PowerLines)
	{
		std::vector<size_t> Run;
		for (const int64_t Node : Line.NodeIds)
		{
			const auto Found = TowerOfNode.find(Node);
			if (Found == TowerOfNode.end())
			{
				if (Run.size() >= 2)
				{
					Network.Runs.push_back(Run);
				}
				Run.clear();
				continue;
			}
			Run.push_back(Found->second);
		}
		if (Run.size() >= 2)
		{
			Network.Runs.push_back(Run);
		}
	}
	return Network;
}

/** Fills each run's untagged towers with the family most of its tagged towers have (Donau when none is tagged). */
void SpreadFamilies(FTowerNetwork& Network)
{
	for (const std::vector<size_t>& Run : Network.Runs)
	{
		std::map<std::string, int> Counts;
		for (const size_t Index : Run)
		{
			if (!Network.Towers[Index].Family.empty())
			{
				++Counts[Network.Towers[Index].Family];
			}
		}
		std::string Common = "Donau";
		int Best = 0;
		for (const auto& [Family, Count] : Counts)
		{
			if (Count > Best)
			{
				Common = Family;
				Best = Count;
			}
		}
		for (const size_t Index : Run)
		{
			if (Network.Towers[Index].Family.empty())
			{
				Network.Towers[Index].Family = Common;
			}
		}
	}
	for (FTower& Tower : Network.Towers)
	{
		if (Tower.Family.empty())
		{
			Tower.Family = "Donau";
		}
	}
}

void AddNeighbour(FTower& Tower, size_t Neighbour)
{
	if (std::find(Tower.Neighbours.begin(), Tower.Neighbours.end(), Neighbour) == Tower.Neighbours.end())
	{
		Tower.Neighbours.push_back(Neighbour);
	}
}

/** The direction of the line through each tower, its yaw, and whether it is an anchor. */
void OrientTowers(FTowerNetwork& Network)
{
	for (const std::vector<size_t>& Run : Network.Runs)
	{
		for (size_t Step = 0; Step + 1 < Run.size(); ++Step)
		{
			AddNeighbour(Network.Towers[Run[Step]], Run[Step + 1]);
			AddNeighbour(Network.Towers[Run[Step + 1]], Run[Step]);
		}
	}
	for (FTower& Tower : Network.Towers)
	{
		const FWorldPoint& At = Tower.Point->Position;
		auto DirectionTo = [&](size_t Other) {
			const FWorldPoint& To = Network.Towers[Other].Point->Position;
			const double Length = std::max(std::hypot(To.X - At.X, To.Y - At.Y), 1e-6);
			return FWorldPoint{(To.X - At.X) / Length, (To.Y - At.Y) / Length};
		};
		if (Tower.Neighbours.empty())
		{
			Tower.bAnchor = true;
		}
		else if (Tower.Neighbours.size() == 1)
		{
			const FWorldPoint Ahead = DirectionTo(Tower.Neighbours[0]);
			Tower.AheadX = Ahead.X;
			Tower.AheadY = Ahead.Y;
			Tower.bAnchor = true;
		}
		else
		{
			// Ahead is the bisector from the first neighbour through the tower to the second.
			const FWorldPoint Back = DirectionTo(Tower.Neighbours[0]);
			const FWorldPoint Ahead = DirectionTo(Tower.Neighbours[1]);
			const double BisectorX = Ahead.X - Back.X;
			const double BisectorY = Ahead.Y - Back.Y;
			const double Length = std::max(std::hypot(BisectorX, BisectorY), 1e-6);
			Tower.AheadX = BisectorX / Length;
			Tower.AheadY = BisectorY / Length;
			const double Deflection = 180.0 - std::acos(std::clamp(Back.X * Ahead.X + Back.Y * Ahead.Y, -1.0, 1.0)) * DegreesPerRadian;
			Tower.bAnchor = Deflection > AnchorAngleDegrees || Tower.Neighbours.size() > 2;
		}
		// The kit's ahead axis (+Y in Blender) is local -Y in Unreal; after a yaw it points to (sin, -cos).
		Tower.Yaw = std::atan2(Tower.AheadX, -Tower.AheadY) * DegreesPerRadian;
	}
}

std::string DesignName(const FTower& Tower)
{
	return Tower.Family + (Tower.bAnchor ? "_Anchor" : "_Suspension");
}

/** Where a kit socket of a tower is in the world (the kit's y flips into Unreal's, then the tower's yaw). */
FStructurePlacement SocketPoint(const FTower& Tower, const FSocket& Socket)
{
	const double Radians = Tower.Yaw / DegreesPerRadian;
	const double LocalX = Socket.X;
	const double LocalY = -Socket.Y;
	FStructurePlacement Point;
	Point.X = Tower.Point->Position.X + LocalX * std::cos(Radians) - LocalY * std::sin(Radians);
	Point.Y = Tower.Point->Position.Y + LocalX * std::sin(Radians) + LocalY * std::cos(Radians);
	Point.Z = Tower.Z + static_cast<float>(Socket.Z);
	return Point;
}

/** The socket of a conductor on a tower toward a neighbour: the ahead or back clamp, by which side it lies on. */
const FSocket& FacingSocket(const FTower& Tower, const std::pair<FSocket, FSocket>& Conductor, const FTower& Toward)
{
	const double DX = Toward.Point->Position.X - Tower.Point->Position.X;
	const double DY = Toward.Point->Position.Y - Tower.Point->Position.Y;
	return DX * Tower.AheadX + DY * Tower.AheadY >= 0.0 ? Conductor.second : Conductor.first;
}

/** One conductor between two world points. */
void AddWire(const FStructurePlacement& From, const FStructurePlacement& To, double SagShare,
			 std::vector<FStructurePlacement>& Out)
{
	const double DX = To.X - From.X;
	const double DY = To.Y - From.Y;
	const double DZ = static_cast<double>(To.Z) - From.Z;
	const double Horizontal = std::hypot(DX, DY);
	const double Length = std::sqrt(Horizontal * Horizontal + DZ * DZ);
	if (Length < 1.0 || Horizontal > LongestSpan)
	{
		return;
	}
	FStructurePlacement Wire = From;
	Wire.Mesh = "pylons_Wire";
	Wire.Kind = EStructureKind::Wire;
	Wire.Yaw = static_cast<float>(std::atan2(DY, DX) * DegreesPerRadian);
	Wire.Pitch = static_cast<float>(std::atan2(DZ, Horizontal) * DegreesPerRadian);
	Wire.ScaleX = static_cast<float>(Length);
	const double Ratio = Horizontal / ReferenceSpan;
	Wire.Param0 = static_cast<float>(std::clamp(ReferenceSag * Ratio * Ratio * SagShare, 0.2, 25.0));
	Out.push_back(Wire);
}

/** The conductors and earth wire of one span, matched by their order on the two designs. */
void AddSpan(const FTower& From, const FDesign& FromDesign, const FTower& To, const FDesign& ToDesign,
			 std::vector<FStructurePlacement>& Out)
{
	const size_t Count = std::min(FromDesign.Conductors.size(), ToDesign.Conductors.size());
	for (size_t Index = 0; Index < Count; ++Index)
	{
		const FSocket& Start = FacingSocket(From, FromDesign.Conductors[Index], To);
		const FSocket& End = FacingSocket(To, ToDesign.Conductors[Index], From);
		AddWire(SocketPoint(From, Start), SocketPoint(To, End), 1.0, Out);
	}
	if (FromDesign.EarthWire && ToDesign.EarthWire)
	{
		AddWire(SocketPoint(From, *FromDesign.EarthWire), SocketPoint(To, *ToDesign.EarthWire), EarthWireSagShare, Out);
	}
}

void AddPylons(const FFarStructureSources& Sources, const FKitSpecs& Specs, const FGroundSampler& Ground,
			   std::vector<FStructurePlacement>& Out)
{
	FTowerNetwork Network = ConnectTowers(Sources);
	SpreadFamilies(Network);
	OrientTowers(Network);
	std::map<std::string, FDesign> Designs;
	for (FTower& Tower : Network.Towers)
	{
		Tower.Z = static_cast<float>(Ground(Tower.Point->Position.X, Tower.Point->Position.Y));
		const std::string Name = DesignName(Tower);
		if (Designs.count(Name) == 0)
		{
			Designs[Name] = ReadDesign(Specs, Name);
		}
		FStructurePlacement Pylon;
		Pylon.Mesh = "pylons_" + Name;
		Pylon.X = Tower.Point->Position.X;
		Pylon.Y = Tower.Point->Position.Y;
		Pylon.Z = Tower.Z;
		Pylon.Yaw = static_cast<float>(Tower.Yaw);
		Out.push_back(Pylon);
	}
	for (const std::vector<size_t>& Run : Network.Runs)
	{
		for (size_t Step = 0; Step + 1 < Run.size(); ++Step)
		{
			const FTower& From = Network.Towers[Run[Step]];
			const FTower& To = Network.Towers[Run[Step + 1]];
			AddSpan(From, Designs[DesignName(From)], To, Designs[DesignName(To)], Out);
		}
	}
}

// -------------------------------------------------------------------------------------------------------- landmarks

void AddLandmarks(const FKitSpecs& Specs, std::vector<FStructurePlacement>& Out)
{
	for (const char* Name : {"Elbphilharmonie"})
	{
		const std::string Key = std::string("landmarks.") + Name;
		const std::vector<double> Origin = Specs.Numbers(Key + ".origin_utm32");
		if (Origin.size() != 2)
		{
			continue;
		}
		const FWorldPoint At = UtmToWorld(Origin[0], Origin[1]);
		FStructurePlacement Landmark;
		Landmark.Mesh = std::string("landmarks_") + Name;
		Landmark.X = At.X;
		Landmark.Y = At.Y;
		Landmark.Z = static_cast<float>(Specs.Number(Key + ".origin_sea_level", 0.0));
		Out.push_back(Landmark);
	}
}
}

std::vector<FStructurePlacement> BuildFarStructures(const FFarStructureSources& Sources, const FKitSpecs& Specs,
												   const FGroundSampler& Ground)
{
	std::vector<FStructurePlacement> Placements;
	for (const FOsmPoint& Turbine : Sources.WindTurbines)
	{
		AddTurbine(Turbine, Specs, Ground, Placements);
	}
	AddPylons(Sources, Specs, Ground, Placements);
	AddLandmarks(Specs, Placements);
	return Placements;
}

FPolygons LandmarkFootprints(const FKitSpecs& Specs)
{
	FPolygons Footprints;
	for (const char* Name : {"Elbphilharmonie"})
	{
		const std::string Key = std::string("landmarks.") + Name + ".footprint_utm32";
		Clipper2Lib::PathD Ring;
		for (int Index = 0; Index < Specs.ListSize(Key); ++Index)
		{
			const std::vector<double> Corner = Specs.Numbers(Key + "." + std::to_string(Index));
			if (Corner.size() == 2)
			{
				const FWorldPoint Point = UtmToWorld(Corner[0], Corner[1]);
				Ring.emplace_back(Point.X, Point.Y);
			}
		}
		if (Ring.size() >= 3)
		{
			Footprints.push_back(Ring);
		}
	}
	return Footprints;
}
}
