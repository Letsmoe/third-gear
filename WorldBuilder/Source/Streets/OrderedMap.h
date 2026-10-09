#pragma once

#include <utility>
#include <vector>

namespace WorldBuilder
{
/**
 * A small map that iterates in insertion order, like a Python dict. The street model keeps the lines of a road in it:
 * a handful of entries, so a linear search is the fastest lookup, and the order in which the lines were added decides
 * the order of the painted output.
 */
template <typename KeyType, typename ValueType>
class TOrderedMap
{
public:
	using FEntry = std::pair<KeyType, ValueType>;

	/** The value of a key, or nullptr when it is missing. */
	const ValueType* Find(const KeyType& Key) const
	{
		for (const FEntry& Entry : Entries)
		{
			if (Entry.first == Key)
			{
				return &Entry.second;
			}
		}
		return nullptr;
	}

	ValueType* Find(const KeyType& Key)
	{
		for (FEntry& Entry : Entries)
		{
			if (Entry.first == Key)
			{
				return &Entry.second;
			}
		}
		return nullptr;
	}

	bool Contains(const KeyType& Key) const
	{
		return Find(Key) != nullptr;
	}

	/** The value of a key, or the default when it is missing. */
	ValueType ValueOr(const KeyType& Key, const ValueType& Default) const
	{
		const ValueType* Found = Find(Key);
		if (Found == nullptr)
		{
			return Default;
		}
		return *Found;
	}

	/** Sets the value of a key; a key that is new goes to the end, an existing one keeps its place. */
	void Set(const KeyType& Key, const ValueType& Value)
	{
		ValueType* Found = Find(Key);
		if (Found != nullptr)
		{
			*Found = Value;
			return;
		}
		Entries.emplace_back(Key, Value);
	}

	/** The value of a key; a key that is missing is added with a default value first. */
	ValueType& operator[](const KeyType& Key)
	{
		ValueType* Found = Find(Key);
		if (Found != nullptr)
		{
			return *Found;
		}
		Entries.emplace_back(Key, ValueType());
		return Entries.back().second;
	}

	size_t Size() const { return Entries.size(); }
	bool IsEmpty() const { return Entries.empty(); }
	auto begin() const { return Entries.begin(); }
	auto end() const { return Entries.end(); }
	auto begin() { return Entries.begin(); }
	auto end() { return Entries.end(); }

private:
	std::vector<FEntry> Entries;
};
}
