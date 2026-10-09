"""Physically based sky/sun/exposure setup shared by level scripts.

Real-world units so night lighting (street lamps, headlights) works later without re-tuning:
sun in lux, exposure as EV100 (sunny day ~ 14-15, overcast ~ 12, street at night ~ 2-4).

Standalone: UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script="Scripts/world_lighting.py <map>"
replaces the lighting actors of an existing level.
"""
import sys

import unreal

LIGHTING_LABELS = {"Sun", "SkyAtmosphere", "SkyLight", "HeightFog", "PostProcess", "Clouds"}
SUN_LUX = 75000.0
DAY_EV100 = 12.5
# Local exposure contrast (1 = off); see set_local_exposure.
LOCAL_SHADOW_CONTRAST = 0.5
LOCAL_HIGHLIGHT_CONTRAST = 0.7
WHITE_TEMPERATURE = 7000.0


def setup(actor_subsystem, sun_pitch=-38.0, sun_yaw=-40.0):
    def spawn(cls, label, rotation=unreal.Rotator(0, 0, 0)):
        actor = actor_subsystem.spawn_actor_from_class(cls, unreal.Vector(0, 0, 0), rotation)
        actor.set_actor_label(label)
        return actor

    sun = spawn(unreal.DirectionalLight, "Sun", unreal.Rotator(roll=0, pitch=sun_pitch, yaw=sun_yaw))
    sun_c = sun.get_component_by_class(unreal.DirectionalLightComponent)
    sun_c.set_mobility(unreal.ComponentMobility.MOVABLE)
    sun_c.set_editor_property("atmosphere_sun_light", True)
    sun_c.set_editor_property("intensity", SUN_LUX)
    sun_c.set_editor_property("light_source_angle", 0.53)  # real solar disc -> soft-ish shadow edges

    spawn(unreal.SkyAtmosphere, "SkyAtmosphere")
    sky = spawn(unreal.SkyLight, "SkyLight")
    sky_c = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_c.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_c.set_editor_property("real_time_capture", True)

    fog = spawn(unreal.ExponentialHeightFog, "HeightFog")
    fog_c = fog.get_component_by_class(unreal.ExponentialHeightFogComponent)
    fog_c.set_editor_property("fog_density", 0.004)  # light haze; distant city fades instead of cutting off

    ppv = spawn(unreal.PostProcessVolume, "PostProcess")
    ppv.set_editor_property("unbound", True)
    settings = ppv.get_editor_property("settings")
    # Lock exposure (min = max EV100). Auto exposure would undo material brightness and pump in VR.
    settings.set_editor_property("override_auto_exposure_method", True)
    settings.set_editor_property("auto_exposure_method", unreal.AutoExposureMethod.AEM_HISTOGRAM)
    settings.set_editor_property("override_auto_exposure_min_brightness", True)
    settings.set_editor_property("auto_exposure_min_brightness", DAY_EV100)
    settings.set_editor_property("override_auto_exposure_max_brightness", True)
    settings.set_editor_property("auto_exposure_max_brightness", DAY_EV100)
    settings.set_editor_property("override_auto_exposure_bias", True)
    settings.set_editor_property("auto_exposure_bias", 0.0)
    settings.set_editor_property("override_motion_blur_amount", True)
    settings.set_editor_property("motion_blur_amount", 0.0)
    set_local_exposure(settings)
    ppv.set_editor_property("settings", settings)


def set_local_exposure(settings):
    """Compresses the contrast between sun and shade the way the eye does.

    With exposure locked for sunlit surfaces, a street in shade gets an eighth of the light and renders nearly black,
    while the eye (and a phone camera's tone curve) sees it as clearly lit. Local exposure brightens large shaded
    regions and tames large bright ones but keeps the detail inside them.
    """
    values = {
        "local_exposure_method": unreal.LocalExposureMethod.BILATERAL,
        "local_exposure_shadow_contrast_scale": LOCAL_SHADOW_CONTRAST,
        "local_exposure_highlight_contrast_scale": LOCAL_HIGHLIGHT_CONTRAST,
        "local_exposure_detail_strength": 1.0,
        "local_exposure_blurred_luminance_blend": 0.6,
        # Shade is lit by the blue sky; the eye adapts to it, so balance slightly warmer than daylight.
        "white_temp": WHITE_TEMPERATURE,
    }
    for name, value in values.items():
        settings.set_editor_property("override_" + name, True)
        settings.set_editor_property(name, value)


def replace_in_current_level(actor_subsystem):
    for actor in actor_subsystem.get_all_level_actors():
        if actor.get_actor_label() in LIGHTING_LABELS:
            actor_subsystem.destroy_actor(actor)
    setup(actor_subsystem)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if args:
        level_subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        actor_subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
        level_subsystem.load_level(f"/Game/Maps/{args[0]}")
        replace_in_current_level(actor_subsystem)
        level_subsystem.save_current_level()
        unreal.log_warning(f"world_lighting: updated {args[0]}")
