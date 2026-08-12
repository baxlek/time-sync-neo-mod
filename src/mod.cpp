#include "mods/hook.hpp"
#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/hook.h"
#include "mods/svc/log.h"
#include "mods/svc/ui.h"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "d/d_kankyo_static.h"

#include <chrono>
#include <cstring>
#include <ctime>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

DEFINE_HOOK(&dScnKy_env_light_c::setDaytime, SetDaytime);

static ConfigVarHandle g_cvar_enabled = 0;

static bool is_mod_enabled() {
    bool enabled = true;
    if (g_cvar_enabled != 0 &&
        svc_config->get_bool(mod_ctx, g_cvar_enabled, &enabled) == MOD_OK)
    {
        return enabled;
    }
    return true;
}

static bool should_sync_time(dScnKy_env_light_c* env_light) {
    if (dKy_darkworld_check()) {
        return false;
    }

    const bool normal_time_progresses =
        !env_light->field_0x130a;
    return normal_time_progresses;
}

static void on_set_daytime_post(ModContext*, void* args, void*, void*) {
    dScnKy_env_light_c* env_light = mods::arg<dScnKy_env_light_c*>(args, 0);
    if (env_light == nullptr || !is_mod_enabled()) {
        return;
    }

    dComIfGp_roomControl_setTimePass(0);
    if (!should_sync_time(env_light)) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time {};
#if defined(_WIN32)
    localtime_s(&local_time, &now_time);
#else
    localtime_r(&now_time, &local_time);
#endif

    const f32 calendar_daytime = local_time.tm_hour * 15.0f +
                                 local_time.tm_min * (15.0f / 60.0f) +
                                 local_time.tm_sec * (15.0f / 3600.0f);

    f32 diff_daytime = calendar_daytime - env_light->daytime;
    if (diff_daytime < 0.0f) {
        diff_daytime += 360.0f;
    }

    // True when game time slightly overshot device time (game is ahead by less than 2 degrees
    // in the absolute sense, ruling out the midnight-crossing case where the raw difference
    // would be negative and large).
    const bool game_slightly_ahead = env_light->daytime > calendar_daytime &&
                                     env_light->daytime - calendar_daytime < 2.0f;

    // True when the game just crossed midnight but the device hasn't yet: game is within
    // 2 degrees past midnight while the device is within 2 degrees before midnight.
    // Without this check the hook would advance game time through a full 360-degree cycle
    // chasing the large numeric gap, causing a visible full-day spin before stopping.
    const bool game_just_past_midnight = env_light->daytime < 2.0f && calendar_daytime > 358.0f;

    if (game_slightly_ahead || game_just_past_midnight ||
        (diff_daytime <= 1.0f && env_light->daytime <= calendar_daytime)) {
        // Game is at or just past the target; snap to device time.
        // If the game crossed midnight before the device did, revert that crossing so mDate
        // stays consistent with the pre-midnight position. The natural game tick will re-cross
        // midnight (and re-increment mDate) once the device also passes midnight.
        if (game_just_past_midnight) {
            env_light->mDate--;
            dComIfGs_setDate(env_light->mDate);
        }
        env_light->daytime = calendar_daytime;
    } else {
        // Game is behind the target; advance by one step and wrap to [0, 360).
        env_light->daytime += 1.0f;
        if (env_light->daytime >= 360.0f) {
            env_light->daytime -= 360.0f;
            // Crossed midnight during catch-up; keep mDate and the day-flag in sync.
            env_light->mDate++;
            dComIfGs_setDate(env_light->mDate);
            dKankyo_DayProc();
        }
    }

    dComIfGs_setTime(env_light->daytime);
}

static ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = UI_CONTROL_TOGGLE;
    control.label = "Enabled";
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = g_cvar_enabled;
    return svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);
}

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    ConfigVarDesc enabled_desc = CONFIG_VAR_DESC_INIT;
    enabled_desc.name = "modEnabled";
    enabled_desc.type = CONFIG_VAR_BOOL;
    enabled_desc.default_bool = true;

    ModResult result = svc_config->register_var(mod_ctx, &enabled_desc, &g_cvar_enabled);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to register enabled cvar");
        return result;
    }

    UiModsPanelDesc panel_desc = UI_MODS_PANEL_DESC_INIT;
    panel_desc.build = build_panel;
    result = svc_ui->register_mods_panel(mod_ctx, &panel_desc);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to register mod panel");
        return result;
    }

    result = mods::hook_add_post<SetDaytime>(svc_hook, on_set_daytime_post);
    if (result != MOD_OK) {
        svc_log->error(mod_ctx, "failed to install on_set_daytime_post");
        return result;
    }

    svc_log->info(mod_ctx, "time_sync_neo initialized");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    svc_log->info(mod_ctx, "time_sync_neo shutdown");
    return MOD_OK;
}
}
