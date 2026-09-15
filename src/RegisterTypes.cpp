
#include "Libretro.hpp"
#include "RetroAchievements.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>

#if defined(__ANDROID__) && defined(__aarch64__)
#include <android/api-level.h>
#include <malloc.h>
#if __ANDROID_API__ < 26
// <malloc.h> declares it only from API 26.
extern "C" int mallopt(int, int) __attribute__((weak));
#endif
#endif

using namespace godot;

namespace
{
Xenu::RetroAchievements* g_retro_achievements = nullptr;
}

void initialize(ModuleInitializationLevel p_level)
{
    if (p_level != ModuleInitializationLevel::MODULE_INITIALIZATION_LEVEL_SCENE)
        return;

#if defined(__ANDROID__) && defined(__aarch64__)
    // Cores match signal fault addresses, which arrive without the heap tag,
    // against memory they malloc'd.
    if (mallopt != nullptr && android_get_device_api_level() >= 31)
        mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
#endif

    ClassDB::register_class<Xenu::LibretroOptionCategory>();
    ClassDB::register_class<Xenu::LibretroOptionValue>();
    ClassDB::register_class<Xenu::LibretroOptionDefinition>();
    ClassDB::register_runtime_class<Xenu::Libretro>();

    // One per process, not one per Libretro node: the player signs in from the
    // menu with no game running, and RetroAchievements tracks a single game
    // session per user however many cabinets are powered on.
    ClassDB::register_class<Xenu::RetroAchievements>();
    g_retro_achievements = memnew(Xenu::RetroAchievements);
    Engine::get_singleton()->register_singleton("RetroAchievements", g_retro_achievements);
}

void uninitialize(ModuleInitializationLevel p_level)
{
    if (p_level != ModuleInitializationLevel::MODULE_INITIALIZATION_LEVEL_SCENE)
        return;

    if (g_retro_achievements)
    {
        Engine::get_singleton()->unregister_singleton("RetroAchievements");
        memdelete(g_retro_achievements);
        g_retro_achievements = nullptr;
    }
}

extern "C"
{
GDExtensionBool GDE_EXPORT libretro_godot_library_init(GDExtensionInterfaceGetProcAddress p_gde_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization)
{
    godot::GDExtensionBinding::InitObject init_obj(p_gde_get_proc_address, p_library, r_initialization);

    init_obj.register_initializer(initialize);
    init_obj.register_terminator(uninitialize);
    init_obj.set_minimum_library_initialization_level(godot::ModuleInitializationLevel::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init_obj.init();
}
}
