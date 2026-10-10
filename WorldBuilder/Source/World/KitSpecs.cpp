#include "KitSpecs.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace WorldBuilder
{
FKitSpecs::FKitSpecs(const std::string& Folder)
{
	std::error_code Error;
	for (const auto& Entry : std::filesystem::directory_iterator(Folder, Error))
	{
		const std::string Name = Entry.path().filename().string();
		if (Name.rfind("kit_spec_", 0) != 0 || Entry.path().extension() != ".txt")
		{
			continue;
		}
		std::ifstream Input(Entry.path());
		std::string Line;
		while (std::getline(Input, Line))
		{
			const size_t Space = Line.find(' ');
			if (Space == std::string::npos)
			{
				continue;
			}
			Values[Line.substr(0, Space)] = Line.substr(Space + 1);
		}
	}
}

bool FKitSpecs::Has(const std::string& Key) const
{
	return Values.count(Key) > 0;
}

std::vector<double> FKitSpecs::Numbers(const std::string& Key) const
{
	std::vector<double> Result;
	const auto Found = Values.find(Key);
	if (Found == Values.end())
	{
		return Result;
	}
	std::istringstream Stream(Found->second);
	double Value = 0.0;
	while (Stream >> Value)
	{
		Result.push_back(Value);
	}
	return Result;
}

double FKitSpecs::Number(const std::string& Key, double Default) const
{
	const std::vector<double> Result = Numbers(Key);
	return Result.empty() ? Default : Result[0];
}

std::string FKitSpecs::Text(const std::string& Key) const
{
	const auto Found = Values.find(Key);
	return Found == Values.end() ? std::string() : Found->second;
}

int FKitSpecs::ListSize(const std::string& Key) const
{
	int Size = 0;
	while (true)
	{
		const std::string Prefix = Key + "." + std::to_string(Size);
		bool bFound = Values.count(Prefix) > 0;
		if (!bFound)
		{
			const std::string Child = Prefix + ".";
			for (const auto& [Name, Value] : Values)
			{
				if (Name.compare(0, Child.size(), Child) == 0)
				{
					bFound = true;
					break;
				}
			}
		}
		if (!bFound)
		{
			return Size;
		}
		++Size;
	}
}
}
