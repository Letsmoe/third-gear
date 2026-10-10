#pragma once

#include <string>
#include <unordered_map>
#include <vector>

/**
 * The specs of the building kit's structure families (Tools/buildingkit/kit/flat_spec.py writes them to
 * <data root>/building_kit/kit_spec_<family>.txt): sizes, sockets and positions the placement needs, by key path
 * ("pylons.Donau_Suspension.height", "landmarks.Elbphilharmonie.origin_utm32").
 */
namespace WorldBuilder
{
class FKitSpecs
{
public:
	/** Reads every kit_spec_*.txt in the folder; a missing folder gives empty specs. */
	explicit FKitSpecs(const std::string& Folder);

	bool Has(const std::string& Key) const;

	/** The numbers of a key; empty when the key is missing or not numeric. */
	std::vector<double> Numbers(const std::string& Key) const;

	/** The first number of a key, or Default. */
	double Number(const std::string& Key, double Default) const;

	/** The text of a key, or an empty string. */
	std::string Text(const std::string& Key) const;

	/** How many numbered children a list key has ("pylons.Donau_Anchor.sockets" has .0, .1 ...). */
	int ListSize(const std::string& Key) const;

private:
	std::unordered_map<std::string, std::string> Values;
};
}
