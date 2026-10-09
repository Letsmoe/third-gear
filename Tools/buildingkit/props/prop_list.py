"""The Hunyuan props: prompt for the reference image and the measured real size used to scale the mesh.

size is (width along X, depth along Y, height along Z) in metres. mount is 'ground' (origin at the bottom centre,
front toward -Y) or 'wall' (origin at the bottom-left of the wall face, the back plane on Y = 0, extending to -Y)."""

SUFFIX = ("isolated on a plain white background, studio photograph, soft even lighting, the whole object fully visible "
          "and centred with some margin around it")

PROPS = {
    "wheelie_bin": {
        "prompt": "A photo of a dark grey 240 litre plastic wheelie bin with a closed black lid and two black wheels, "
                  "German household waste bin, three-quarter view from the front, " + SUFFIX,
        "size": (0.58, 0.74, 1.07), "mount": "ground", "material": "Plastic", "triangles": 6000, "seed": 11,
    },
    "bike_stand": {
        "prompt": "A photo of a galvanised steel bicycle parking hoop, a single inverted U shaped tube with two straight "
                  "legs bolted to two small square base plates, street furniture, front view, " + SUFFIX,
        "size": (0.60, 0.10, 0.90), "mount": "ground", "material": "Metal", "triangles": 3500, "seed": 5,
    },
    "letterbox": {
        "prompt": "A photo of a yellow German Deutsche Post free standing letterbox on a short post, a yellow rounded "
                  "metal postbox with a black posting slot and a small collection plate, front three-quarter view, "
                  + SUFFIX,
        "size": (0.34, 0.32, 1.10), "mount": "ground", "material": "Metal", "triangles": 7000, "seed": 3,
    },
    "advertising_column": {
        "prompt": "A photo of a Litfasssaeule, a stout German advertising column, a thick fat cylinder only about two "
                  "and a half times taller than it is wide, covered in layered paper posters, with a conical dark green metal roof and "
                  "a small finial on top, standing on a short dark green plinth, front view, " + SUFFIX,
        "size": (1.20, 1.20, 3.20), "mount": "ground", "material": "Metal", "triangles": 12000, "seed": 21,
    },
    "door_surround": {
        "prompt": "A photo of a tall narrow white stucco plaster door surround from an old town house, taller than wide "
                  "with a ratio of two to three, two slender fluted pilasters with capitals carrying a small cornice "
                  "with a keystone and floral relief, the tall doorway itself left as a plain dark rectangle, straight "
                  "frontal view, symmetrical, " + SUFFIX,
        "size": (1.85, 0.30, 2.90), "mount": "wall", "material": "Plaster", "triangles": 20000, "seed": 8,
    },
    "window_pediment": {
        "prompt": "A photo of a white stucco plaster window pediment ornament, a triangular gable with an egg and dart "
                  "moulding and a central shell relief, mounted flat on a wall, straight frontal view, " + SUFFIX,
        "size": (1.50, 0.22, 0.65), "mount": "wall", "material": "Plaster", "triangles": 12000, "seed": 4,
    },
}
