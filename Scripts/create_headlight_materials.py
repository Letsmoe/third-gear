"""Creates the light functions of the car's headlights in /Game/Vehicles/Materials (headless editor script).

  M_LF_LowBeam   asymmetric European low beam: flat cut-off line 0.57 degrees below the horizon on the oncoming side,
                 kicked up to the right (15 degrees, to 2.2 degrees above the aim) for the kerb and signs, bright
                 hot spot just below the cut-off right of centre, foreground fill. Drawn in degrees from the lamp's axis.
  M_LF_HighBeam  round, far reaching high beam with a soft surround.

A light function receives the lit point in the light's own frame (the Light Vector node: z along the beam, y to the
right, x up), so the pattern follows the lamp and needs no parameters. The light function atlas (128 pixels per light) is
switched off for deferred lighting in DefaultEngine.ini, because it would blur the cut-off line over about 3 degrees.

Run:
  UnrealEditor-Cmd <project>.uproject -run=pythonscript -script=<this file> -unattended -nosplash
"""
import unreal

FOLDER = "/Game/Vehicles/Materials"
mel = unreal.MaterialEditingLibrary

COMMON_HLSL = """
// The engine hands light functions the vector swizzled: z along the beam, y to the right, x up.
float Depth = LightVec.z;
if (Depth < 0.2 * length(LightVec))
{
    return float3(0, 0, 0);
}
float H = degrees(atan2(LightVec.y, Depth));
float V = degrees(atan2(LightVec.x, Depth));
"""

LOW_BEAM_HLSL = COMMON_HLSL + """
float Cutoff = H > 0.0 ? clamp(H * 0.27, 0.0, 2.2) : 0.0;
float BelowCutoff = Cutoff - V;
float Mask = smoothstep(-0.12, 0.3, BelowCutoff);
float HotSpot = exp(-pow((H - 3.0) / 11.0, 2.0)) * (0.30 + 0.70 * exp(-pow((V + 1.3) / 2.6, 2.0)));
float Foreground = 0.05 * exp(-pow(H / 22.0, 2.0)) * exp(-pow((V + 5.0) / 5.0, 2.0));
float Shoulder = 0.35 * exp(-pow((H - 14.0) / 16.0, 2.0)) * exp(-pow((V - Cutoff + 1.0) / 2.0, 2.0));
float Edge = saturate((38.0 - length(float2(H, V))) / 6.0);
float Value = saturate(HotSpot + Foreground + Shoulder) * Mask * Edge;
return float3(Value, Value, Value);
"""

HIGH_BEAM_HLSL = COMMON_HLSL + """
float Core = exp(-pow(H / 9.0, 2.0) - pow((V - 0.2) / 4.0, 2.0));
float Wide = 0.25 * exp(-pow(H / 24.0, 2.0) - pow((V + 3.0) / 10.0, 2.0));
float Edge = saturate((40.0 - length(float2(H, V))) / 6.0);
float Value = saturate(Core + Wide) * Edge;
return float3(Value, Value, Value);
"""


def custom_input(name):
    entry = unreal.CustomInput()
    entry.set_editor_property("input_name", name)
    return entry


def build(name, code):
    path = f"{FOLDER}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)

    light_vector = mel.create_material_expression(material, unreal.MaterialExpressionLightVector, -700, 0)
    custom = mel.create_material_expression(material, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property("code", code)
    custom.set_editor_property("description", "BeamPattern")
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("inputs", [custom_input("LightVec")])
    mel.connect_material_expressions(light_vector, "", custom, "LightVec")
    mel.connect_material_property(custom, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log(f"Created {path}")


build("M_LF_LowBeam", LOW_BEAM_HLSL)
build("M_LF_HighBeam", HIGH_BEAM_HLSL)
