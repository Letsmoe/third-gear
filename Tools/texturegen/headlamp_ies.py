"""Writes IESNA LM-63 photometric files for a European low beam (ECE R112 class B shape) and a high beam.

Output in <data root>/texturegen/headlamps/ as headlamp_low.ies and headlamp_high.ies, imported as light profiles by
Scripts/import_headlight_profiles.py. Both files are photometric type C. A point on the beam is described by its
horizontal angle H (positive to the right of the lamp axis, the nearside for right-hand traffic) and its vertical angle
V (positive up); the C-plane angle 0 is the right, 90 is up, 180 the left, 270 down, and the vertical angle of the file is
the angle from the lamp axis.

Low beam (one lamp, candela):
  - Cut-off 0.57 degrees below the axis on the oncoming (left) side, a 15 degree step up to 1.0 degree above the axis on
    the right. Under the line the intensity rises within about 0.2 degrees to the hot spot, which sits 0.9 degrees under
    the cut-off and 2.5 degrees right of the axis, 30000 cd, with ECE test point values roughly 75R 0.57D/1.15R about 80 %
    of the peak, 50R 0.86D/1.72R about 60 %, and the glare point B50L (0.57U, 3.43L) a few hundred cd, as R112 limits it to 350.
  - A foreground fill reaches 10 to 20 degrees down and about 40 degrees to each side.
High beam: a centred hot spot of 90000 cd, 5 degrees wide horizontally and 3.5 vertically, on a broad skirt.

Usage: python3 -I Tools/texturegen/headlamp_ies.py
"""
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bootstrap"))
import data_root  # noqa: E402

LOW_PEAK_CANDELA = 30000.0
HIGH_PEAK_CANDELA = 90000.0
CUTOFF_LEFT_DEGREES = -0.57
CUTOFF_RIGHT_DEGREES = 1.0
STEP_SLOPE = math.tan(math.radians(15.0))
GLARE_FLOOR_CANDELA = 120.0


def vertical_angles():
    """Vertical angles from the axis: fine near the cut-off, coarse further out, up to the back hemisphere."""
    angles = [i * 0.1 for i in range(0, 101)]
    angles += [10.0 + i * 0.5 for i in range(1, 41)]
    angles += [30.0 + i * 2.0 for i in range(1, 16)]
    angles += [60.0 + i * 5.0 for i in range(1, 7)]
    angles += [90.0 + i * 10.0 for i in range(1, 10)]
    return angles


def horizontal_angles():
    """C-plane angles, every 2.5 degrees all around (no symmetry assumed by the loader)."""
    return [i * 2.5 for i in range(0, 145)]


def to_h_v(gamma_degrees, plane_degrees):
    """Horizontal and vertical angle of the direction that is gamma off the axis in the given C-plane."""
    gamma = math.radians(gamma_degrees)
    plane = math.radians(plane_degrees)
    forward = math.cos(gamma)
    right = math.sin(gamma) * math.cos(plane)
    up = math.sin(gamma) * math.sin(plane)
    if forward <= 0.0:
        return None
    return math.degrees(math.atan2(right, forward)), math.degrees(math.asin(max(-1.0, min(1.0, up))))


def cutoff_height(horizontal):
    """Vertical angle of the cut-off line at a horizontal angle: flat on the left, 15 degree rise, flat higher on the right."""
    rising = CUTOFF_LEFT_DEGREES + STEP_SLOPE * max(0.0, horizontal)
    return min(rising, CUTOFF_RIGHT_DEGREES)


def smooth_edge(distance_below, softness=0.12):
    """0 on the line, 1 a little under it; the optics blur the edge over a few tenths of a degree."""
    return 1.0 / (1.0 + math.exp(-(distance_below - 2.0 * softness) / (0.5 * softness)))


def asymmetric_gauss(x, centre, sigma_left, sigma_right):
    sigma = sigma_left if x < centre else sigma_right
    return math.exp(-0.5 * ((x - centre) / sigma) ** 2)


