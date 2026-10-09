"""Prompt list for the world and weather sounds. Seeds are fixed so any take can be regenerated exactly.

Each entry: id, kind ("loop" is made seamless, "oneshot" is trimmed and faded), seconds to generate, seed, prompt.
Engine, tyre, wind-in-the-car and the small mechanical sounds are not generated: Stable Audio makes poor steady
pitched sounds, so those are synthesised in C++ (Source/DrivingGame/CarSoundDsp.cpp).
"""

NEGATIVE_PROMPT = (
    "music, melody, instruments, drums, singing, speech, voices, talking, laughter, hum, distortion, "
    "low quality, noise gate, loud sudden sounds, clipping"
)

SUFFIX = ", field recording, natural ambience, stereo, no music, no speech"

SOUNDS = [
    # Wind in trees outside, heard through a closed car (the game low-passes it).
    dict(id="wind_trees_a", kind="loop", seconds=44.0, seed=1101,
         prompt="steady wind blowing through the leaves of tall deciduous trees along a street, soft leaf rustle with slow gusts"),
    dict(id="wind_trees_b", kind="loop", seconds=44.0, seed=1102,
         prompt="strong gusty wind in a large oak tree, branches creaking slightly, leaves rushing, rising and falling gusts"),
    # Town: distant traffic by day and by night.
    dict(id="town_day", kind="loop", seconds=44.0, seed=1201,
         prompt="distant city traffic hum heard from a quiet residential street, far away cars, muffled urban background, no close vehicles, no horns"),
    dict(id="town_night", kind="loop", seconds=44.0, seed=1202,
         prompt="quiet town at night, very faint distant road traffic far away, calm empty street, occasional distant car passing"),
    # Birds in daytime, two takes so the loops do not repeat together.
    dict(id="birds_day_a", kind="loop", seconds=44.0, seed=1301,
         prompt="blackbird and sparrows singing in a suburban garden in the morning, sparse songbirds, quiet air between calls"),
    dict(id="birds_day_b", kind="loop", seconds=44.0, seed=1302,
         prompt="city park birdsong, a few songbirds chirping and tweeting, wood pigeon cooing in the distance, calm daytime"),
    dict(id="birds_day_c", kind="loop", seconds=44.0, seed=1303,
         prompt="a single songbird singing a melodic song on a quiet street, other birds answering far away, calm spring morning, mostly quiet air"),
    dict(id="birds_day_d", kind="loop", seconds=44.0, seed=1304,
         prompt="sparrows chirping in a hedge and a robin singing, natural irregular birdsong in a small town, relaxed daytime"),
    # Rain heard from inside the car: roof, glass, and the road outside.
    dict(id="rain_roof_light", kind="loop", seconds=44.0, seed=1401,
         prompt="light rain drizzle falling on a car roof, soft sparse drops tapping on sheet metal, heard from inside the car"),
    dict(id="rain_roof_heavy", kind="loop", seconds=44.0, seed=1402,
         prompt="heavy rain pouring on a car roof, dense loud drumming on sheet metal, heard from inside the car"),
    dict(id="rain_glass_light", kind="loop", seconds=44.0, seed=1411,
         prompt="light rain pattering on a car windscreen glass, fine drops on glass, heard from inside the car"),
    dict(id="rain_glass_heavy", kind="loop", seconds=44.0, seed=1412,
         prompt="heavy rain hitting a car windscreen, dense drops hammering on glass, heard from inside the car"),
    dict(id="rain_road_light", kind="loop", seconds=44.0, seed=1421,
         prompt="light steady rain falling on a wet asphalt street, soft broadband hiss of drizzle, outdoors"),
    dict(id="rain_road_heavy", kind="loop", seconds=44.0, seed=1422,
         prompt="heavy rainfall on a wet road, loud broadband hiss of downpour with splashing, outdoors"),
    # Thunder, near to far. The game picks one by distance and delays it by distance / 343 m/s.
    dict(id="thunder_close_a", kind="oneshot", seconds=16.0, seed=1501,
         prompt="very close thunder clap, sharp loud crack followed by a deep rolling rumble, thunderstorm"),
    dict(id="thunder_close_b", kind="oneshot", seconds=16.0, seed=1502,
         prompt="nearby lightning strike thunder, sudden loud crack then long booming roll, thunderstorm"),
    dict(id="thunder_mid_a", kind="oneshot", seconds=18.0, seed=1511,
         prompt="thunder at medium distance, rumbling roll with a soft crack at the start, thunderstorm"),
    dict(id="thunder_mid_b", kind="oneshot", seconds=18.0, seed=1512,
         prompt="rolling thunder several kilometres away, deep rumble that swells and fades, thunderstorm"),
    dict(id="thunder_mid_c", kind="oneshot", seconds=18.0, seed=1513,
         prompt="thunder rumbling across the sky, deep growl that rolls and fades away, thunderstorm over a town"),
    dict(id="thunder_mid_d", kind="oneshot", seconds=18.0, seed=1514,
         prompt="a long rolling peal of thunder from a storm some kilometres away, low and powerful, no rain"),
    dict(id="thunder_far_a", kind="oneshot", seconds=20.0, seed=1521,
         prompt="distant thunder, low soft rumble far away on the horizon, no crack, thunderstorm"),
    dict(id="thunder_far_b", kind="oneshot", seconds=20.0, seed=1522,
         prompt="faraway thunder, long deep muffled rolling rumble, summer storm in the distance"),
]
