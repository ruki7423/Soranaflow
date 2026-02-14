#pragma once

#include <QHash>
#include <QString>
#include <typeinfo>
#include <cassert>

// Lightweight service locator for dependency injection.
//
// Transitional pattern: wraps existing singletons so that tests can
// substitute mock implementations without changing production code.
//
// Usage (production — registered once in main.cpp):
//   ServiceLocator::provide<Settings>(Settings::instance());
//   ServiceLocator::provide<AudioEngine>(AudioEngine::instance());
//
// Usage (consumer — no change from singleton pattern):
//   auto* settings = ServiceLocator::get<Settings>();
//
// Usage (test — override with mock):
//   MockSettings mock;
//   ServiceLocator::provide<Settings>(&mock);
//   // ... run test ...
//   ServiceLocator::reset();
//
// Thread safety: register during startup (single-threaded), then
// read-only during normal operation.  Not safe for concurrent writes.
class ServiceLocator {
public:
    // Register a service instance.  Ownership is NOT transferred —
    // caller (or the singleton pattern) manages lifetime.
    template<typename T>
    static void provide(T* instance)
    {
        registry()[key<T>()] = static_cast<void*>(instance);
    }

    // Retrieve a registered service.  Returns nullptr if not registered.
    template<typename T>
    static T* get()
    {
        auto it = registry().find(key<T>());
        if (it != registry().end())
            return static_cast<T*>(it.value());
        return nullptr;
    }

    // Remove a single service registration.
    template<typename T>
    static void remove()
    {
        registry().remove(key<T>());
    }

    // Clear all registrations (for test teardown).
    static void reset()
    {
        registry().clear();
    }

private:
    template<typename T>
    static QString key()
    {
        return QString::fromLatin1(typeid(T).name());
    }

    static QHash<QString, void*>& registry()
    {
        static QHash<QString, void*> s_registry;
        return s_registry;
    }
};