def low_beam_candela(horizontal, vertical):
    """Relative low beam intensity (1 = hot spot) at the given angles."""
    cutoff = cutoff_height(horizontal)
    below = cutoff - vertical
    if below <= 0.0:
        scatter = math.exp(-(-below) / 1.2) * asymmetric_gauss(horizontal, 0.0, 12.0, 14.0)
        return GLARE_FLOOR_CANDELA / LOW_PEAK_CANDELA * scatter * smooth_edge(-below + 0.3) + 0.0
    spot_depth = 0.9
    vertical_core = math.exp(-0.5 * ((below - spot_depth) / (0.9 if below > spot_depth else 0.45)) ** 2)
    # The hot spot is tied to the cut-off on the left half and follows the rise on the right half.
    horizontal_core = asymmetric_gauss(horizontal, 2.5, 6.0, 8.0)
    core = vertical_core * horizontal_core
    fill = 0.20 * math.exp(-below / 3.2) * asymmetric_gauss(horizontal, 1.0, 14.0, 17.0)
    wide = 0.05 * math.exp(-below / 8.0) * asymmetric_gauss(horizontal, 0.0, 28.0, 30.0)
    intensity = (core + fill + wide) * smooth_edge(below)
    return max(intensity, GLARE_FLOOR_CANDELA / LOW_PEAK_CANDELA)


def high_beam_candela(horizontal, vertical):
    """Relative high beam intensity (1 = hot spot) at the given angles."""
    core = math.exp(-0.5 * ((horizontal / 5.0) ** 2 + ((vertical - 0.3) / 3.5) ** 2))
    skirt = 0.06 * math.exp(-0.5 * ((horizontal / 14.0) ** 2 + (vertical / 9.0) ** 2))
    return core + skirt


def beam_value(profile, peak_candela, gamma, plane):
    angles = to_h_v(gamma, plane)
    if angles is None:
        return 0.0
    return peak_candela * profile(angles[0], angles[1])


def normalised(profile):
    """The profile divided by its maximum over the sampled grid, so the peak candela is exact."""
    maximum = 0.0
    for gamma in vertical_angles():
        for plane in horizontal_angles():
            angles = to_h_v(gamma, plane)
            if angles is not None:
                maximum = max(maximum, profile(angles[0], angles[1]))
    return lambda horizontal, vertical: profile(horizontal, vertical) / maximum


def write_ies(path, title, profile, peak_candela):
    """Writes one photometric type C file: for each C-plane the candela along the vertical angles."""
    gammas = vertical_angles()
    planes = horizontal_angles()
    with open(path, "w") as handle:
        handle.write("IESNA:LM-63-2002\n")
        handle.write("[TEST] third-gear synthetic ECE R112 style profile\n")
        handle.write("[MANUFAC] Third Gear\n")
        handle.write(f"[LUMINAIRE] {title}\n")
        handle.write("TILT=NONE\n")
        # lamps, lumens (-1 = absolute photometry), multiplier, vertical count, horizontal count, type C, metres, opening
        handle.write(f"1 -1 1 {len(gammas)} {len(planes)} 1 2 0.08 0.04 0.0\n")
        handle.write("1.0 1.0 40.0\n")
        handle.write(" ".join(f"{gamma:g}" for gamma in gammas) + "\n")
        handle.write(" ".join(f"{plane:g}" for plane in planes) + "\n")
        for plane in planes:
            values = [beam_value(profile, peak_candela, gamma, plane) for gamma in gammas]
            handle.write(" ".join(f"{value:.1f}" for value in values) + "\n")


NORMALISED_LOW = normalised(low_beam_candela)
NORMALISED_HIGH = normalised(high_beam_candela)


def print_test_points():
    """Prints the intensity at ECE test points, to compare with the R112 table."""
    points = {"HV (0,0)": (0.0, 0.0), "75R (0.57D,1.15R)": (1.15, -0.57), "50R (0.86D,1.72R)": (1.72, -0.86),
              "50V (0.86D,0)": (0.0, -0.86), "25L (1.72D,9L)": (-9.0, -1.72), "B50L (0.57U,3.43L)": (-3.43, 0.57),
              "hot spot (0.9 under, 2.5R)": (2.5, -1.47), "step right (0.2U, 6R)": (6.0, 0.2)}
    for name, (horizontal, vertical) in points.items():
        print(f"low  {name:28s} {LOW_PEAK_CANDELA * NORMALISED_LOW(horizontal, vertical):9.0f} cd")
    print(f"high HV {HIGH_PEAK_CANDELA * NORMALISED_HIGH(0.0, 0.0):9.0f} cd")


def main():
    out_dir = os.path.join(str(data_root.data_root()), "texturegen", "headlamps")
    os.makedirs(out_dir, exist_ok=True)
    write_ies(os.path.join(out_dir, "headlamp_low.ies"), "Low beam", NORMALISED_LOW, LOW_PEAK_CANDELA)
    write_ies(os.path.join(out_dir, "headlamp_high.ies"), "High beam", NORMALISED_HIGH, HIGH_PEAK_CANDELA)
    print("wrote", out_dir)
    print_test_points()


if __name__ == "__main__":
    main()
