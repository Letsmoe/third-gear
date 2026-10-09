"""Creates /Game/World/MPC_Weather, the material parameter collection UWeatherVisualsSubsystem fills from the weather.

  UnrealEditor-Cmd DrivingGame.uproject -run=pythonscript -script=Scripts/create_weather_parameters.py -unattended

Materials read these to show the weather: wet roads and puddles, snow cover, wind sway, rain on surfaces.
Re-running keeps the asset and only adds missing parameters, so materials keep their references.
"""
import unreal

PATH = "/Game/World"
NAME = "MPC_Weather"
# name -> default; all 0..1 unless noted
SCALARS = {
    "Wetness": 0.0,         # road and surface wetness, rises in rain, dries slowly
    "Puddles": 0.0,         # standing water in hollows, once wetness is high
    "RainIntensity": 0.0,   # current rain, 1 = 10 mm/h or more
    "SnowCover": 0.0,       # lying snow
    "WindSpeed": 0.0,       # m/s including gusts
    "WindDirectionX": 1.0,  # unit vector the wind blows towards, Unreal X east
    "WindDirectionY": 0.0,  # Unreal Y south
    "CloudCover": 0.0,
    "Thunder": 0.0,         # nearest thunderstorm's activity
    "Night": 0.0,           # 1 once the sun is 6 degrees below the horizon: lit windows, lamps
    "LeafDensity": 1.0,     # foliage on deciduous trees: 0 bare (winter) to 1 full (summer)
    "LeafColour": 0.0,      # autumn colouring of the remaining foliage: 0 green to 1 fully turned
    "FallenLeaves": 0.0,    # leaves lying on the ground
    "LeafFall": 0.0,        # leaves falling right now, with the wind
    "TimeOfDay": 12.0,      # local hour, 0 to 24: window lamps switch on and off by it
    "GroundIlluminance": 1.0,  # daylight on level ground relative to the clear 38 degree sun, the light in rooms
}


def main():
    """Creates the collection if needed and adds any missing scalar parameter."""
    full = f"{PATH}/{NAME}"
    collection = unreal.load_asset(full) if unreal.EditorAssetLibrary.does_asset_exist(full) else None
    if collection is None:
        tools = unreal.AssetToolsHelpers.get_asset_tools()
        collection = tools.create_asset(NAME, PATH, unreal.MaterialParameterCollection,
                                        unreal.MaterialParameterCollectionFactoryNew())
    parameters = list(collection.get_editor_property("scalar_parameters"))
    existing = {str(p.get_editor_property("parameter_name")) for p in parameters}
    for name, default in SCALARS.items():
        if name in existing:
            continue
        parameter = unreal.CollectionScalarParameter()
        parameter.set_editor_property("parameter_name", name)
        parameter.set_editor_property("default_value", default)
        parameters.append(parameter)
    collection.set_editor_property("scalar_parameters", parameters)
    unreal.EditorAssetLibrary.save_loaded_asset(collection)
    unreal.log_warning(f"create_weather_parameters: {full} has {len(parameters)} scalars")


main()
