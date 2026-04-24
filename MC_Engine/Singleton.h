#pragma once
#include <assert.h>
//! CRTP base class - Curiously Recurring Template Pattern
// - definition : A class inherits from a template that is instantiated with the class itself:
// - “Derived passes itself into Base” is the “curiously recurring” part.
//
// Two usage patterns:
//   1. No-arg:   class Foo : Singleton<Foo> { Foo() = default; };  ->  Foo::Init();  Foo::Get();
//   2. With-arg: class Bar : Singleton<Bar> { Bar(int x) {...}; };  ->  Bar::Init(42);  Bar::Get();
//
// Init() must be called exactly once before Get(). Asserts fire on misuse.
template <typename T>
class Singleton {
    inline static T* sInstance = nullptr;
public:
    Singleton(const Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton(Singleton&&) = delete;
    Singleton& operator=(Singleton&&) = delete;

    template <typename... Args>
    static T& Init(Args&&... args) {
        assert(!sInstance && "Singleton already initialized");
        static T instance(std::forward<Args>(args)...);
        sInstance = &instance;
        return instance;
    }

    static T& Get() {
        assert(sInstance && "Call Init() before Get()");
        return *sInstance;
    }

protected:
    Singleton() = default;
    ~Singleton() = default;
};

// example usage
#ifdef aewge23b2GH920np2qevnewpqev // some random string

// no-arg singleton (unchanged call site feel)
class Foo final : public Singleton<Foo> {
    friend class Singleton<Foo>;
private:
    Foo() = default;
public:
    void Goo() {}
};
// Foo::Init();
// Foo::Get().Goo();

// parameterized singleton
class Bar final : public Singleton<Bar> {
    friend class Singleton<Bar>;
    int mX;
    Bar() = delete;
    Bar(int x) : mX(x) {}
public:
    void Hoo() {}
};
// Bar::Init(42);
// Bar::Get().Hoo();

#endif