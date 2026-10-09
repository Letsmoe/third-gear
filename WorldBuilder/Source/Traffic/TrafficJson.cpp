#include "TrafficJson.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace WorldBuilder
{
std::string PythonDouble(double Value)
{
	if (Value == 0.0)
	{
		return std::signbit(Value) ? "-0.0" : "0.0";
	}
	char Buffer[64];
	const auto Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), std::abs(Value), std::chars_format::scientific);
	const std::string Scientific(Buffer, Result.ptr);
	const size_t ExponentMark = Scientific.find('e');
	std::string Digits = Scientific.substr(0, ExponentMark);
	const int Exponent = std::stoi(Scientific.substr(ExponentMark + 1));
	Digits.erase(std::remove(Digits.begin(), Digits.end(), '.'), Digits.end());
	std::string Text;
	if (-4 <= Exponent && Exponent < 16)
	{
		if (Exponent < 0)
		{
			Text = "0." + std::string(-Exponent - 1, '0') + Digits;
		}
		else if (static_cast<int>(Digits.size()) <= Exponent + 1)
		{
			Text = Digits + std::string(Exponent + 1 - Digits.size(), '0') + ".0";
		}
		else
		{
			Text = Digits.substr(0, Exponent + 1) + "." + Digits.substr(Exponent + 1);
		}
	}
	else
	{
		char ExponentText[16];
		std::snprintf(ExponentText, sizeof(ExponentText), "e%c%02d", Exponent < 0 ? '-' : '+', std::abs(Exponent));
		Text = Digits.substr(0, 1) + (Digits.size() > 1 ? "." + Digits.substr(1) : "") + ExponentText;
	}
	return Value < 0 ? "-" + Text : Text;
}

namespace
{
void AppendDoubles(std::string& Text, const double* Values, int Count)
{
	Text += "[";
	for (int Index = 0; Index < Count; ++Index)
	{
		Text += (Index > 0 ? "," : "") + PythonDouble(Values[Index]);
	}
	Text += "]";
}

void AppendApproach(std::string& Text, const FApproachRecord& Approach)
{
	Text += "{\"id\":" + std::to_string(Approach.Id) + ",\"phase\":" + std::to_string(Approach.Phase)
		+ ",\"way\":" + std::to_string(Approach.Way) + ",\"stop_line\":";
	AppendDoubles(Text, Approach.StopLine, 4);
	Text += ",\"direction\":";
	AppendDoubles(Text, Approach.Direction, 2);
	Text += ",\"lanes\":" + std::to_string(Approach.Lanes) + ",\"speed\":" + PythonDouble(Approach.SpeedKmh) + "}";
}

void AppendJunction(std::string& Text, const FJunctionRecord& Junction)
{
	Text += "{\"id\":" + std::to_string(Junction.Id) + ",\"x\":" + PythonDouble(Junction.X)
		+ ",\"y\":" + PythonDouble(Junction.Y) + ",\"crossing_only\":" + (Junction.bCrossingOnly ? "true" : "false")
		+ ",\"phases\":[";
	for (size_t Index = 0; Index < Junction.Phases.size(); ++Index)
	{
		const FPhaseRecord& Phase = Junction.Phases[Index];
		Text += (Index > 0 ? "," : "") + std::string("{\"green\":") + PythonDouble(Phase.Green)
			+ ",\"amber\":" + PythonDouble(Phase.Amber) + ",\"clearance\":" + PythonDouble(Phase.Clearance)
			+ ",\"pedestrian\":" + (Phase.bPedestrian ? "true" : "false") + "}";
	}
	Text += "],\"approaches\":[";
	for (size_t Index = 0; Index < Junction.Approaches.size(); ++Index)
	{
		Text += Index > 0 ? "," : "";
		AppendApproach(Text, Junction.Approaches[Index]);
	}
	Text += "]}";
}

void AppendSpeedWay(std::string& Text, const FSpeedWay& Way)
{
	Text += "{\"id\":" + std::to_string(Way.Id) + ",\"limit\":" + PythonDouble(Way.LimitKmh)
		+ ",\"width\":" + PythonDouble(Way.Width) + ",\"oneway\":" + (Way.bOneway ? "true" : "false")
		+ ",\"points\":[";
	for (size_t Index = 0; Index < Way.Points.size(); ++Index)
	{
		Text += (Index > 0 ? "," : "") + std::string("[") + PythonDouble(Way.Points[Index].X) + ","
			+ PythonDouble(Way.Points[Index].Y) + "]";
	}
	Text += "]}";
}
}

std::string TrafficJsonText(const FTrafficNetwork& Network)
{
	std::string Text = "{\"junctions\":[";
	for (size_t Index = 0; Index < Network.Junctions.size(); ++Index)
	{
		Text += Index > 0 ? "," : "";
		AppendJunction(Text, Network.Junctions[Index]);
	}
	Text += "],\"speed_ways\":[";
	for (size_t Index = 0; Index < Network.SpeedWays.size(); ++Index)
	{
		Text += Index > 0 ? "," : "";
		AppendSpeedWay(Text, Network.SpeedWays[Index]);
	}
	Text += "]}";
	return Text;
}

bool WriteTrafficJson(const std::string& Path, const FTrafficNetwork& Network)
{
	std::ofstream File(Path, std::ios::binary);
	if (!File)
	{
		return false;
	}
	File << TrafficJsonText(Network);
	return static_cast<bool>(File);
}
}
