#pragma once
#include <unordered_map>
#include <typeindex>
#include <memory>

class FgBlackboard {
public:
	template<class T> T& Add() {
		auto& slot = mEntries[typeid(T)];
		if (!slot) slot = std::make_unique<Holder<T>>();
		return static_cast<Holder<T>*>(slot.get())->v;
	}
	template<class T> T& Get() { return static_cast<Holder<T>*>(mEntries.at(typeid(T)).get())->v; }
	template <class T> const T& Get() const { return static_cast<Holder<T>*>(mEntries.at(typeid(T)).get())->v; }

	template <class T> T* TryGet() {
		auto it = mEntries.find(typeid(T));
		return it == mEntries.end() ? nullptr : &static_cast<Holder<T>*>(it->second.get())->v;
	}

	template <class T> const T* TryGet() const {
		auto it = mEntries.find(typeid(T));
		return it == mEntries.end() ? nullptr : &static_cast<Holder<T>*>(it->second.get())->v;
	}
	void Clear() { mEntries.clear(); }
private:
	struct IHolder { virtual ~IHolder() = default; };
	template<class T> struct Holder : IHolder { T v{}; };
	std::unordered_map<std::type_index, std::unique_ptr<IHolder>> mEntries;
};