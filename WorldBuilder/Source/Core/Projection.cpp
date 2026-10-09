#include "Projection.h"

#include <array>
#include <cmath>
#include <numbers>

namespace WorldBuilder
{
namespace
{
/** GRS80, the ellipsoid of ETRS89. */
constexpr double SemiMajorAxis = 6378137.0;
constexpr double Flattening = 1.0 / 298.257222101;
/** UTM zone 32: central meridian, scale on it and false easting. */
constexpr double CentralMeridianDegrees = 9.0;
constexpr double ScaleFactor = 0.9996;
constexpr double FalseEasting = 500000.0;

/** The constants of the Krüger series, which depend only on the ellipsoid. */
struct FKruegerSeries
{
	double RectifyingRadius = 0.0;
	double Eccentricity = 0.0;
	std::array<double, 6> Alpha{};
	/** The coefficients of the inverse series. */
	std::array<double, 6> Beta{};
};

FKruegerSeries MakeSeries()
{
	const double N = Flattening / (2.0 - Flattening);
	const double N2 = N * N, N3 = N2 * N, N4 = N3 * N, N5 = N4 * N, N6 = N5 * N;
	FKruegerSeries Series;
	Series.RectifyingRadius = SemiMajorAxis / (1.0 + N) * (1.0 + N2 / 4.0 + N4 / 64.0 + N6 / 256.0);
	Series.Eccentricity = 2.0 * std::sqrt(N) / (1.0 + N);
	Series.Alpha = {
		N / 2.0 - 2.0 * N2 / 3.0 + 5.0 * N3 / 16.0 + 41.0 * N4 / 180.0 - 127.0 * N5 / 288.0 + 7891.0 * N6 / 37800.0,
		13.0 * N2 / 48.0 - 3.0 * N3 / 5.0 + 557.0 * N4 / 1440.0 + 281.0 * N5 / 630.0 - 1983433.0 * N6 / 1935360.0,
		61.0 * N3 / 240.0 - 103.0 * N4 / 140.0 + 15061.0 * N5 / 26880.0 + 167603.0 * N6 / 181440.0,
		49561.0 * N4 / 161280.0 - 179.0 * N5 / 168.0 + 6601661.0 * N6 / 7257600.0,
		34729.0 * N5 / 80640.0 - 3418889.0 * N6 / 1995840.0,
		212378941.0 * N6 / 319334400.0,
	};
	Series.Beta = {
		N / 2.0 - 2.0 * N2 / 3.0 + 37.0 * N3 / 96.0 - N4 / 360.0 - 81.0 * N5 / 512.0 + 96199.0 * N6 / 604800.0,
		N2 / 48.0 + N3 / 15.0 - 437.0 * N4 / 1440.0 + 46.0 * N5 / 105.0 - 1118711.0 * N6 / 3870720.0,
		17.0 * N3 / 480.0 - 37.0 * N4 / 840.0 - 209.0 * N5 / 4480.0 + 5569.0 * N6 / 90720.0,
		4397.0 * N4 / 161280.0 - 11.0 * N5 / 504.0 - 830251.0 * N6 / 7257600.0,
		4583.0 * N5 / 161280.0 - 108847.0 * N6 / 3991680.0,
		20648693.0 * N6 / 638668800.0,
	};
	return Series;
}

const FKruegerSeries Series = MakeSeries();
constexpr double DegreesToRadians = std::numbers::pi / 180.0;

/** The tangent of the geodetic latitude from the tangent of the conformal one, by Newton's method (Karney 2011). */
double GeodeticTangent(double ConformalTangent)
{
	const double E = Series.Eccentricity;
	const double OneMinusESquared = 1.0 - E * E;
	double Tau = ConformalTangent;
	for (int Iteration = 0; Iteration < 5; ++Iteration)
	{
		const double Sigma = std::sinh(E * std::atanh(E * Tau / std::sqrt(1.0 + Tau * Tau)));
		const double Estimate = Tau * std::sqrt(1.0 + Sigma * Sigma) - Sigma * std::sqrt(1.0 + Tau * Tau);
		const double Step = (ConformalTangent - Estimate) / std::sqrt(1.0 + Estimate * Estimate)
			* (1.0 + OneMinusESquared * Tau * Tau) / (OneMinusESquared * std::sqrt(1.0 + Tau * Tau));
		Tau += Step;
		if (std::abs(Step) < 1e-14)
		{
			break;
		}
	}
	return Tau;
}
}

FUtmPoint LonLatToUtm(double Longitude, double Latitude)
{
	const double Phi = Latitude * DegreesToRadians;
	const double Lambda = (Longitude - CentralMeridianDegrees) * DegreesToRadians;
	const double SinPhi = std::sin(Phi);
	// Conformal latitude, as its tangent.
	const double Tau = std::sinh(std::atanh(SinPhi) - Series.Eccentricity * std::atanh(Series.Eccentricity * SinPhi));
	const double XiPrime = std::atan2(Tau, std::cos(Lambda));
	const double EtaPrime = std::atanh(std::sin(Lambda) / std::sqrt(1.0 + Tau * Tau));

	double Xi = XiPrime;
	double Eta = EtaPrime;
	for (int Order = 1; Order <= 6; ++Order)
	{
		const double Alpha = Series.Alpha[Order - 1];
		Xi += Alpha * std::sin(2.0 * Order * XiPrime) * std::cosh(2.0 * Order * EtaPrime);
		Eta += Alpha * std::cos(2.0 * Order * XiPrime) * std::sinh(2.0 * Order * EtaPrime);
	}
	return {FalseEasting + ScaleFactor * Series.RectifyingRadius * Eta, ScaleFactor * Series.RectifyingRadius * Xi};
}

FLonLat UtmToLonLat(double East, double North)
{
	const double Xi = North / (ScaleFactor * Series.RectifyingRadius);
	const double Eta = (East - FalseEasting) / (ScaleFactor * Series.RectifyingRadius);
	double XiPrime = Xi;
	double EtaPrime = Eta;
	for (int Order = 1; Order <= 6; ++Order)
	{
		const double Beta = Series.Beta[Order - 1];
		XiPrime -= Beta * std::sin(2.0 * Order * Xi) * std::cosh(2.0 * Order * Eta);
		EtaPrime -= Beta * std::cos(2.0 * Order * Xi) * std::sinh(2.0 * Order * Eta);
	}
	const double SinhEta = std::sinh(EtaPrime);
	const double CosXi = std::cos(XiPrime);
	const double ConformalTangent = std::sin(XiPrime) / std::sqrt(SinhEta * SinhEta + CosXi * CosXi);
	const double Latitude = std::atan(GeodeticTangent(ConformalTangent));
	const double Longitude = std::atan2(SinhEta, CosXi);
	return {CentralMeridianDegrees + Longitude / DegreesToRadians, Latitude / DegreesToRadians};
}

FWorldPoint LonLatToWorld(double Longitude, double Latitude)
{
	const FUtmPoint Utm = LonLatToUtm(Longitude, Latitude);
	return {Utm.East - OriginEast, OriginNorth - Utm.North};
}
}
